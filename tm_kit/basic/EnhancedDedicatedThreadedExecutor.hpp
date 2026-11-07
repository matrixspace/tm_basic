#ifndef TM_KIT_BASIC_ENHANCED_DEDICATED_THREADED_EXECUTOR_HPP_
#define TM_KIT_BASIC_ENHANCED_DEDICATED_THREADED_EXECUTOR_HPP_
#include <tm_kit/infra/RealTimeThreadedExecutor.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <array>
namespace dev
{
    namespace cd606
    {
        namespace tm
        {
            namespace basic
            {
                using infra::ThreadedExecOptions;
                namespace enhanced_dedicated_executor_detail
                {
                    //Vyukov intrusive unbounded MPSC queue with a stub node.
                    //link publication uses one exchange + one store; allocation may block.
                    //The global order is
                    //the exchange order, so items of all producers form one FIFO.
                    template <class Item>
                    class MPSCQueue {
                    private:
                        struct Node {
                            std::atomic<Node *> next{nullptr};
                            std::optional<Item> value;
                            unsigned shard = 0;
                            Node() = default;
                        };
                        // Sharded bounded Boost.Lockfree recycle queues. A node returns to the
                        // shard that allocated it. Thread-local state is only an integer,
                        // never a queue pointer: producer exit and queue destruction do
                        // not leave dangling registrations. Colliding producers bypass a busy cache.
                        static constexpr std::size_t recycleShards = 8;
                        static constexpr std::size_t recycleCapacity = 32;
                        struct RecycleCache {
                            boost::lockfree::spsc_queue<Node *, boost::lockfree::capacity<recycleCapacity>> queue;
                            // Queue consumer returns nodes (one writer). Producers take
                            // nodes as one logical reader, with nonblocking admission.
                            // Collisions bypass the cache rather than spin on this flag.
                            alignas(64) std::atomic_flag reading = ATOMIC_FLAG_INIT;
                            // Protected by reading: amortize queue index synchronization.
                            // Stored in the queue, so producer exit cannot strand nodes.
                            std::array<Node *, 8> local{};
                            std::size_t available = 0;
                        };
                        std::array<RecycleCache, recycleShards> recycled_;

                        static unsigned producerShard()
                        {
                            static std::atomic<unsigned> next{0};
                            thread_local unsigned const shard = next.fetch_add(1, std::memory_order_relaxed) % recycleShards;
                            return shard;
                        }
                        Node *acquireNode()
                        {
                            auto shard = producerShard();
                            Node *n = nullptr;
                            auto &cache = recycled_[shard];
                            if (!cache.reading.test_and_set(std::memory_order_acquire)) {
                                if (cache.available == 0) {
                                    cache.available = cache.queue.pop(cache.local.data(), cache.local.size());
                                }
                                if (cache.available) {
                                    n = cache.local[--cache.available];
                                }
                                cache.reading.clear(std::memory_order_release);
                            }
                            if (n) {
                                n->next.store(nullptr, std::memory_order_relaxed);
                            } else {
                                n = new Node();
                                n->shard = shard;
                            }
                            return n;
                        }
                        void recycleNode(Node *n)
                        {
                            if (!recycled_[n->shard].queue.push(n)) {
                                delete n;
                            }
                        }
                        //written by producers
                        alignas(64) std::atomic<Node *> head_;
                        //owned by the single consumer
                        alignas(64) Node *tail_;

                    public:
                        MPSCQueue()
                            : head_(nullptr)
                            , tail_(nullptr)
                        {
                            Node *stub = new Node();
                            head_.store(stub, std::memory_order_relaxed);
                            tail_ = stub;
                        }
                        ~MPSCQueue()
                        {
                            for (auto &cache : recycled_) {
                                while (cache.available) {
                                    delete cache.local[--cache.available];
                                }
                                Node *cached;
                                while (cache.queue.pop(cached)) {
                                    delete cached;
                                }
                            }
                            Node *n = tail_;
                            while (n) {
                                Node *next = n->next.load(std::memory_order_relaxed);
                                delete n;
                                n = next;
                            }
                        }
                        MPSCQueue(MPSCQueue const &) = delete;
                        MPSCQueue &operator=(MPSCQueue const &) = delete;
                        //any thread. The exchange is seq_cst: together with the
                        //seq_cst load of the sleeping flag that follows it in the
                        //executor, and the consumer's seq_cst store of that flag
                        //followed by maybeNonEmpty(), it forms a Dekker handshake
                        //(either the consumer sees the item or the producer sees the
                        //consumer sleeping). Plain seq_cst operations are used instead of
                        //atomic_thread_fence so that TSan can check the protocol.
                        void push(Item &&x)
                        {
                            std::unique_ptr<Node> owned(acquireNode());
                            // Reclaim the empty node even if Item construction throws.
                            owned->value.emplace(std::move(x));
                            Node *n = owned.release();
                            Node *prev = head_.exchange(n, std::memory_order_seq_cst);
                            prev->next.store(n, std::memory_order_release);
                        }
                        //consumer only; may transiently report empty while a producer
                        //is between its exchange and its link store, the wake-up
                        //protocol of the executor covers that window
                        template <class F>
                        bool tryConsume(F &&consume)
                        {
                            Node *next = tail_->next.load(std::memory_order_acquire);
                            if (!next) {
                                return false;
                            }
                            Node *old = tail_;
                            tail_ = next;
                            recycleNode(old);
                            // The new tail remains the queue's stub. Its item
                            // can be consumed in place, without a second move.
                            // Reset also on exceptions, leaving a valid stub.
                            struct Reset {
                                std::optional<Item> &value;
                                ~Reset()
                                {
                                    value.reset();
                                }
                            } reset{next->value};
                            std::forward<F>(consume)(std::move(*next->value));
                            return true;
                        }
                        //consumer only. Unlike tryConsume this also reports items whose
                        //producer has done the exchange but not yet linked the node, so
                        //the consumer never goes to sleep inside that window
                        bool maybeNonEmpty() const
                        {
                            return head_.load(std::memory_order_seq_cst) != tail_;
                        }
                    };

                    //everything the execution thread touches lives here and is shared
                    //with that (detached) thread, so the executor object itself may be
                    //destroyed while the thread is still winding down
                    template <class Item>
                    struct DedicatedThreadState {
                        ThreadedExecOptions opts;
                        MPSCQueue<Item> queue;
                        alignas(64) std::atomic<bool> running{false};
                        alignas(64) std::atomic<bool> sleeping{false};
                        std::atomic<bool> wakeRequested{false};
                        std::atomic<std::size_t> pending{0};
                        std::mutex mutex;
                        std::condition_variable cond;

                        explicit DedicatedThreadState(ThreadedExecOptions const &o)
                            : opts(o)
                        {
                        }

                        void notifyLocked()
                        {
                            std::lock_guard<std::mutex> _(mutex);
                            cond.notify_one();
                        }
                    };
                } // namespace enhanced_dedicated_executor_detail

                //Default executor: one dedicated (detached) thread per node.
                //Blocking: the producer only takes the mutex / notifies when the consumer
                //has announced it is about to sleep (eventcount style), so the hot path
                //avoids per-item notification while the consumer is active; node
                //allocation on cache misses still has its own costs. While the host has idle work the
                //consumer waits at most Config::idlePeriod and then calls the idle work,
                //otherwise it sleeps until push / stop / wakeIdle.

                template <class Item, class Config = infra::DedicatedThreadExecutorConfig>
                class EnhancedDedicatedThreadedExecutor {
                private:
                    using State = enhanced_dedicated_executor_detail::DedicatedThreadState<Item>;
                    std::shared_ptr<State> s_;
                    std::optional<std::thread::native_handle_type> handle_;

                    static void wakeIfSleeping(State *s)
                    {
                        //seq_cst load after the seq_cst exchange in queue.push; pairs
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
                        auto budget = s->opts.spinWait;
                        if (hasIdle) {
                            budget = std::min(budget, std::chrono::duration_cast<std::chrono::nanoseconds>(Config::idlePeriod));
                        }
                        if (recentlyActive && infra::realtime_threaded_executor_detail::spinUntil(budget, ready)) {
                            s->wakeRequested.exchange(false, std::memory_order_acq_rel);
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
                        s->wakeRequested.exchange(false, std::memory_order_acq_rel);
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
                            if (!s->queue.maybeNonEmpty()) {
                                //timed out (or woken by wakeIdle) with nothing pending
                                h->executorIdleWork();
                            }
                        }
                    }

                public:
                    static constexpr bool kOwnContext = true;
                    static constexpr bool kSerialCallbacks = true;
                    explicit EnhancedDedicatedThreadedExecutor(ThreadedExecOptions const &opts = ThreadedExecOptions{})
                        : s_(std::make_shared<State>(opts))
                        , handle_(std::nullopt)
                    {
                    }
                    EnhancedDedicatedThreadedExecutor(EnhancedDedicatedThreadedExecutor const &) = delete;
                    EnhancedDedicatedThreadedExecutor &operator=(EnhancedDedicatedThreadedExecutor const &) = delete;
                    ~EnhancedDedicatedThreadedExecutor()
                    {
                        stop();
                    }
                    template <class Host>
                    void start(Host *h)
                    {
                        s_->running.store(true, std::memory_order_release);
                        std::thread th(&EnhancedDedicatedThreadedExecutor::run<Host>, s_, h);
                        handle_ = th.native_handle();
                        //same lifecycle as the old handler bases: the thread is detached
                        th.detach();
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
                        s->queue.push(std::move(x));
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
                    std::optional<std::thread::native_handle_type> threadHandle()
                    {
                        return handle_;
                    }
                };

                struct EnhancedDedicatedThreadedExecutorComponent : infra::RealTimeThreadedExecutorComponent<EnhancedDedicatedThreadedExecutor> {};
            } // namespace basic
        } // namespace tm
    } // namespace cd606
} // namespace dev
#endif
