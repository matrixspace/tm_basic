#ifndef TM_KIT_BASIC_SINGLE_PASS_ITERATION_CLOCK_CLOCK_HPP_
#define TM_KIT_BASIC_SINGLE_PASS_ITERATION_CLOCK_CLOCK_HPP_

#include <chrono>
#include <ctime>
#include <optional>

namespace dev { namespace cd606 { namespace tm { namespace basic { namespace single_pass_iteration_clock {
    
    template <class TimePoint, bool AllowLocalTimeOverride=false>
    class Clock;
    
    template <class TimePoint>
    class Clock<TimePoint, true> {
    public:
        using TimePointType = TimePoint;
    private:
        TimePoint latestTime_;
        std::optional<TimePoint> localTime_;
    protected:
        void updateLatestTime(TimePoint const &t) {
            if (t > latestTime_) {
                latestTime_ = t;
            } 
        }
    public:
        TimePoint now() const {
            if (localTime_) {
                return *localTime_;
            }
            return latestTime_;
        }
        void sleepFor(decltype(TimePoint()-TimePoint()) const &duration) {
            latestTime_ += duration;
        }
        void setLocalTime(TimePoint const &t) {
            localTime_ = t;
        }
        void resetLocalTime() {
            localTime_ = std::nullopt;
        }
    };
    template <class TimePoint>
    class Clock<TimePoint, false> {
    public:
        using TimePointType = TimePoint;
    private:
        TimePoint latestTime_;
    protected:
        void updateLatestTime(TimePoint const &t) {
            if (t > latestTime_) {
                latestTime_ = t;
            } 
        }
    public:
        TimePoint now() const {
            return latestTime_;
        }
        void sleepFor(decltype(TimePoint()-TimePoint()) const &duration) {
            latestTime_ += duration;
        }
    };

} } } } }

#endif