#ifndef TM_KIT_BASIC_REAL_TIME_CLOCK_CLOCK_COMPONENT_HPP_
#define TM_KIT_BASIC_REAL_TIME_CLOCK_CLOCK_COMPONENT_HPP_

#include <functional>
#include <memory>
#include <tm_kit/basic/real_time_clock/Clock.hpp>
#include <tm_kit/basic/real_time_clock/TscClock.hpp>

namespace dev { namespace cd606 { namespace tm { namespace basic { namespace real_time_clock {
    class ClockComponentImpl;

    class ClockComponent : public Clock {
    private:
        std::unique_ptr<ClockComponentImpl> impl_;
    public:
        using TimePointType = std::chrono::system_clock::time_point;
        using DurationType = std::chrono::system_clock::duration;
        static constexpr bool PreserveInputRelativeOrder = false;
        static constexpr bool CanBeActualTimeClock = true;

        ClockComponent();
        ClockComponent(Clock::ClockSettings const &);
        ClockComponent(ClockComponent &&);
        ClockComponent &operator=(ClockComponent &&);
        ~ClockComponent();
        
        TimePointType resolveTime() {
            return Clock::now();
        }
        TimePointType resolveTime(TimePointType const &/*_triggeringInputTime*/) {
            return Clock::now();
        }
        //the functions returned are cancellors
        std::function<void()> createOneShotTimer(TimePointType const &fireAtTime, std::function<void()> callback);
        std::function<void()> createOneShotDurationTimer(DurationType const &fireAfterDuration, std::function<void()> callback);
        std::function<void()> createRecurringTimer(TimePointType const &firstFireAtTime, TimePointType const &lastFireAtTime, DurationType const &period, std::function<void()> callback);
        std::function<void()> createVariableDurationRecurringTimer(TimePointType const &firstFireAtTime, TimePointType const &lastFireAtTime, std::function<DurationType(TimePointType const &)> periodCalc, std::function<void()> callback);
    };

    class SimplifiedClockComponent : public Clock {
    private:
        std::unique_ptr<ClockComponentImpl> impl_;
    public:
        using TimePointType = std::chrono::system_clock::time_point;
        using DurationType = std::chrono::system_clock::duration;
        static constexpr bool PreserveInputRelativeOrder = false;
        static constexpr bool CanBeActualTimeClock = true;

        SimplifiedClockComponent();
        SimplifiedClockComponent(SimplifiedClockComponent &&);
        SimplifiedClockComponent &operator=(SimplifiedClockComponent &&);
        ~SimplifiedClockComponent();
        
        static TimePointType resolveTime() {
            return TimePointType {};
        }
        static TimePointType resolveTime(TimePointType const &triggeringInputTime) {
            return triggeringInputTime;
        }
        //the functions returned are cancellors
        std::function<void()> createOneShotTimer(TimePointType const &fireAtTime, std::function<void()> callback);
        std::function<void()> createOneShotDurationTimer(DurationType const &fireAfterDuration, std::function<void()> callback);
        std::function<void()> createRecurringTimer(TimePointType const &firstFireAtTime, TimePointType const &lastFireAtTime, DurationType const &period, std::function<void()> callback);
        std::function<void()> createVariableDurationRecurringTimer(TimePointType const &firstFireAtTime, TimePointType const &lastFireAtTime, std::function<DurationType(TimePointType const &)> periodCalc, std::function<void()> callback);
    };

    //Same as ClockComponent (actual time only, no virtual clock settings),
    //but resolveTime() and now() read a TSC-based wall clock (see TscClock.hpp)
    //instead of calling system_clock::now(). Timestamps stay real time; they
    //track system_clock to within the re-sync error reported by tscStats().
    //Timers are unchanged (system_clock based).
    class TscClockComponent : public ClockComponent {
    private:
        std::unique_ptr<TscClock> tsc_;
    public:
        TscClockComponent() : ClockComponent(), tsc_(std::make_unique<TscClock>()) {}
        explicit TscClockComponent(TscClock::Options const &options) : ClockComponent(), tsc_(std::make_unique<TscClock>(options)) {}
        TscClockComponent(TscClockComponent &&) = default;
        TscClockComponent &operator=(TscClockComponent &&) = default;
        ~TscClockComponent() = default;

        TimePointType now() {
            return tsc_->now();
        }
        TimePointType resolveTime() {
            return tsc_->now();
        }
        TimePointType resolveTime(TimePointType const &/*_triggeringInputTime*/) {
            return tsc_->now();
        }
        TscClock::Stats tscStats() const {
            return tsc_->stats();
        }
        bool usesTsc() const {
            return tsc_->usesTsc();
        }
    };

} } } } }

#endif