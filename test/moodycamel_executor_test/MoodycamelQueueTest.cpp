#include <tm_kit/basic/MoodycamelThreadedExecutor.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif
using namespace dev::cd606::tm;
namespace
{
    int failures = 0;
    void check(bool ok, char const *name)
    {
        std::cout << (ok ? "ok: " : "FAIL: ") << name << '\n';
        failures += !ok;
    }
    void queueBlockStressTest()
    {
        using Queue = basic::moodycamel_threaded_executor_detail::MPSCQueue<std::pair<unsigned, unsigned>>;
        Queue queue;
        constexpr unsigned producers = 16, count = 100000;
        std::array<unsigned, producers> expected{};
        std::array<std::thread, producers> threads;
        for (unsigned p = 0; p < producers; ++p) {
            threads[p] = std::thread([&, p] {
                for (unsigned i = 0; i < count; ++i) {
                    queue.push({p, i});
                    if ((i & 255) == 0) {
                        std::this_thread::yield();
                    }
                }
            });
        }
        unsigned received = 0;
        bool ordered = true;
        while (received < producers * count) {
            if (!queue.tryConsume([&](auto &&item) {
                    ordered = ordered && item.second == expected[item.first];
                    ++expected[item.first];
                    ++received;
                })) {
                std::this_thread::yield();
            }
        }
        for (auto &thread : threads) {
            thread.join();
        }
        check(ordered && !queue.maybeNonEmpty(), "publication shard collisions preserve exact counts and producer FIFO");
        // Exercise repeated block turnover after all initial producers exit.
        for (unsigned round = 0; round < 1000; ++round) {
            for (unsigned i = 0; i < 513; ++i) {
                queue.push({0, i});
            }
            for (unsigned i = 0; i < 513; ++i) {
                ordered = queue.tryConsume([&](auto &&item) { ordered = ordered && item.second == i; }) && ordered;
            }
        }
        check(ordered, "reusable blocks preserve per-producer FIFO across repeated bursts");
        queue.push({0, 0});
    }
    void producerChurnTest()
    {
        for (unsigned lifetime = 0; lifetime < 8; ++lifetime) {
            basic::moodycamel_threaded_executor_detail::MPSCQueue<unsigned> queue;
            for (unsigned generation = 0; generation < 32; ++generation) {
                std::thread producer([&] {
                    for (unsigned i = 0; i < 1000; ++i) {
                        queue.push(unsigned(i));
                    }
                });
                producer.join();
                unsigned expected = 0;
                while (queue.tryConsume([&](unsigned &&i) {
                    if (i != expected++) {
                        ++failures;
                    }
                })) {
                }
                if (expected != 1000 || queue.maybeNonEmpty()) {
                    ++failures;
                }
            }
        }
        check(failures == 0, "producer exit and queue address reuse preserve ownership and counts");
    }
    struct Item {
        static int moves, live;
        static bool throwMove;
        int value;
        explicit Item(int v)
            : value(v)
        {
            ++live;
        }
        Item(Item const &) = delete;
        Item &operator=(Item const &) = delete;
        Item(Item &&x)
            : value(x.value)
        {
            if (throwMove) {
                throw std::runtime_error("move");
            }
            ++moves;
            ++live;
        }
        ~Item()
        {
            --live;
        }
    };
    int Item::moves = 0;
    int Item::live = 0;
    bool Item::throwMove = false;
    void queueTest()
    {
        using Q = basic::moodycamel_threaded_executor_detail::MPSCQueue<Item>;
        {
            Q queue;
            bool called = false;
            check(!queue.tryConsume([&](Item &&) { called = true; }) && !called, "empty queue never invokes consumer");
            Item value(7);
            Item::throwMove = true;
            try {
                queue.push(std::move(value));
                check(false, "throwing move propagated");
            } catch (std::runtime_error const &) {
            }
            Item::throwMove = false;
            check(!queue.maybeNonEmpty(), "throwing enqueue leaves queue unchanged");
            queue.push(std::move(value));
            check(Item::moves == 1, "enqueue constructs move-only non-default item once");
            int observed = 0;
            queue.tryConsume([&](Item &&x) { observed = x.value; });
            check(observed == 7 && Item::moves == 1 && Item::live == 1, "consume in place has no extra move and destroys queued item");
            Item::throwMove = true;
            try {
                queue.push(std::move(value));
                check(false, "cached-node throwing move propagated");
            } catch (std::runtime_error const &) {
            }
            Item::throwMove = false;
            check(!queue.maybeNonEmpty() && Item::live == 1, "reused-block construction exception does not retain Item");
            queue.push(Item(9));
            queue.push(Item(10));
            try {
                queue.tryConsume([](Item &&) { throw std::runtime_error("consume"); });
            } catch (std::runtime_error const &) {
            }
            check(queue.tryConsume([&](Item &&x) { observed = x.value; }) && observed == 10, "throwing consumer preserves remaining producer sequence");
            queue.push(Item(11)); // destructor drops a still-queued item
        }
        check(Item::live == 0, "queue drains/destroys all item lifetimes");
    }
    template <class F>
    bool wait(F &&ready)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ready()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }
    using Exec = basic::MoodycamelThreadedExecutor<std::pair<unsigned, unsigned>>;
    struct SelectedEnv : basic::MoodycamelThreadedExecutorComponent {};
    static_assert(std::is_same_v<infra::RealTimeThreadedExecutorFor<SelectedEnv, std::pair<unsigned, unsigned>>, Exec>);
    struct Host {
        Exec *exec;
        std::atomic<bool> ready{false}, wantsIdle{false}, stopInCallback{false};
        std::atomic<unsigned> count{0}, errors{0}, idle{0}, batches{0};
        std::atomic<std::size_t> firstBatch{0};
        std::array<unsigned, 4> expected{};
        void executorWaitForStart()
        {
            while (!ready.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        bool executorHasIdleWork()
        {
            return wantsIdle.load();
        }
        void executorIdleWork()
        {
            ++idle;
        }
        void executorBeforeBatch(std::pair<unsigned, unsigned> const &, std::size_t n)
        {
            if (batches.fetch_add(1) == 0) {
                firstBatch = n;
            }
        }
        void executorHandle(std::pair<unsigned, unsigned> &&x)
        {
            if (x.second != expected[x.first]++) {
                ++errors;
            }
            if (stopInCallback.load()) {
                exec->stop();
            }
            count.fetch_add(1, std::memory_order_release);
        }
    };
    void executorTest()
    {
        // Host and executor deliberately outlive detached callback epilogues.
        infra::ThreadedExecOptions opts;
        opts.trackPendingCount = true;
        auto *exec = new Exec(opts);
        auto *host = new Host{exec};
        check(!exec->push({0, 0}), "moodycamel rejects pushes before start");
        exec->start(host);
        std::array<std::thread, 4> producers;
        for (unsigned p = 0; p < 4; ++p) {
            producers[p] = std::thread([&, p] {
                for (unsigned n = 0; n < 10000; ++n) {
                    exec->push({p, n});
                }
            });
        }
        for (auto &thread : producers) {
            thread.join();
        }
        host->ready.store(true, std::memory_order_release);
        check(wait([&] { return host->count.load(std::memory_order_acquire) == 40000; }) && host->errors == 0, "moodycamel starts once and drains concurrent producers in FIFO order");
        check(host->firstBatch == 40000 && exec->threadHandle().has_value(), "moodycamel preserves batch counts and native handle");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(host->idle == 0, "moodycamel sleeps without periodic idle work");
        host->wantsIdle = true;
        exec->wakeIdle();
        check(wait([&] { return host->idle > 2; }), "moodycamel wakeIdle activates periodic idle work");
        host->stopInCallback = true;
        exec->push({0, 10000});
        check(wait([&] { return host->count.load(std::memory_order_acquire) == 40001; }) && !exec->push({0, 10001}), "moodycamel stop is callback safe and rejects later pushes");
    }
    void sleepingPublicationTest()
    {
        for (unsigned spinUs : {0U, 200U}) {
            infra::ThreadedExecOptions opts;
            opts.spinWait = std::chrono::microseconds(spinUs);
            auto *exec = new Exec(opts);
            auto *host = new Host{exec};
            host->ready = true;
            exec->start(host);
            bool delivered = true;
            for (unsigned i = 0; i < 1000; ++i) {
                std::this_thread::sleep_for(std::chrono::microseconds(300));
                exec->push({0, i});
                if (!wait([&] { return host->count.load(std::memory_order_acquire) == i + 1; })) {
                    delivered = false;
                    break;
                }
            }
            check(delivered && host->errors == 0, "publication alone wakes a sleeping consumer without idle tokens");
            exec->stop();
        }
    }
    void wakeRaceTest()
    {
        for (unsigned spinUs : {0U, 200U}) {
            infra::ThreadedExecOptions opts;
            opts.spinWait = std::chrono::microseconds(spinUs);
            auto *exec = new Exec(opts);
            auto *host = new Host{exec};
            host->ready = true;
            exec->start(host);
            std::thread producer([&] {
                for (unsigned n = 0; n < 20000; ++n) {
                    exec->push({0, n});
                    if ((n & 63) == 0) {
                        std::this_thread::yield();
                    }
                }
            });
            std::thread waker([&] {
                for (unsigned n = 0; n < 20000; ++n) {
                    host->wantsIdle = (n & 1) != 0;
                    exec->wakeIdle();
                }
            });
            producer.join();
            waker.join();
            check(wait([&] { return host->count.load(std::memory_order_acquire) == 20000; }) && host->errors == 0, "concurrent idle wakes and publications drain without lost wakeups");
            auto idleBefore = host->idle.load();
            host->wantsIdle = true;
            exec->wakeIdle();
            check(wait([&] { return host->idle.load() > idleBefore; }), "final idle activation survives wake acknowledgement race");
            exec->stop();
            check(!exec->push({0, 20000}), "stop remains effective after wake stress");
        }
    }
#ifdef __linux__
    std::vector<int> affinityOf(std::thread::native_handle_type h)
    {
        std::vector<int> r;
        cpu_set_t set;
        CPU_ZERO(&set);
        if (pthread_getaffinity_np(h, sizeof(set), &set) == 0) {
            for (int c = 0; c < CPU_SETSIZE; ++c) {
                if (CPU_ISSET(c, &set)) {
                    r.push_back(c);
                }
            }
        }
        return r;
    }
    // CPU time the consumer burns after one item: approximately the spin budget.
    double consumerCpuMsAfterOnePush(Exec *exec, Host *host, unsigned seq)
    {
        clockid_t cid;
        if (pthread_getcpuclockid(*exec->threadHandle(), &cid) != 0) {
            return -1;
        }
        timespec a{}, b{};
        clock_gettime(cid, &a);
        exec->push({0, seq});
        wait([&] { return host->count.load(std::memory_order_acquire) == seq + 1; });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        clock_gettime(cid, &b);
        return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    }
    void nodeOptionsTest()
    {
        auto allowed = affinityOf(pthread_self());
        if (allowed.empty()) {
            check(false, "process affinity readable");
            return;
        }
        int const first = allowed.front(), last = allowed.back();
        auto *exec = new Exec;
        auto *host = new Host{exec};
        host->ready = true;
        // Before start: stored and applied when the thread is created.
        bool before = exec->configure(infra::ThreadedNodeOptions().Cpus({first}).SpinWait(std::chrono::nanoseconds::zero()));
        exec->start(host);
        auto now = affinityOf(*exec->threadHandle());
        check(before && now.size() == 1 && now[0] == first, "moodycamel: affinity set before start applied at start");
        double noSpin = consumerCpuMsAfterOnePush(exec, host, 0);
        // After start (node already constructed, graph not yet running).
        bool after = exec->configure(infra::ThreadedNodeOptions().Cpus({last}).SpinWait(std::chrono::milliseconds(30)));
        now = affinityOf(*exec->threadHandle());
        check(after && now.size() == 1 && now[0] == last, "moodycamel: affinity changed after start");
        double spin = consumerCpuMsAfterOnePush(exec, host, 1);
        std::cout << "      consumer CPU ms after one push: spinWait=0 " << noSpin << ", spinWait=30ms " << spin << '\n';
        check(noSpin >= 0 && noSpin < 5, "moodycamel: spinWait=0 does not spin");
        check(spin >= 15, "moodycamel: per-node spinWait takes effect");
        // Rejected options change nothing.
        bool bad = exec->configure(infra::ThreadedNodeOptions().Cpus({-1}).SpinWait(std::chrono::nanoseconds::zero()));
        now = affinityOf(*exec->threadHandle());
        double stillSpin = consumerCpuMsAfterOnePush(exec, host, 2);
        check(!bad && now.size() == 1 && now[0] == last && stillSpin >= 15, "moodycamel: invalid CPU rejected, affinity and spinWait kept");
        // Empty options keep both settings.
        bool keep = exec->configure(infra::ThreadedNodeOptions());
        now = affinityOf(*exec->threadHandle());
        check(keep && now.size() == 1 && now[0] == last, "moodycamel: empty options keep affinity");
        exec->stop();
    }
#endif
} // namespace
int main()
{
    queueTest();
    queueBlockStressTest();
    producerChurnTest();
    executorTest();
    sleepingPublicationTest();
    wakeRaceTest();
#ifdef __linux__
    nodeOptionsTest();
#endif
    return failures ? 1 : 0;
}
