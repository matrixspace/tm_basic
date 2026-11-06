#ifndef TM_KIT_BASIC_MOODYCAMEL_THREADED_EXECUTOR_HPP_
#define TM_KIT_BASIC_MOODYCAMEL_THREADED_EXECUTOR_HPP_

#include <tm_kit/infra/RealTimeThreadedExecutor.hpp>

#if __has_include(<concurrentqueue/moodycamel/blockingconcurrentqueue.h>)
#include <concurrentqueue/moodycamel/blockingconcurrentqueue.h>
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#define TM_KIT_BASIC_HAS_MOODYCAMEL_THREADED_EXECUTOR 1
#elif __has_include(<moodycamel/blockingconcurrentqueue.h>)
#include <moodycamel/blockingconcurrentqueue.h>
#include <moodycamel/concurrentqueue.h>
#define TM_KIT_BASIC_HAS_MOODYCAMEL_THREADED_EXECUTOR 1
#endif

#ifdef TM_KIT_BASIC_HAS_MOODYCAMEL_THREADED_EXECUTOR

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

//Optional executor for RealTimeApp threaded handlers backed by moodycamel's
//ConcurrentQueue / BlockingConcurrentQueue (vcpkg port "concurrentqueue").
//It implements the executor contract of tm_kit/infra/RealTimeThreadedExecutor.hpp.
//Enable it for every threaded handler of an environment with
//    struct MyEnv : ..., basic::MoodycamelThreadedExecutorComponent {};
//
//Differences from the default infra::DedicatedThreadExecutor:
//  - ORDERING (contract 2, weaker form): items pushed by one thread are
//    handled in push order, but items from DIFFERENT producer threads are
//    NOT kept in their global push order. A threaded handler doing a
//    monotonic time check (CheckTime=true, e.g. threaded actions) may
//    therefore drop more out-of-order data when fed by several threads.
//    Prefer the default executor for such nodes.
//  - the consumer dequeues in bulk (up to kBulk items per wake-up)
//  - wakeIdle() / stop() enqueue an empty wake token; it costs one queue slot
//  - the pending count passed to executorBeforeBatch is size_approx()
//Like the default executor it uses one detached thread per node, and its
//state is shared with that thread so the executor may be destroyed first.

namespace dev
{
    namespace cd606
    {
        namespace tm
        {
            namespace basic
            {

                namespace moodycamel_threaded_executor_detail
                {
                    //an empty optional is a wake token (wakeIdle / stop)
                    template <class Item>
                    using Slot = std::optional<Item>;

                    template <class Item>
                    struct State {
                        using Queue = moodycamel::BlockingConcurrentQueue<Slot<Item>>;
                        infra::ThreadedExecOptions opts;
                        Queue queue;
                        alignas(64) std::atomic<bool> running{false};
                        explicit State(infra::ThreadedExecOptions const &o)
                            : opts(o)
                            , queue()
                        {
                        }
                    };
                } // namespace moodycamel_threaded_executor_detail

                template <class Item>
                class MoodycamelThreadedExecutor {
                private:
                    using State = moodycamel_threaded_executor_detail::State<Item>;
                    using Slot = moodycamel_threaded_executor_detail::Slot<Item>;
                    static constexpr std::size_t kBulk = 64;

                    std::shared_ptr<State> s_;
                    std::optional<std::thread::native_handle_type> handle_;

                    //handles one dequeued bulk; returns true when at least one real item
                    //was in it
                    template <class Host>
                    static bool handleBulk(State *s, Host *h, Slot *buf, std::size_t n)
                    {
                        bool hadItem = false;
                        bool reported = false;
                        for (std::size_t i = 0; i < n; ++i) {
                            if (!buf[i]) {
                                continue;
                            }
                            hadItem = true;
                            //contract 3: after stop() nothing new is handled, the rest
                            //of the bulk is dropped
                            if (s->running.load(std::memory_order_acquire)) {
                                if (s->opts.trackPendingCount && !reported) {
                                    reported = true;
                                    h->executorBeforeBatch(*buf[i], (n - i) + s->queue.size_approx());
                                }
                                h->executorHandle(std::move(*buf[i]));
                            }
                            buf[i].reset();
                        }
                        return hadItem;
                    }
                    template <class Host>
                    static void run(std::shared_ptr<State> s, Host *h)
                    {
                        h->executorWaitForStart();
                        std::vector<Slot> buf(kBulk);
                        while (s->running.load(std::memory_order_acquire)) {
                            //drain without blocking first, then wait
                            std::size_t n = s->queue.try_dequeue_bulk(buf.begin(), kBulk);
                            if (n == 0) {
                                if (h->executorHasIdleWork()) {
                                    auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::milliseconds(1)).count();
                                    n = s->queue.wait_dequeue_bulk_timed(buf.begin(), kBulk, us > 0 ? us : 1);
                                } else {
                                    //no idle work: block until an item or a wake token
                                    n = s->queue.wait_dequeue_bulk(buf.begin(), kBulk);
                                }
                            }
                            bool const hadItem = handleBulk(s.get(), h, buf.data(), n);
                            if (!s->running.load(std::memory_order_acquire)) {
                                return;
                            }
                            //contract 4: idle work only after a timeout / wake token
                            //with nothing handled, and only when nothing is visible
                            //in the queue
                            if (!hadItem && s->queue.size_approx() == 0 && h->executorHasIdleWork()) {
                                h->executorIdleWork();
                            }
                        }
                    }
                    void enqueueWakeToken()
                    {
                        s_->queue.enqueue(Slot{});
                    }

                public:
                    static constexpr bool kOwnContext = true;
                    static constexpr bool kSerialCallbacks = true;
                    explicit MoodycamelThreadedExecutor(infra::ThreadedExecOptions const &opts = infra::ThreadedExecOptions{})
                        : s_(std::make_shared<State>(opts))
                        , handle_(std::nullopt)
                    {
                    }
                    MoodycamelThreadedExecutor(MoodycamelThreadedExecutor const &) = delete;
                    MoodycamelThreadedExecutor &operator=(MoodycamelThreadedExecutor const &) = delete;
                    ~MoodycamelThreadedExecutor()
                    {
                        stop();
                    }
                    template <class Host>
                    void start(Host *h)
                    {
                        s_->running.store(true, std::memory_order_release);
                        std::thread th(&MoodycamelThreadedExecutor::run<Host>, s_, h);
                        handle_ = th.native_handle();
                        th.detach();
                    }
                    bool push(Item &&x)
                    {
                        if (!s_->running.load(std::memory_order_acquire)) {
                            return false;
                        }
                        //enqueue allocates when needed and only fails on allocation failure
                        return s_->queue.enqueue(Slot{std::move(x)});
                    }
                    void stop()
                    {
                        if (s_->running.exchange(false, std::memory_order_acq_rel)) {
                            enqueueWakeToken();
                        }
                    }
                    void wakeIdle()
                    {
                        if (s_->running.load(std::memory_order_acquire)) {
                            enqueueWakeToken();
                        }
                    }
                    std::optional<std::thread::native_handle_type> threadHandle()
                    {
                        return handle_;
                    }
                };

                struct MoodycamelThreadedExecutorComponent : public infra::RealTimeThreadedExecutorComponent<MoodycamelThreadedExecutor> {};

            } // namespace basic
        } // namespace tm
    } // namespace cd606
} // namespace dev

#endif //TM_KIT_BASIC_HAS_MOODYCAMEL_THREADED_EXECUTOR

#endif
