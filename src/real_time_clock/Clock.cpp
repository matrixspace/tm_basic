#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

#include <tm_kit/infra/ChronoUtils.hpp>
#include <tm_kit/basic/real_time_clock/Clock.hpp>

namespace dev { namespace cd606 { namespace tm { namespace basic { namespace real_time_clock {
    Clock::Clock() : settings_(std::nullopt) {}
    Clock::Clock(Clock::ClockSettings const &s) : settings_(s) {}

    namespace {
        // Helper function to parse virtual time from date and time strings
        std::chrono::system_clock::time_point parseVirtualTime(
            std::string const &yyyymmdd,
            std::string const &hhmm
        ) {
            std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm *m = std::localtime(&t);
            std::vector<std::string> parts;

            // Parse date if provided
            std::string dateCopy = yyyymmdd;
            boost::trim(dateCopy);
            if (dateCopy != "") {
                try {
                    boost::split(parts, dateCopy, boost::is_any_of("-"));
                    if (parts.size() != 3) {
                        throw std::invalid_argument("Virtual date must be in YYYY-MM-DD format, got: " + dateCopy);
                    }
                    m->tm_year = boost::lexical_cast<int>(parts[0]) - 1900;
                    m->tm_mon = boost::lexical_cast<int>(parts[1]) - 1;
                    m->tm_mday = boost::lexical_cast<int>(parts[2]);

                    // Validate date ranges
                    if (m->tm_mon < 0 || m->tm_mon > 11) {
                        throw std::invalid_argument("Virtual date month must be 1-12, got: " + parts[1]);
                    }
                    if (m->tm_mday < 1 || m->tm_mday > 31) {
                        throw std::invalid_argument("Virtual date day must be 1-31, got: " + parts[2]);
                    }
                    parts.clear();
                } catch (boost::bad_lexical_cast const &e) {
                    throw std::invalid_argument("Invalid virtual date format: " + dateCopy + " (" + e.what() + ")");
                }
            }

            // Parse time
            std::string timeCopy = hhmm;
            boost::trim(timeCopy);
            if (timeCopy.empty()) {
                throw std::invalid_argument("Virtual time cannot be empty");
            }
            try {
                boost::split(parts, timeCopy, boost::is_any_of(":"));
                if (parts.empty() || parts.size() > 3) {
                    throw std::invalid_argument("Virtual time must be in HH:MM or HH:MM:SS format, got: " + timeCopy);
                }
                int hour = boost::lexical_cast<int>(parts[0]);
                int min = (parts.size() > 1) ? boost::lexical_cast<int>(parts[1]) : 0;

                // Validate time ranges
                if (hour < 0 || hour > 23) {
                    throw std::invalid_argument("Virtual time hour must be 0-23, got: " + parts[0]);
                }
                if (min < 0 || min > 59) {
                    throw std::invalid_argument("Virtual time minute must be 0-59, got: " + parts[1]);
                }

                m->tm_hour = hour;
                m->tm_min = min;
                m->tm_sec = 0;
            } catch (boost::bad_lexical_cast const &e) {
                throw std::invalid_argument("Invalid virtual time format: " + timeCopy + " (" + e.what() + ")");
            }
            m->tm_isdst = -1;

            t = std::mktime(m);
            if (t == -1) {
                throw std::invalid_argument("Invalid virtual date/time combination");
            }
            return std::chrono::system_clock::from_time_t(t);
        }

        // Helper function to calculate next aligned time point
        std::chrono::system_clock::time_point calculateNextAlignedTime(int alignMinute) {
            if (alignMinute <= 0) {
                throw std::invalid_argument("actual_align_minute must be positive, got " + std::to_string(alignMinute));
            }
            if (alignMinute > 60) {
                throw std::invalid_argument("actual_align_minute must be <= 60, got " + std::to_string(alignMinute));
            }

            std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm *m = std::localtime(&t);

            int min = m->tm_min;
            if (min % alignMinute == 0) {
                min += alignMinute;
            } else {
                min = ((min / alignMinute) + 1) * alignMinute;
            }

            if (min >= 60) {
                m->tm_hour += 1;
                m->tm_min = min % 60;
                if (m->tm_hour >= 24) {
                    t += 24 * 3600;
                    m = std::localtime(&t);
                    m->tm_hour = 0;
                    m->tm_min = min % 60;
                }
            } else {
                m->tm_min = min;
            }
            m->tm_sec = 0;

            t = std::mktime(m);
            return std::chrono::system_clock::from_time_t(t);
        }

        // Helper function to parse fixed actual time (HH:MM format)
        std::chrono::system_clock::time_point parseFixedActualTime(std::string const &hhmm) {
            std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm *m = std::localtime(&t);

            std::string timeCopy = hhmm;
            boost::trim(timeCopy);
            if (timeCopy.empty()) {
                throw std::invalid_argument("Actual time cannot be empty");
            }

            std::vector<std::string> parts;
            try {
                boost::split(parts, timeCopy, boost::is_any_of(":"));
                if (parts.empty() || parts.size() > 3) {
                    throw std::invalid_argument("Actual time must be in HH:MM or HH:MM:SS format, got: " + timeCopy);
                }

                int hour = boost::lexical_cast<int>(parts[0]);
                int min = (parts.size() > 1) ? boost::lexical_cast<int>(parts[1]) : 0;

                // Validate time ranges
                if (hour < 0 || hour > 23) {
                    throw std::invalid_argument("Actual time hour must be 0-23, got: " + parts[0]);
                }
                if (min < 0 || min > 59) {
                    throw std::invalid_argument("Actual time minute must be 0-59, got: " + parts[1]);
                }

                m->tm_hour = hour;
                m->tm_min = min;
                m->tm_sec = 0;
            } catch (boost::bad_lexical_cast const &e) {
                throw std::invalid_argument("Invalid actual time format: " + timeCopy + " (" + e.what() + ")");
            }

            t = std::mktime(m);
            if (t == -1) {
                throw std::invalid_argument("Invalid actual time");
            }
            return std::chrono::system_clock::from_time_t(t);
        }

        Clock::ClockSettings createClockSettings(boost::property_tree::ptree &tree) {
            std::string yyyymmddVirtual = tree.get<std::string>("clock_settings.virtual.date");
            std::string hhmmVirtual = tree.get<std::string>("clock_settings.virtual.time");
            double speed = tree.get<double>("clock_settings.speed");

            Clock::ClockSettings ret;

            // Parse actual time: alignment mode or fixed time mode
            auto actualAlignMinuteOpt = tree.get_optional<int>("clock_settings.actual_align_minute");
            if (actualAlignMinuteOpt) {
                ret.synchronizationPointActual = calculateNextAlignedTime(*actualAlignMinuteOpt);
            } else {
                std::string hhmmActual = tree.get<std::string>("clock_settings.actual.time");
                ret.synchronizationPointActual = parseFixedActualTime(hhmmActual);
            }

            // Parse virtual time (same for both modes)
            ret.synchronizationPointVirtual = parseVirtualTime(yyyymmddVirtual, hhmmVirtual);

            ret.speed = speed;

            return ret;
        }
    }

    Clock::ClockSettings Clock::loadClockSettingsFromJSONFile(std::string const &fileName) {
        boost::property_tree::ptree tree;
        boost::property_tree::read_json(fileName, tree);
        return createClockSettings(tree);
    }
    Clock::ClockSettings Clock::loadClockSettingsFromJSONString(std::string const &jsonContent) {
        boost::property_tree::ptree tree;
        std::stringstream ss;
        ss << jsonContent;
        boost::property_tree::read_json(ss, tree);
        return createClockSettings(tree);
    }

    Clock::ClockSettings Clock::clockSettingsWithStartPoint(int actualHHMM, std::string const &virtualStartPoint, double speed) {
        Clock::ClockSettings ret;

        std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm *m = std::localtime(&t);
        m->tm_hour = actualHHMM/100;
        m->tm_min = actualHHMM%100;
        m->tm_sec = 0;
        t = std::mktime(m);
        ret.synchronizationPointActual = std::chrono::system_clock::from_time_t(t);

        ret.synchronizationPointVirtual = infra::withtime_utils::parseLocalTime(virtualStartPoint);
        ret.speed = speed;

        return ret;
    }
    Clock::ClockSettings Clock::clockSettingsWithStartPointCorrespondingToNextAlignment(int actualTimeAlignMinute, std::string const &virtualStartPoint, double speed) {
        Clock::ClockSettings ret;

        ret.synchronizationPointActual = calculateNextAlignedTime(actualTimeAlignMinute);
        ret.synchronizationPointVirtual = infra::withtime_utils::parseLocalTime(virtualStartPoint);
        ret.speed = speed;

        return ret;
    }

    namespace {
        std::chrono::system_clock::time_point actualToVirtual(Clock::ClockSettings const &s, std::chrono::system_clock::time_point const &t) {
            auto d = t-s.synchronizationPointActual;
            d *= s.speed;
            return s.synchronizationPointVirtual+d;
        }
        std::chrono::system_clock::time_point virtualToActual(Clock::ClockSettings const &s, std::chrono::system_clock::time_point const &t) {
            auto d = t-s.synchronizationPointVirtual;
            d /= s.speed;
            return s.synchronizationPointActual+d;
        }
    }

    std::chrono::system_clock::time_point Clock::now() {
        if (settings_) {
            return actualToVirtual(*settings_, std::chrono::system_clock::now());
        } else {
            return std::chrono::system_clock::now();
        }
    }

    uint64_t Clock::currentTimeMillis() {
        return infra::withtime_utils::sinceEpoch<std::chrono::milliseconds>(now());
    }

    void Clock::sleepFor(std::chrono::system_clock::duration const &virtualDuration) {
        std::this_thread::sleep_for(actualDuration(virtualDuration));
    }

    std::chrono::system_clock::time_point Clock::actualTime(std::chrono::system_clock::time_point const &virtualT) {
        if (settings_) {
            return virtualToActual(*settings_, virtualT);
        } else {
            return virtualT;
        }
    }
    std::chrono::system_clock::time_point Clock::virtualTime(std::chrono::system_clock::time_point const &actualT) {
        if (settings_) {
            return actualToVirtual(*settings_, actualT);
        } else {
            return actualT;
        }        
    }
    std::chrono::system_clock::duration Clock::actualDuration(std::chrono::system_clock::duration const &virtualD) {
        if (settings_) {
            return std::chrono::duration_cast<std::chrono::system_clock::duration>(virtualD/settings_->speed);
        } else {
            return virtualD;
        }          
    }
    std::chrono::system_clock::duration Clock::virtualDuration(std::chrono::system_clock::duration const &actualD) {
        if (settings_) {
            return std::chrono::duration_cast<std::chrono::system_clock::duration>(actualD*settings_->speed);
        } else {
            return actualD;
        } 
    }
} } } } }