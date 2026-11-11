#ifndef TM_KIT_BASIC_MOODYCAMEL_THREADED_EXECUTOR_HPP_
#define TM_KIT_BASIC_MOODYCAMEL_THREADED_EXECUTOR_HPP_
#include <tm_kit/infra/RealTimeThreadedExecutor.hpp>
#ifndef TM_KIT_BASIC_DISABLE_MOODYCAMEL_THREADED_EXECUTOR
#if __has_include(<concurrentqueue/moodycamel/concurrentqueue.h>)
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#define TM_KIT_BASIC_HAS_MOODYCAMEL_THREADED_EXECUTOR 1
#elif __has_include(<moodycamel/concurrentqueue.h>)
#include <moodycamel/concurrentqueue.h>
#define TM_KIT_BASIC_HAS_MOODYCAMEL_THREADED_EXECUTOR 1
#endif
#endif

#ifdef TM_KIT_BASIC_HAS_MOODYCAMEL_THREADED_EXECUTOR
#include <array>
#include <cstdint>
#include <vector>
namespace dev
{
    namespace cd606
    {
        namespace tm
        {
            namespace basic
            {
                using infra::ThreadedExecOptions;
                namespace moodycamel_threaded_executor_detail
                {
                    // Per-producer FIFO only. ConcurrentQueue owns reusable blocks.
                    template <class Item>
                    class MPSCQueue {
                        struct Traits : moodycamel::ConcurrentQueueDefaultTraits {
                            static constexpr std::size_t BLOCK_SIZE = 256;
                        };
                        moodycamel::ConcurrentQueue<Item, Traits> queue_;
                        moodycamel::ConsumerToken consumer_{queue_};
                        struct alignas(64) Publication {
                            std::atomic<std::size_t> count{0};
                        };
                        std::array<Publication, 8> published_{};
                        // TLS contains no queue/token pointer and outlives no queue state.
                        static unsigned producerShard()
                        {
                            static std::atomic<unsigned> next{0};
                            static thread_local unsigned const shard = next.fetch_add(1, std::memory_order_relaxed) % 8;
                            return shard;
                        }
                        alignas(64) std::size_t consumed_ = 0;
                        template <class F>
                        struct Sink {
                            F &consume;
                            std::size_t &consumed;
                            void operator=(Item &&item)
                            {
                                ++consumed;
                                consume(std::move(item));
                            }
                        };

                    public:
                        void push(Item &&item)
                        {
                            if (!queue_.enqueue(std::move(item))) {
                                throw std::bad_alloc();
                            }
                            // This RMW also pairs with the consumer's sleeping store.
                            published_[producerShard()].count.fetch_add(1, std::memory_order_seq_cst);
                        }
                        template <class F>
                        bool tryConsume(F &&consume)
                        {
                            Sink<F> sink{consume, consumed_};
                            return queue_.try_dequeue(consumer_, sink);
                        }
                        // Consumer only. If a producer observes sleeping=false before
                        // its seq_cst store, the count RMW precedes this scan in the
                        // seq_cst order. Otherwise it takes the notify mutex. Thus an
                        // enqueue cannot be missed by both the scan and notification.
                        // A dequeue may precede its count increment: inequality then
                        // conservatively stays awake until publication completes.
                        bool maybeNonEmpty() const
                        {
                            std::size_t total = 0;
                            for (auto const &slot : published_) {
                                total += slot.count.load(std::memory_order_seq_cst);
                            }
                            return total != consumed_;
                        }
                        // Hint only: never the sole predicate for blocking/idle.
                        bool hasPublishedItem() const
                        {
                            return queue_.size_approx() != 0;
                        }
                    };
                    template <class Item>
                    struct DedicatedThreadState {
                        ThreadedExecOptions opts;
                        MPSCQueue<Item> queue;
                        alignas(64) std::atomic<bool> running{false};
                        alignas(64) std::atomic<bool> sleeping{false};
                        std::atomic<bool> wakeRequested{false};
                        std::atomic<std::size_t> pending{0};
                        // Per-node override of opts.spinWait (configure); read by the
                        // consumer before each wait, so a change applies from the next wait.
                        std::atomic<std::int64_t> spinWaitNs;
                        std::mutex mutex;
                        std::condition_variable cond;

                        explicit DedicatedThreadState(ThreadedExecOptions const &o)
                            : opts(o)
                            , spinWaitNs(static_cast<std::int64_t>(o.spinWait.count()))
                        {
                        }

                        void notifyLocked()
                        {
                            std::lock_guard<std::mutex> _(mutex);
                            cond.notify_one();
                        }
                    };
                } // namespace moodycamel_threaded_executor_detail

                //Optional executor: one dedicated thread per node, joined in its destructor.
                //Blocking: the producer only takes the mutex / notifies when the consumer
                //has announced it is about to sleep (eventcount style), so the hot path
                //avoids per-item notification while the consumer is active; block
                //allocation on cache misses still has its own costs. While the host has idle work the
                //consumer waits at most Config::idlePeriod and then calls the idle work,
                //otherwise it sleeps until push / stop / wakeIdle.

                template <class Item, class Config = infra::DedicatedThreadExecutorConfig>
                class MoodycamelThreadedExecutor {
                private:
                    using State = moodycamel_threaded_executor_detail::DedicatedThreadState<Item>;
                    std::shared_ptr<State> s_;
                    std::thread thread_;           // joined in destructor after stop()
                    std::optional<std::thread::native_handle_type> handle_;
                    std::mutex configMutex_;     // guards handle_ publication and pendingCpus_
                    std::vector<int> pendingCpus_;

                    static void wakeIfSleeping(State *s)
                    {
                        //seq_cst load after publication in queue.push; pairs
                        //with the seq_cst store + maybeNonEmpty() in waitForWork
                        if (s->sleeping.load(std::memory_order_seq_cst)) {
                            s->notifyLocked();
                        }
                    }
                    template <class Host>
                    static bool drainBatch(State *s, Host *h)
                    {
                        bool handled = false;
                        while (s->queue.tryConsume([&](Item &&item) {
                            if (s->opts.trackPendingCount) {
                                if (!handled) {
                                    h->executorBeforeBatch(item, s->pending.load(std::memory_order_relaxed));
                                }
                                s->pending.fetch_sub(1, std::memory_order_relaxed);
                            }
                            handled = true;
                            h->executorHandle(std::move(item));
                        })) {
                            if (!s->running.load(std::memory_order_acquire)) {
                                break;
                            }
                        }
                        return handled;
                    }
                    //Blocking mode: returns once there may be work, the idle period has
                    //elapsed, or the executor is stopping
                    template <class Host>
                    static void waitForWork(State *s, Host *h, bool recentlyActive)
                    {
                        auto ready = [s]() { return s->queue.maybeNonEmpty() || !s->running.load(std::memory_order_acquire) || s->wakeRequested.load(std::memory_order_acquire); };
                        bool const hasIdle = h->executorHasIdleWork();
                        auto const idleDeadline = hasIdle ? std::chrono::steady_clock::now() + Config::idlePeriod : std::chrono::steady_clock::time_point::max();
                        auto budget = std::chrono::nanoseconds(s->spinWaitNs.load(std::memory_order_relaxed));
                        if (hasIdle) {
                            budget = std::min(budget, std::chrono::duration_cast<std::chrono::nanoseconds>(Config::idlePeriod));
                        }
                        if (recentlyActive && infra::realtime_threaded_executor_detail::spinUntil(budget, [s] {
                                return s->queue.hasPublishedItem() || !s->running.load(std::memory_order_acquire) || s->wakeRequested.load(std::memory_order_acquire);
                            })) {
                            // Avoid a write to the sleeping cache line unless an
                            // explicit idle wake needs acknowledgement.
                            if (s->wakeRequested.load(std::memory_order_acquire)) {
                                s->wakeRequested.exchange(false, std::memory_order_acq_rel);
                            }
                            return;
                        }
                        for (unsigned i = 0; i < Config::spinBeforeSleep; ++i) {
                            if (s->queue.maybeNonEmpty() || !s->running.load(std::memory_order_acquire)) {
                                return;
                            }
                        }
                        s->sleeping.store(true, std::memory_order_seq_cst);
                        if (!ready()) {
                            std::unique_lock<std::mutex> lock(s->mutex);
                            if (hasIdle) {
                                s->cond.wait_until(lock, idleDeadline, ready);
                            } else {
                                s->cond.wait(lock, ready);
                            }
                        }
                        s->sleeping.store(false, std::memory_order_relaxed);
                        //a wakeIdle racing with this reset is not lost: its host state
                        //change happened before it set the flag, and the next round
                        //re-reads executorHasIdleWork() before sleeping again
                        if (s->wakeRequested.load(std::memory_order_acquire)) {
                            s->wakeRequested.exchange(false, std::memory_order_acq_rel);
                        }
                    }
                    template <class Host>
                    static void run(std::shared_ptr<State> s, Host *h)
                    {
                        h->executorWaitForStart();
                        bool recentlyActive = false;
                        while (s->running.load(std::memory_order_acquire)) {
                            if (drainBatch(s.get(), h)) {
                                recentlyActive = true;
                                //more data may have arrived while handling; only
                                //idle / sleep once the queue is really empty
                                continue;
                            }
                            waitForWork(s.get(), h, recentlyActive);
                            recentlyActive = false;
                            if (!s->running.load(std::memory_order_acquire)) {
                                return;
                            }
                            // The approximate size is only an active-path hint. Check
                            // exact publication counts before idle or sleep.
                            if (!s->queue.hasPublishedItem() && !s->queue.maybeNonEmpty()) {
                                //timed out (or woken by wakeIdle) with nothing pending
                                h->executorIdleWork();
                            }
                        }
                    }

                public:
                    static constexpr bool kOwnContext = true;
                    static constexpr bool kSerialCallbacks = true;
                    explicit MoodycamelThreadedExecutor(ThreadedExecOptions const &opts = ThreadedExecOptions{})
                        : s_(std::make_shared<State>(opts))
                        , handle_(std::nullopt)
                    {
                    }
                    MoodycamelThreadedExecutor(MoodycamelThreadedExecutor const &) = delete;
                    MoodycamelThreadedExecutor &operator=(MoodycamelThreadedExecutor const &) = delete;
                    ~MoodycamelThreadedExecutor()
                    {
                        stop();
                        if (thread_.joinable()) {
                            // Destroyed from its own callback: the thread cannot join
                            // itself; it exits on its own after the callback returns.
                            if (thread_.get_id() == std::this_thread::get_id()) {
                                thread_.detach();
                            } else {
                                thread_.join();
                            }
                        }
                    }
                    template <class Host>
                    void start(Host *h)
                    {
                        s_->running.store(true, std::memory_order_release);
                        std::thread th(&MoodycamelThreadedExecutor::run<Host>, s_, h);
                        {
                            std::lock_guard<std::mutex> lock(configMutex_);
                            handle_ = th.native_handle();
                            infra::realtime_threaded_executor_detail::setThreadAffinity(*handle_, pendingCpus_);
                        }
                        thread_ = std::move(th);
                    }
                    bool push(Item &&x)
                    {
                        State *s = s_.get();
                        if (!s->running.load(std::memory_order_acquire)) {
                            return false;
                        }
                        if (s->opts.trackPendingCount) {
                            s->pending.fetch_add(1, std::memory_order_relaxed);
                        }
                        try {
                            s->queue.push(std::move(x));
                        } catch (...) {
                            if (s->opts.trackPendingCount) {
                                s->pending.fetch_sub(1, std::memory_order_relaxed);
                            }
                            throw;
                        }
                        wakeIfSleeping(s);
                        return true;
                    }
                    void stop()
                    {
                        s_->running.store(false, std::memory_order_release);
                        s_->notifyLocked();
                    }
                    void wakeIdle()
                    {
                        s_->wakeRequested.store(true, std::memory_order_release);
                        s_->notifyLocked();
                    }
                    // Before the graph starts (possibly after start()). spinWait takes
                    // effect from the consumer's next wait; the CPU set is applied
                    // immediately when the thread runs, otherwise at start().
                    bool configure(infra::ThreadedNodeOptions const &o)
                    {
                        // Reject the whole call before changing anything.
                        if (!infra::realtime_threaded_executor_detail::validCpus(o.cpus)) {
                            return false;
                        }
                        if (o.spinWait) {
                            auto ns = std::max(o.spinWait->count(), static_cast<std::chrono::nanoseconds::rep>(0));
                            s_->spinWaitNs.store(static_cast<std::int64_t>(ns), std::memory_order_relaxed);
                        }
                        std::lock_guard<std::mutex> lock(configMutex_);
                        if (o.cpus.empty()) {
                            return true; // keep the current / pending affinity
                        }
                        pendingCpus_ = o.cpus;
                        if (handle_ && s_->running.load(std::memory_order_acquire)) {
                            return infra::realtime_threaded_executor_detail::setThreadAffinity(*handle_, pendingCpus_);
                        }
                        return true;
                    }
                    std::optional<std::thread::native_handle_type> threadHandle()
                    {
                        return handle_;
                    }
                };

                struct MoodycamelThreadedExecutorComponent : infra::RealTimeThreadedExecutorComponent<MoodycamelThreadedExecutor> {};
            } // namespace basic
        } // namespace tm
    } // namespace cd606
} // namespace dev
#endif // TM_KIT_BASIC_HAS_MOODYCAMEL_THREADED_EXECUTOR

#endif // TM_KIT_BASIC_MOODYCAMEL_THREADED_EXECUTOR_HPP_
