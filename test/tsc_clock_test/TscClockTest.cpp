#include <tm_kit/basic/real_time_clock/ClockComponent.hpp>
#include <tm_kit/basic/real_time_clock/TscClock.hpp>

#include <x86intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

//Tests for TscClock / TscClockComponent:
//  1. invariant TSC detection and that the TSC path is actually used
//  2. accuracy against system_clock over ~3s (covers ~30 re-syncs): the
//     error of every sample is bracketed by two system_clock reads
//  3. per-thread monotonicity with 4 reader threads across re-syncs
//  4. monotonicity when every re-sync takes the step branch (system_clock
//     apparently behind the clock half of the time)
//  5. a backward error larger than stepThreshold does not step the clock
//     back (deterministic, using the white-box test access)
//  6. forceSystemClock fallback, including its monotonicity across threads
//     and its high-water mark after a backward system_clock adjustment
//  7. per-call cost compared with system_clock::now()
//  8. TscClockComponent: resolveTime() is real time (not the epoch)

using namespace dev::cd606::tm::basic::real_time_clock;

//white-box access used only by the tests: puts the map into the state a real
//system_clock jump would produce, without changing the machine's clock
namespace dev { namespace cd606 { namespace tm { namespace basic { namespace real_time_clock {
    struct TscClockTestAccess {
        static void moveAhead(TscClock &c, int64_t aheadNs) {
            c.baseNs_.store(c.baseNs_.load(std::memory_order_relaxed)+aheadNs, std::memory_order_relaxed);
        }
        //move the anchor back without changing the mapped value of any tick,
        //so the next read finds d >= resyncTicks_ and re-syncs immediately
        static void makeResyncDue(TscClock &c) {
            double const nsPerTick = 1.0/c.ticksPerNs_.load(std::memory_order_relaxed);
            uint64_t const shift = c.resyncTicks_+1000;
            uint64_t const bt = c.baseTsc_.load(std::memory_order_relaxed);
            int64_t const bn = c.baseNs_.load(std::memory_order_relaxed);
            c.baseTsc_.store(bt-shift, std::memory_order_relaxed);
            c.baseNs_.store(bn-static_cast<int64_t>(static_cast<double>(shift)*nsPerTick), std::memory_order_relaxed);
        }
        static void setFallbackFloor(TscClock &c, int64_t ns) {
            c.lastFallbackNs_.store(ns, std::memory_order_relaxed);
        }
    };
} } } } }

namespace {

    int g_failures = 0;
    void check(bool cond, std::string const &what) {
        if (!cond) {
            ++g_failures;
            std::cout << "FAIL: " << what << std::endl;
        } else {
            std::cout << "ok:   " << what << std::endl;
        }
    }

    int64_t sysNs() {
        return TscClock::systemNowNs();
    }

    void testAccuracy(TscClock &c) {
        //bracket each reading with system_clock reads; the error is the
        //distance from the bracket (0 when inside)
        std::vector<int64_t> errs;
        auto const until = std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < until) {
            for (int ii=0; ii<1000; ++ii) {
                int64_t a = sysNs();
                int64_t t = c.nowNs();
                int64_t b = sysNs();
                int64_t e = 0;
                if (t < a) {
                    e = t-a;
                } else if (t > b) {
                    e = t-b;
                }
                errs.push_back(e);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        std::vector<int64_t> absErr(errs.size());
        std::transform(errs.begin(), errs.end(), absErr.begin(), [](int64_t x) { return std::llabs(x); });
        std::sort(absErr.begin(), absErr.end());
        auto q = [&absErr](double p) {
            return absErr[static_cast<std::size_t>(p*static_cast<double>(absErr.size()-1))];
        };
        auto st = c.stats();
        std::cout << "      accuracy vs system_clock over 3s, " << absErr.size() << " samples: |err| p50 " << q(0.5) << "ns p99 " << q(0.99)
                  << "ns p99.99 " << q(0.9999) << "ns max " << absErr.back() << "ns; resyncs " << st.resyncs << ", steps " << st.steps
                  << ", last resync error " << st.lastErrorNs << "ns, " << st.ticksPerNs << " ticks/ns" << std::endl;
        check(st.resyncs >= 10, "periodic re-sync happened (>=10 in 3s)");
        check(st.steps == 0, "no step needed while system_clock was stable");
        check(q(0.99) <= 1000, "p99 |error| <= 1us");
        //a sample can only exceed this if system_clock itself was slewed hard
        //by NTP or a reader was preempted between its brackets
        check(absErr.back() <= 50000, "max |error| <= 50us");
    }

    //runs kThreads readers for the given duration; returns the number of
    //readings that were lower than the previous reading on the same thread
    int64_t runReaders(TscClock &c, int kThreads, std::chrono::steady_clock::duration duration, int64_t &totalReads) {
        std::atomic<bool> stop{false};
        std::vector<int64_t> backwards(kThreads, 0), reads(kThreads, 0);
        std::vector<std::thread> threads;
        for (int k=0; k<kThreads; ++k) {
            threads.emplace_back([&, k]() {
                int64_t last = c.nowNs();
                while (!stop.load(std::memory_order_relaxed)) {
                    int64_t t = c.nowNs();
                    if (t < last) {
                        ++backwards[k];
                    }
                    last = t;
                    ++reads[k];
                }
            });
        }
        std::this_thread::sleep_for(duration);
        stop = true;
        for (auto &t : threads) {
            t.join();
        }
        int64_t b = 0;
        totalReads = 0;
        for (int k=0; k<kThreads; ++k) {
            b += backwards[k];
            totalReads += reads[k];
        }
        return b;
    }

    void testMonotonic(TscClock &c) {
        constexpr int kThreads = 4;
        uint64_t resyncsBefore = c.stats().resyncs;
        int64_t reads = 0;
        int64_t b = runReaders(c, kThreads, std::chrono::seconds(2), reads);
        std::cout << "      " << reads << " reads on " << kThreads << " threads, " << (c.stats().resyncs-resyncsBefore) << " re-syncs meanwhile" << std::endl;
        check(b == 0, "per-thread readings never go backwards across re-syncs");
    }

    //a 1ns threshold makes nearly every re-sync take the step branch, and the
    //paired samples then fall on both sides of the current mapping, so the
    //backward case (system_clock behind the clock) is exercised constantly:
    //the clock must stay where it is, not step back
    void testMonotonicAcrossSteps() {
        TscClock::Options o;
        o.resyncInterval = std::chrono::milliseconds(1);
        o.stepThreshold = std::chrono::nanoseconds(1);
        TscClock c(o);
        if (!c.usesTsc()) {
            check(false, "step test needs the TSC path");
            return;
        }
        int64_t reads = 0;
        int64_t b = runReaders(c, 2, std::chrono::seconds(1), reads);
        auto st = c.stats();
        std::cout << "      " << reads << " reads, " << st.resyncs << " re-syncs, " << st.steps << " steps, last error "
                  << st.lastErrorNs << "ns" << std::endl;
        check(b == 0, "readings never go backwards when re-syncs step");
        check(st.steps >= 100, "step branch exercised (>=100 steps in 1s)");
    }

    //put the map 100us ahead of system_clock (as a backward system_clock
    //adjustment would leave it relative to a clock already read at that
    //value) and let the next read re-sync: the error is far beyond
    //stepThreshold, and the clock must stay where it is instead of stepping
    //back to system time
    void testBackwardErrorDoesNotStepBack() {
        TscClock::Options o;
        o.stepThreshold = std::chrono::microseconds(10);
        o.resyncInterval = std::chrono::seconds(1);
        TscClock c(o);
        if (!c.usesTsc()) {
            check(false, "backward-step test needs the TSC path");
            return;
        }
        TscClockTestAccess::moveAhead(c, 100000);
        int64_t const v0 = c.nowNs();
        TscClockTestAccess::makeResyncDue(c);
        uint64_t const stepsBefore = c.stats().steps;
        int64_t const v1 = c.nowNs();
        auto st = c.stats();
        std::cout << "      backward error " << st.lastErrorNs << "ns at re-sync, reading delta " << (v1-v0) << "ns" << std::endl;
        check(st.steps == stepsBefore+1, "re-sync took the step branch");
        check(st.lastErrorNs <= -50000, "re-sync saw the backward error");
        check(v1 >= v0, "a backward system_clock error does not move the clock back");
    }

    void testFallback() {
        TscClock::Options o;
        o.forceSystemClock = true;
        TscClock c(o);
        int64_t a = sysNs();
        int64_t t = c.nowNs();
        int64_t b = sysNs();
        check(!c.usesTsc() && t >= a && t <= b, "forceSystemClock falls back to system_clock");

        int64_t reads = 0;
        int64_t bwd = runReaders(c, 4, std::chrono::seconds(1), reads);
        std::cout << "      " << reads << " fallback reads on 4 threads" << std::endl;
        check(bwd == 0, "fallback readings never go backwards");

        //set the high-water mark into the future, as a backward system_clock
        //adjustment would: readings must not drop below the values already
        //handed out
        int64_t const floor = sysNs()+std::chrono::milliseconds(1).count();
        TscClockTestAccess::setFallbackFloor(c, floor);
        int64_t const t2 = c.nowNs();
        check(t2 >= floor, "fallback never returns below its high-water mark");
        check(c.nowNs() >= t2, "fallback readings stay non-decreasing");
    }

    template <class F>
    double costNs(F &&f, double ghz) {
        constexpr int kN = 2000000;
        int64_t sink = 0;
        uint64_t t0 = __rdtsc();
        for (int ii=0; ii<kN; ++ii) {
            sink += f();
        }
        uint64_t t1 = __rdtsc();
        asm volatile("" : : "g"(&sink) : "memory");
        return static_cast<double>(t1-t0)/ghz/kN;
    }

    void testCost(TscClock &c) {
        double ghz = c.stats().ticksPerNs;
        double tsc = costNs([&c]() { return c.nowNs(); }, ghz);
        double sys = costNs([]() { return sysNs(); }, ghz);
        std::cout << "      cost per call (throughput loop): TscClock " << tsc << "ns, system_clock " << sys << "ns" << std::endl;
        check(tsc < sys, "TscClock::nowNs() is cheaper than system_clock::now()");
    }

    void testComponent() {
        TscClockComponent c;
        auto a = std::chrono::system_clock::now();
        auto t = c.resolveTime();
        auto t2 = c.resolveTime(std::chrono::system_clock::time_point{});
        auto b = std::chrono::system_clock::now();
        auto slack = std::chrono::microseconds(50);
        check(t >= a-slack && t <= b+slack && t2 >= t, "TscClockComponent::resolveTime() is real time");
        check(c.now() >= t, "TscClockComponent::now() uses the same clock");
    }

} // namespace

int main() {
    bool inv = TscClock::hasInvariantTsc();
    check(inv, "CPU reports invariant TSC");
    TscClock c;
    check(c.usesTsc() == inv, "TSC path used iff invariant TSC");
    if (c.usesTsc()) {
        testAccuracy(c);
        testMonotonic(c);
        testMonotonicAcrossSteps();
        testBackwardErrorDoesNotStepBack();
        testCost(c);
    }
    testFallback();
    testComponent();
    if (g_failures == 0) {
        std::cout << "ALL PASSED" << std::endl;
        return 0;
    }
    std::cout << g_failures << " FAILURE(S)" << std::endl;
    return 1;
}
