#ifndef TM_KIT_BASIC_REAL_TIME_CLOCK_TSC_CLOCK_HPP_
#define TM_KIT_BASIC_REAL_TIME_CLOCK_TSC_CLOCK_HPP_

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <x86intrin.h>
#define TM_KIT_BASIC_TSC_CLOCK_HAS_RDTSC 1
#endif

namespace dev { namespace cd606 { namespace tm { namespace basic { namespace real_time_clock {

    //Wall clock (system_clock epoch, nanoseconds) derived from the TSC.
    //
    //A reading is rdtsc plus a seqlock-protected linear map
    //  ns = baseNs + (tsc-baseTsc)*mult/2^32
    //which costs about a third of std::chrono::system_clock::now().
    //
    //The map is re-anchored against system_clock lazily: the first reading
    //that finds more than resyncInterval since the anchor does the re-sync
    //(one paired system_clock read, ~100ns once per interval; other threads
    //keep using the old map meanwhile). The re-sync keeps the clock
    //continuous and slews it towards system_clock (bounded by maxSlewPpm),
    //so within one thread readings never go backwards unless system_clock
    //itself jumped by more than stepThreshold, in which case the clock steps
    //to the new system time, like system_clock does.
    //
    //If the CPU has no invariant TSC (or this is not x86-64), every reading
    //is simply std::chrono::system_clock::now().
    class TscClock {
    public:
        struct Options {
            std::chrono::nanoseconds resyncInterval = std::chrono::milliseconds(100);
            std::chrono::nanoseconds stepThreshold = std::chrono::milliseconds(1);
            double maxSlewPpm = 1000.0;
            std::chrono::nanoseconds initialCalibration = std::chrono::milliseconds(5);
            bool forceSystemClock = false;
        };
        struct Stats {
            uint64_t resyncs;
            uint64_t steps;
            int64_t lastErrorNs; //system_clock minus this clock at the last re-sync
            double ticksPerNs;
        };
    private:
        //hot data, read by every reader: one cache line
        alignas(64) std::atomic<uint64_t> seq_;
        std::atomic<uint64_t> baseTsc_;
        std::atomic<int64_t> baseNs_;
        std::atomic<uint64_t> mult_; //ns per tick in 32.32 fixed point
        uint64_t resyncTicks_;
        bool useTsc_;

        //written only by the thread holding the seqlock (odd seq_)
        alignas(64) Options options_;
        uint64_t calTsc_;
        int64_t calNs_;
        std::atomic<double> ticksPerNs_;
        std::atomic<uint64_t> resyncs_;
        std::atomic<uint64_t> steps_;
        std::atomic<int64_t> lastErrorNs_;

        //returns true if the map was re-anchored (by us)
        bool resync(uint64_t expectedSeq);
        void anchor(uint64_t tsc, int64_t ns, double nsPerTick);
    public:
        TscClock();
        explicit TscClock(Options const &options);
        TscClock(TscClock const &) = delete;
        TscClock &operator=(TscClock const &) = delete;

        //invariant (constant rate, non-stop) TSC reported by cpuid
        static bool hasInvariantTsc();
        //paired read: system_clock ns and the TSC at the middle of the read
        static void pairedSample(uint64_t &tsc, int64_t &ns);
        static int64_t systemNowNs() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        bool usesTsc() const {
            return useTsc_;
        }
        Stats stats() const;

        inline int64_t nowNs() {
#ifdef TM_KIT_BASIC_TSC_CLOCK_HAS_RDTSC
            if (useTsc_) {
                while (true) {
                    uint64_t s = seq_.load(std::memory_order_acquire);
                    if (s & 1) {
                        _mm_pause();
                        continue;
                    }
                    uint64_t const bt = baseTsc_.load(std::memory_order_relaxed);
                    int64_t const bn = baseNs_.load(std::memory_order_relaxed);
                    uint64_t const m = mult_.load(std::memory_order_relaxed);
                    //the tick is read inside the seqlock window: a re-sync that
                    //anchors after this read makes us retry, so a later reading
                    //in the same thread can never use an older map with a newer
                    //tick (which could step backwards when the rate changed)
                    uint64_t const t = __rdtsc();
                    std::atomic_thread_fence(std::memory_order_acquire);
                    if (seq_.load(std::memory_order_relaxed) != s) {
                        continue;
                    }
                    int64_t const d = static_cast<int64_t>(t-bt);
                    if (static_cast<uint64_t>(d) >= resyncTicks_ && d > 0) {
                        if (resync(s)) {
                            continue;
                        }
                    }
                    return bn+static_cast<int64_t>((static_cast<__int128>(d)*static_cast<__int128>(m)) >> 32);
                }
            }
#endif
            return systemNowNs();
        }
        inline std::chrono::system_clock::time_point now() {
            return std::chrono::system_clock::time_point(
                std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(nowNs()))
            );
        }
    };

} } } } }

#endif
