#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <tm_kit/basic/IntIDComponent.hpp>
#include <tm_kit/basic/real_time_clock/ClockComponent.hpp>
#include <tm_kit/basic/SpdLoggingComponent.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

//Benchmark of the TM Kit RealTimeApp graph machinery itself: the cost one
//message pays for passing nodes, queues, environments and schedulers when
//the user logic is trivial. Not a test: run it by hand, preferably pinned to
//idle cores (same advice as low_latency_bench).
//
//Graph (deliberately varied):
//  source(int64_t)
//    -> pure        liftPure<int64_t>               1:1
//    -> maybe       liftMaybe<int64_t>              drops every third
//    -> multi       liftMulti<int64_t>              1:2
//    -> split       liftPure<int64_t -> tuple>      tuple producer
//    -> dispatch    dispatchTupleAction             tuple -> variant fan-out
//    -> join        liftPure2<int64_t,uint32_t>     two-input variant action
//    -> sinkA
//  trigger(Key<int64_t>) -> on-order facility -> sinkB
//
//Variants differ only in which nodes are SuggestThreaded, so the deltas show
//the framework cost of each execution model.
//
//Columns: wall/cpu ns per pushed message, and thousands of messages/s.

using namespace dev::cd606::tm;

namespace {

    //AppRunner reports graph warnings through env.log(), so the clock has
    //to come with a logging component
    using Clock = basic::TimeComponentEnhancedWithSpdLogging<basic::real_time_clock::ClockComponent, false>;
    using Env = infra::Environment<
        infra::CheckTimeComponent<false>, infra::TrivialExitControlComponent
        , basic::IntIDComponent<>, Clock>;
    using M = infra::RealTimeApp<Env>;
    using R = infra::AppRunner<M>;
    using LP = infra::LiftParameters<Env::TimePointType>;

    template <class T>
    class PushSource final : public M::template AbstractImporter<T> {
    public:
        virtual void start(Env *) override final {}
        inline void push(Env *env, T &&v) {
            this->publish(M::template pureInnerData<T>(env, std::move(v)));
        }
    };

    struct IntSink final : public M::template AbstractExporter<int64_t> {
        std::atomic<int64_t> count {0};
        std::atomic<int64_t> bad {0};
        virtual void start(Env *) override final {}
        virtual void handle(typename M::template InnerData<int64_t> &&d) override final {
            if (d.timedData.value < 0) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
            count.fetch_add(1, std::memory_order_relaxed);
        }
    };

    struct Variant {
        char const *name;
        bool threadedAction;
        bool threadedFacility;
        bool threadedSrc;
    };

    bool runVariant(Variant const &v, int64_t messages)
    {
        auto *env = new Env(); //leaked: detached node threads outlive main
        auto *r = new R(env);
        auto *src = new PushSource<int64_t>();
        auto *sinkA = new IntSink();
        auto *sinkB = new IntSink();

        auto tl = [](bool t) {
            return t ? LP {}.SuggestThreaded(true) : LP {};
        };

        auto pure = M::template liftPure<int64_t>([](int64_t &&x) {return x+1;}, tl(v.threadedSrc));
        auto maybe = M::template liftMaybe<int64_t>([](int64_t &&x) -> std::optional<int64_t> {
            if (x%3 == 0) {
                return std::nullopt;
            }
            return x;
        }, tl(v.threadedAction));
        auto multi = M::template liftMulti<int64_t>([](int64_t &&x) {
            return std::vector<int64_t> {x, x+1};
        }, tl(v.threadedAction));
        auto split = M::template liftPure<int64_t>([](int64_t &&x) {
            return std::tuple<int64_t, uint32_t> {x, static_cast<uint32_t>(x)};
        });
        auto disp = M::template dispatchTupleAction<int64_t, uint32_t>();
        auto join = M::template liftPure2<int64_t, uint32_t>([](std::variant<int64_t, uint32_t> &&x) -> int64_t {
            if (x.index() == 0) {
                return std::get<0>(x);
            }
            return static_cast<int64_t>(std::get<1>(x));
        }, tl(v.threadedAction));
        auto facility = M::template liftPureOnOrderFacility<int64_t>(
            [](int64_t &&x) {return x;}, tl(v.threadedFacility));
        auto trigger = M::template triggerImporter<typename M::template Key<int64_t>>();

        r->connect(r->importItem("src", M::importer(src)), r->actionAsSink("pure", pure));
        r->connect(r->actionAsSource(pure), r->actionAsSink("maybe", maybe));
        r->connect(r->actionAsSource(maybe), r->actionAsSink("multi", multi));
        r->connect(r->actionAsSource(multi), r->actionAsSink("split", split));
        r->connect(r->actionAsSource(split), r->actionAsSink("disp", disp));
        //both branches of the fan-out feed the two-input join
        //disp is a variant action: its source carries both branches, so the
        //two consumers are wired by branch index
        r->connect_2_0(r->actionAsSource(disp), r->actionAsSink_2_0("join", join));
        r->connect_2_1(r->actionAsSource(disp), r->actionAsSink_2_1(join));
        r->connect(r->actionAsSource(join), r->exporterAsSink("sinkA", M::exporter(sinkA)));
        //an on-order facility answers with KeyedData<input, output>
        auto keyedSink = M::template pureExporter<M::template KeyedData<int64_t, int64_t>>(
            [sinkB](M::template KeyedData<int64_t, int64_t> &&d) {
                if (d.data < 0) {
                    sinkB->bad.fetch_add(1, std::memory_order_relaxed);
                }
                sinkB->count.fetch_add(1, std::memory_order_relaxed);
            });
        r->placeOrderWithFacility(
            r->importItem("facTrigger", std::get<0>(trigger))
            , "facility", facility
            , r->exporterAsSink("sinkB", keyedSink));
        r->finalize();
        auto &fire = std::get<1>(trigger);

        //warm up: start every node thread and let the clock settle
        src->push(env, int64_t(-1));
        //triggerImporter sets its running flag inside its own thread, so a
        //fire() right after finalize() can be dropped: retry until the
        //facility has answered once
        for (int tries = 0; tries < 1000 && sinkB->count.load() == 0; ++tries) {
            M::template Key<int64_t> k {int64_t(0)};
            fire(std::move(k));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        //report only what the measured pushes produced
        int64_t const baseA = sinkA->count.load();
        int64_t const baseB = sinkB->count.load();

        auto const cpuBegin = std::clock();
        auto const wallBegin = std::chrono::steady_clock::now();
        for (int64_t i = 0; i < messages; ++i) {
            src->push(env, int64_t(i));
            M::template Key<int64_t> k {i};
            fire(std::move(k));
        }
        // The graph deterministically keeps messages-messages/3 inputs,
        // emits two multi items and dispatches each to two join inputs.
        // Finish on exact counts; a quiet window can end early on a stalled
        // graph and charged background CPU after all messages had completed.
        int64_t const expectA = 4 * (messages - messages / 3);
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while ((sinkA->count.load() - baseA < expectA || sinkB->count.load() - baseB < messages) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        auto const wallEnd = std::chrono::steady_clock::now();
        auto const cpuEnd = std::clock();
        bool const ok = sinkA->count.load() - baseA == expectA && sinkB->count.load() - baseB == messages && sinkA->bad.load() + sinkB->bad.load() == 0;

        double const wallNs = std::chrono::duration<double, std::nano>(wallEnd-wallBegin).count();
        double const cpuNs = 1e9*static_cast<double>(cpuEnd-cpuBegin)/CLOCKS_PER_SEC;
        std::printf("%-24s pushes=%7lld joinOut=%7lld facilityOut=%7lld bad=%lld  wall=%7.0f ns/msg cpu=%7.0f ns/msg %6.0f kpush/s\n",
            v.name, static_cast<long long>(messages)
            , static_cast<long long>(sinkA->count.load()-baseA)
            , static_cast<long long>(sinkB->count.load()-baseB)
            , static_cast<long long>(sinkA->bad.load()+sinkB->bad.load())
            //kpush/s = messages / wall seconds / 1000 = messages / wall ms
            , wallNs/messages, cpuNs/messages, static_cast<double>(messages)/(wallNs/1e6));
        std::fflush(stdout);
        if (!ok) {
            std::fprintf(stderr, "graph did not deliver exact expected counts\n");
        }
        //graph leaked on purpose: detached threads and the clock thread
        return ok;
    }
}

int main(int argc, char **argv) {
    int64_t const messages = (argc > 1) ? std::atoll(argv[1]) : 200000;
    std::printf("RealTimeApp graph overhead, graph: pure -> maybe -> multi -> split -> dispatch -> join (+ on-order facility)\n");
    std::printf("messages per variant: %lld\n", static_cast<long long>(messages));
    if (messages <= 0 || messages > 100000000) {
        return 2;
    }
    // Optional argv[2] isolates one graph in its own process for profiling.
    std::string selected = argc > 2 ? argv[2] : "all";
    bool ok = true, matched = false;
    for (auto const &v :
         {Variant{"plain", false, false, false},
          Variant{"src threaded", true, false, false},
          Variant{"action+facility threaded", true, true, false},
          Variant{"src+action+facility", true, true, true}}) {
        if (selected == "all" || selected == v.name) {
            matched = true;
            ok = runVariant(v, messages) && ok;
        }
    }
    return matched ? (ok ? 0 : 1) : 2;
}
