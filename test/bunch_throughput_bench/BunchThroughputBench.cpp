#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/basic/CommonFlowUtils.hpp>
#include <tm_kit/basic/IntIDComponent.hpp>
#include <tm_kit/basic/real_time_clock/ClockComponent.hpp>
#include <tm_kit/basic/SpdLoggingComponent.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

//Throughput comparison of the queue container used by the self-threaded "bunch"
//node (CommonFlowUtilComponents::bunch / bunchWithProcessing): how fast the node
//aggregates a stream of T into vector<T> batches when its incoming / processing
//queues are std::list (original), std::deque (current production) or std::vector
//(double-buffer alternative). "prod" runs the real production nodes instead of
//the self-contained replica. Not a test: run it by hand, pinned to idle cores
//(same advice as simple_graph_bench).
//
//Two payloads are measured: "small" (8 bytes) and "big" (64 bytes). This is the
//point of the comparison: the payload T is moved, never copied, by list/deque,
//but std::vector reallocates its buffer by COPYING InnerData<T> (its move is not
//noexcept), so the vector variant is only competitive for cheap-to-copy payloads
//and cannot compile for move-only payloads at all. The sink counts the aggregated
//items and verifies they arrive in order with no loss/duplication.
//
//Columns: wall/cpu ns per pushed item, and thousands of items/s.

using namespace dev::cd606::tm;

namespace {

    using Clock = basic::TimeComponentEnhancedWithSpdLogging<basic::real_time_clock::ClockComponent, false>;
    using Env = infra::Environment<
        infra::CheckTimeComponent<false>, infra::TrivialExitControlComponent
        , basic::IntIDComponent<>, Clock>;
    using M = infra::RealTimeApp<Env>;
    using R = infra::AppRunner<M>;
    using CFU = basic::CommonFlowUtilComponents<M>;

    //8-byte and 64-byte payloads; .seq is the order-check key.
    struct SmallPayload { int64_t seq; };
    struct BigPayload { int64_t seq; int64_t a, b, c, d, e, f, g; };
    template <class T>
    inline int64_t seqOf(T const &v) { return v.seq; }

    //Identity buncher so bunchWithProcessing<T> has the same output as bunch<T>.
    template <class T>
    struct IdentityBuncher {
        std::vector<T> operator()(std::vector<T> &&v) { return std::move(v); }
    };

    template <class T>
    class PushSource final : public M::template AbstractImporter<T> {
    public:
        virtual void start(Env *) override final {}
        inline void push(Env *env, T &&v) {
            this->publish(M::template pureInnerData<T>(env, std::move(v)));
        }
    };

    //Counts the aggregated items and checks that values arrive strictly in
    //order with no gaps/duplicates. handle() runs in the bunch node's thread, so
    //nextExpected is touched by one thread only; items/bad are polled by main.
    template <class T>
    struct BatchSink final : public M::template AbstractExporter<std::vector<T>> {
        std::atomic<int64_t> items {0};
        std::atomic<int64_t> bad {0};
        int64_t nextExpected = 0;
        virtual void start(Env *) override final {}
        virtual void handle(typename M::template InnerData<std::vector<T>> &&d) override final {
            int64_t cnt = 0;
            int64_t badLocal = 0;
            for (auto const &v : d.timedData.value) {
                if (seqOf(v) != nextExpected) {
                    ++badLocal;
                }
                ++nextExpected;
                ++cnt;
            }
            items.fetch_add(cnt, std::memory_order_relaxed);
            bad.fetch_add(badLocal, std::memory_order_relaxed);
        }
    };

    //Replica of the production BunchAction (bunch<T>), templated on the queue
    //container and payload so the benchmark can compare list/deque/vector under
    //the same threading + publish structure. Uses the conditional-notify + swap
    //drain and the range-for + clear consumer loop (valid for all three
    //containers).
    template <class T, class Queue>
    class QueueBunchAction final : public M::template AbstractAction<T, std::vector<T>> {
    private:
        using Inner = typename M::template InnerData<T>;
        typename infra::RealTimeAppComponents<Env>::template TimeChecker<false, T> timeChecker_;
        std::mutex mutex_;
        std::condition_variable cond_;
        std::atomic<bool> running_{false};
        std::thread th_;
        Queue incoming_, processing_;

        void stopThread() {
            running_ = false;
            cond_.notify_one();
        }
        void runThread() {
            while (running_) {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cond_.wait(lock, [this]{ return !running_ || !incoming_.empty(); });
                    if (!running_) {
                        lock.unlock();
                        return;
                    }
                    processing_.swap(incoming_);
                    lock.unlock();
                }
                std::vector<T> output;
                Env *env = nullptr;
                bool isFinal = false;
                for (auto &data : processing_) {
                    if (timeChecker_(data)) {
                        env = data.environment;
                        isFinal = isFinal || data.timedData.finalFlag;
                        output.push_back(std::move(data.timedData.value));
                    }
                }
                processing_.clear();
                if (!output.empty()) {
                    this->publish(typename M::template InnerData<std::vector<T>> {
                        env, { env->resolveTime(), std::move(output), isFinal }
                    });
                }
            }
        }
        void putData(Inner &&data) {
            if (running_) {
                bool wasEmpty;
                {
                    std::lock_guard<std::mutex> _(mutex_);
                    wasEmpty = incoming_.empty();
                    incoming_.push_back(std::move(data));
                }
                if (wasEmpty) {
                    cond_.notify_one();
                }
            }
        }
    public:
        QueueBunchAction() {
            running_ = true;
            th_ = std::thread(&QueueBunchAction::runThread, this);
            th_.detach();
        }
        virtual ~QueueBunchAction() {
            stopThread();
        }
        virtual bool isThreaded() const override final {
            return true;
        }
        virtual bool isOneTimeOnly() const override final {
            return false;
        }
        virtual void handle(Inner &&data) override final {
            putData(std::move(data));
        }
        virtual void setIdleWorker(std::function<void(void *)> /*worker*/) override final {
        }
        virtual void setStartWaiter(std::function<void()> /*waiter*/) override final {
        }
    };

    //Shared measurement: connect PushSource<T> -> bunchAction -> BatchSink<T>,
    //push messages items from main and report ns/item + kitem/s, checking order.
    template <class T, class ActionT>
    bool measureNode(char const *name, ActionT bunchAction, int64_t messages, int64_t warmup)
    {
        auto *env = new Env(); //leaked: detached node threads outlive main
        auto *r = new R(env);
        auto *src = new PushSource<T>();
        auto *sink = new BatchSink<T>();

        r->connect(r->importItem("src", M::importer(src)), r->actionAsSink("bunch", bunchAction));
        r->connect(r->actionAsSource(bunchAction), r->exporterAsSink("sink", M::exporter(sink)));
        r->finalize();

        //warm up: start the bunch thread, let the clock settle, flush the queue
        for (int64_t i = 0; i < warmup; ++i) {
            src->push(env, T { i });
        }
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (sink->items.load(std::memory_order_relaxed) < warmup && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        if (sink->items.load(std::memory_order_relaxed) != warmup) {
            std::fprintf(stderr, "%s: warmup did not flush (%lld != %lld)\n", name
                , static_cast<long long>(sink->items.load()), static_cast<long long>(warmup));
            return false;
        }
        //sink is now idle; reset the order-check baseline for the measured run
        sink->nextExpected = 0;
        int64_t const base = sink->items.load(std::memory_order_relaxed);

        auto const cpuBegin = std::clock();
        auto const wallBegin = std::chrono::steady_clock::now();
        for (int64_t i = 0; i < messages; ++i) {
            src->push(env, T { i });
        }
        auto const runDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (sink->items.load(std::memory_order_relaxed) - base < messages && std::chrono::steady_clock::now() < runDeadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        auto const wallEnd = std::chrono::steady_clock::now();
        auto const cpuEnd = std::clock();

        int64_t const got = sink->items.load(std::memory_order_relaxed) - base;
        int64_t const bad = sink->bad.load(std::memory_order_relaxed);
        bool const ok = got == messages && bad == 0;

        double const wallNs = std::chrono::duration<double, std::nano>(wallEnd - wallBegin).count();
        double const cpuNs = 1e9 * static_cast<double>(cpuEnd - cpuBegin) / CLOCKS_PER_SEC;
        std::printf("%-22s pushes=%8lld got=%8lld bad=%8lld  wall=%7.0f ns/item cpu=%7.0f ns/item %8.0f kitem/s\n"
            , name, static_cast<long long>(messages), static_cast<long long>(got), static_cast<long long>(bad)
            , wallNs / messages, cpuNs / messages, static_cast<double>(messages) / (wallNs / 1e6));
        std::fflush(stdout);
        if (!ok) {
            std::fprintf(stderr, "%s: did not deliver exact expected items (got=%lld bad=%lld)\n"
                , name, static_cast<long long>(got), static_cast<long long>(bad));
        }
        //graph leaked on purpose: detached node threads outlive main
        return ok;
    }

    template <class T, class Queue>
    bool runVariant(char const *name, int64_t messages, int64_t warmup)
    {
        auto action = M::template fromAbstractAction<T, std::vector<T>>(new QueueBunchAction<T, Queue>());
        return measureNode<T>(name, std::move(action), messages, warmup);
    }

    template <class T>
    bool runProduction(char const *payload, int64_t messages, int64_t warmup)
    {
        bool ok = true;
        ok = measureNode<T>((std::string(payload) + "/prod-bunch").c_str(), CFU::bunch<T>(), messages, warmup) && ok;
        ok = measureNode<T>((std::string(payload) + "/prod-bunchWithProcessing").c_str()
            , CFU::template bunchWithProcessing<T, IdentityBuncher<T>>(), messages, warmup) && ok;
        return ok;
    }

    template <class T>
    bool runPayload(char const *payload, int64_t messages, int64_t warmup, std::string const &container)
    {
        using Inner = typename M::template InnerData<T>;
        bool ok = true;
        bool matched = false;
        if (container == "all" || container == "list") {
            matched = true;
            ok = runVariant<T, std::list<Inner>>((std::string(payload) + "/list").c_str(), messages, warmup) && ok;
        }
        if (container == "all" || container == "deque") {
            matched = true;
            ok = runVariant<T, std::deque<Inner>>((std::string(payload) + "/deque").c_str(), messages, warmup) && ok;
        }
        if (container == "all" || container == "vector") {
            matched = true;
            ok = runVariant<T, std::vector<Inner>>((std::string(payload) + "/vector").c_str(), messages, warmup) && ok;
        }
        if (container == "all" || container == "prod") {
            matched = true;
            ok = runProduction<T>(payload, messages, warmup) && ok;
        }
        return matched && ok;
    }
} // namespace

int main(int argc, char **argv)
{
    int64_t const messages = (argc > 1) ? std::atoll(argv[1]) : 5000000;
    int64_t const warmup = (argc > 2) ? std::atoll(argv[2]) : 100000;
    std::string const container = (argc > 3) ? argv[3] : "all";
    std::string const payload = (argc > 4) ? argv[4] : "all";
    std::printf("bunch node queue-container throughput: PushSource<T> -> bunch -> BatchSink\n");
    std::printf("messages=%lld warmup=%lld container=%s payload=%s\n"
        , static_cast<long long>(messages), static_cast<long long>(warmup), container.c_str(), payload.c_str());
    if (messages <= 0 || messages > 100000000 || warmup < 0 || warmup > messages) {
        return 2;
    }
    bool ok = true;
    bool matched = false;
    if (payload == "all" || payload == "small") {
        matched = true;
        ok = runPayload<SmallPayload>("small", messages, warmup, container) && ok;
    }
    if (payload == "all" || payload == "big") {
        matched = true;
        ok = runPayload<BigPayload>("big", messages, warmup, container) && ok;
    }
    return matched ? (ok ? 0 : 1) : 2;
}
