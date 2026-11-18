#ifndef TM_KIT_BASIC_INTERRUPTIBLE_CLOCK_WAIT_HPP_
#define TM_KIT_BASIC_INTERRUPTIBLE_CLOCK_WAIT_HPP_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <type_traits>
#include <utility>

namespace dev
{
    namespace cd606
    {
        namespace tm
        {
            namespace basic
            {
                namespace clock_wait_detail
                {

                    template <class Env, class Duration, class = void>
                    struct HasActualDuration : std::false_type {};

                    template <class Env, class Duration>
                    struct HasActualDuration<Env, Duration, std::void_t<decltype(std::declval<Env &>().actualDuration(std::declval<Duration const &>()))>> : std::true_type {};

                    // The caller changes running under mutex before notifying cond.
                    template <class Env, class Duration>
                    void wait(Env *env, Duration duration, std::mutex &mutex, std::condition_variable &cond, std::atomic<bool> const &running)
                    {
                        if constexpr (HasActualDuration<Env, Duration>::value) {
                            auto actual = env->actualDuration(duration);
                            std::unique_lock<std::mutex> lock(mutex);
                            cond.wait_for(lock, actual, [&running] { return !running; });
                        } else {
                            // Legacy environments expose only sleepFor. Keep the clock's
                            // scaling semantics and check for stop between virtual-time slices.
                            auto const slice = std::max(Duration{1}, std::chrono::duration_cast<Duration>(std::chrono::milliseconds(10)));
                            while (running && duration > Duration::zero()) {
                                auto const step = std::min(duration, slice);
                                env->sleepFor(step);
                                duration -= step;
                            }
                        }
                    }

                } // namespace clock_wait_detail
            } // namespace basic
        } // namespace tm
    } // namespace cd606
} // namespace dev

#endif
