#include <tm_kit/basic/EnhancedDedicatedThreadedExecutor.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
using namespace dev::cd606::tm;
namespace
{
    int failures = 0;
    void check(bool ok, char const *name)
    {
        std::cout << (ok ? "ok: " : "FAIL: ") << name << '\n';
        failures += !ok;
    }
    void queueRecycleStressTest()
    {
        using Queue = basic::enhanced_dedicated_executor_detail::MPSCQueue<std::pair<unsigned, unsigned>>;
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
        check(ordered && !queue.maybeNonEmpty(), "recycle contention preserves exact counts and producer FIFO");
        // Exercise repeated ring wrap, full-cache eviction and destruction with
        // outstanding values after all producer threads have exited.
        for (unsigned round = 0; round < 1000; ++round) {
            for (unsigned i = 0; i < 513; ++i) {
                queue.push({0, i});
            }
            for (unsigned i = 0; i < 513; ++i) {
                ordered = queue.tryConsume([&](auto &&item) { ordered = ordered && item.second == i; }) && ordered;
            }
        }
        check(ordered, "recycle cache wraps and evicts excess nodes without changing FIFO");
        queue.push({0, 0});
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
        using Q = basic::enhanced_dedicated_executor_detail::MPSCQueue<Item>;
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
            check(!queue.maybeNonEmpty() && Item::live == 1, "cached-node construction exception releases node without retaining Item");
            queue.push(Item(9));
            queue.push(Item(10));
            try {
                queue.tryConsume([](Item &&) { throw std::runtime_error("consume"); });
            } catch (std::runtime_error const &) {
            }
            check(queue.tryConsume([&](Item &&x) { observed = x.value; }) && observed == 10, "throwing consumer leaves valid FIFO stub");
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
    using Exec = basic::EnhancedDedicatedThreadedExecutor<std::pair<unsigned, unsigned>>;
    struct SelectedEnv : basic::EnhancedDedicatedThreadedExecutorComponent {};
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
        check(!exec->push({0, 0}), "enhanced rejects pushes before start");
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
        check(wait([&] { return host->count.load(std::memory_order_acquire) == 40000; }) && host->errors == 0, "enhanced starts once and drains concurrent producers in FIFO order");
        check(host->firstBatch == 40000 && exec->threadHandle().has_value(), "enhanced preserves batch counts and native handle");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(host->idle == 0, "enhanced sleeps without periodic idle work");
        host->wantsIdle = true;
        exec->wakeIdle();
        check(wait([&] { return host->idle > 2; }), "enhanced wakeIdle activates periodic idle work");
        host->stopInCallback = true;
        exec->push({0, 10000});
        check(wait([&] { return host->count.load(std::memory_order_acquire) == 40001; }) && !exec->push({0, 10001}), "enhanced stop is callback safe and rejects later pushes");
    }
} // namespace
int main()
{
    queueTest();
    queueRecycleStressTest();
    executorTest();
    return failures ? 1 : 0;
}
