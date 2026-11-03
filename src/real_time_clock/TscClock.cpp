#include <tm_kit/basic/real_time_clock/TscClock.hpp>

#include <algorithm>
#include <cmath>
#include <thread>

#ifdef TM_KIT_BASIC_TSC_CLOCK_HAS_RDTSC
#include <cpuid.h>
#endif

namespace dev { namespace cd606 { namespace tm { namespace basic { namespace real_time_clock {

    namespace {
        inline uint64_t multFor(double nsPerTick) {
            return static_cast<uint64_t>(std::llround(nsPerTick*4294967296.0));
        }
    }

    bool TscClock::hasInvariantTsc() {
#ifdef TM_KIT_BASIC_TSC_CLOCK_HAS_RDTSC
        unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
        if (__get_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx) == 0 || eax < 0x80000007u) {
            return false;
        }
        if (__get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx) == 0) {
            return false;
        }
        return (edx & (1u << 8)) != 0;
#else
        return false;
#endif
    }

    void TscClock::pairedSample(uint64_t &tsc, int64_t &ns) {
#ifdef TM_KIT_BASIC_TSC_CLOCK_HAS_RDTSC
        //keep the tightest of a few brackets so that a preemption or an
        //interrupt inside one of them does not bias the anchor
        uint64_t bestWidth = UINT64_MAX;
        for (int ii=0; ii<5; ++ii) {
            uint64_t const t0 = __rdtsc();
            int64_t const n = systemNowNs();
            uint64_t const t1 = __rdtsc();
            uint64_t const w = t1-t0;
            if (w < bestWidth) {
                bestWidth = w;
                tsc = t0+w/2;
                ns = n;
            }
        }
#else
        tsc = 0;
        ns = systemNowNs();
#endif
    }

    TscClock::TscClock() : TscClock(Options {}) {}

    TscClock::TscClock(Options const &options)
        : seq_(0)
        , baseTsc_(0)
        , baseNs_(0)
        , mult_(0)
        , resyncTicks_(UINT64_MAX)
        , useTsc_(false)
        , options_(options)
        , calTsc_(0)
        , calNs_(0)
        , ticksPerNs_(0.0)
        , resyncs_(0)
        , steps_(0)
        , lastErrorNs_(0)
        , lastFallbackNs_(INT64_MIN)
    {
#ifdef TM_KIT_BASIC_TSC_CLOCK_HAS_RDTSC
        if (options_.forceSystemClock || !hasInvariantTsc()) {
            return;
        }
        uint64_t t0, t1;
        int64_t n0, n1;
        pairedSample(t0, n0);
        std::this_thread::sleep_for(options_.initialCalibration);
        pairedSample(t1, n1);
        if (t1 <= t0 || n1 <= n0) {
            return;
        }
        double const tpn = static_cast<double>(t1-t0)/static_cast<double>(n1-n0);
        calTsc_ = t0;
        calNs_ = n0;
        ticksPerNs_.store(tpn, std::memory_order_relaxed);
        resyncTicks_ = static_cast<uint64_t>(static_cast<double>(options_.resyncInterval.count())*tpn);
        anchor(t1, n1, 1.0/tpn);
        useTsc_ = true;
#endif
    }

    void TscClock::anchor(uint64_t tsc, int64_t ns, double nsPerTick) {
        baseTsc_.store(tsc, std::memory_order_relaxed);
        baseNs_.store(ns, std::memory_order_relaxed);
        mult_.store(multFor(nsPerTick), std::memory_order_relaxed);
    }

    bool TscClock::resync(uint64_t expectedSeq) {
        //only one thread re-anchors; the others keep using the current map
        if (!seq_.compare_exchange_strong(expectedSeq, expectedSeq+1, std::memory_order_acquire, std::memory_order_relaxed)) {
            return false;
        }
        std::atomic_thread_fence(std::memory_order_release);

        uint64_t tsc;
        int64_t sysNs;
        pairedSample(tsc, sysNs);
        uint64_t const bt = baseTsc_.load(std::memory_order_relaxed);
        int64_t const bn = baseNs_.load(std::memory_order_relaxed);
        uint64_t const m = mult_.load(std::memory_order_relaxed);
        int64_t const d = static_cast<int64_t>(tsc-bt);
        int64_t const cur = bn+static_cast<int64_t>((static_cast<__int128>(d)*static_cast<__int128>(m)) >> 32);
        int64_t const err = sysNs-cur;
        lastErrorNs_.store(err, std::memory_order_relaxed);

        if (std::llabs(err) > options_.stepThreshold.count() || tsc <= calTsc_ || sysNs <= calNs_) {
            //system_clock jumped (or was set), or the sample cannot extend the
            //rate baseline: restart the rate estimate from here, keeping the
            //last known rate meanwhile. anchor at max(cur,sysNs) so that a
            //backward jump of system_clock is ignored instead of stepping the
            //clock back: readings must never decrease
            calTsc_ = tsc;
            calNs_ = sysNs;
            anchor(tsc, std::max(cur, sysNs), 1.0/ticksPerNs_.load(std::memory_order_relaxed));
            steps_.fetch_add(1, std::memory_order_relaxed);
        } else {
            //long-baseline rate estimate, then slew so that err is absorbed
            //over the next interval, never faster than maxSlewPpm
            double const tpn = static_cast<double>(tsc-calTsc_)/static_cast<double>(sysNs-calNs_);
            ticksPerNs_.store(tpn, std::memory_order_relaxed);
            double const interval = static_cast<double>(options_.resyncInterval.count());
            double const maxSlew = options_.maxSlewPpm*1e-6;
            double const slew = std::clamp(static_cast<double>(err)/interval, -maxSlew, maxSlew);
            anchor(tsc, cur, (1.0/tpn)*(1.0+slew));
        }
        resyncs_.fetch_add(1, std::memory_order_relaxed);
        seq_.store(expectedSeq+2, std::memory_order_release);
        return true;
    }

    TscClock::Stats TscClock::stats() const {
        return Stats {
            resyncs_.load(std::memory_order_relaxed)
            , steps_.load(std::memory_order_relaxed)
            , lastErrorNs_.load(std::memory_order_relaxed)
            , ticksPerNs_.load(std::memory_order_relaxed)
        };
    }

} } } } }
