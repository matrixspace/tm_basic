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

// Tests for TscClock / TscClockComponent:
//   1. invariant TSC detection and that the TSC path is actually used
//   2. accuracy against system_clock over ~3s (covers ~30 re-syncs): the
//      error of every sample is bracketed by two system_clock reads
//   3. per-thread monotonicity with 4 reader threads across re-syncs
//   4. forceSystemClock fallback
//   5. per-call cost compared with system_clock::now()
//   6. TscClockComponent: resolveTime() is real time (not the epoch)

using namespace dev::cd606::tm::basic::real_time_clock;

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
        // bracket each reading with system_clock reads; the error is the
        // distance from the bracket (0 when inside)
        std::vector<int64_t> errs;
        auto const until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < until) {
            for (int ii = 0; ii < 1000; ++ii) {
                int64_t a = sysNs();
                int64_t t = c.nowNs();
                int64_t b = sysNs();
                int64_t e = 0;
                if (t < a) {
                    e = t - a;
                } else if (t > b) {
                    e = t - b;
                }
                errs.push_back(e);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        std::vector<int64_t> absErr(errs.size());
        std::transform(errs.begin(), errs.end(), absErr.begin(), [](int64_t x) { return std::llabs(x); });
        std::sort(absErr.begin(), absErr.end());
        auto q = [&absErr](double p) {
            return absErr[static_cast<std::size_t>(p * static_cast<double>(absErr.size() - 1))];
        };
        auto st = c.stats();
        std::cout << "      accuracy vs system_clock over 3s, " << absErr.size() << " samples: |err| p50 " << q(0.5) << "ns p99 " << q(0.99)
                  << "ns p99.99 " << q(0.9999) << "ns max " << absErr.back() << "ns; resyncs " << st.resyncs << ", steps " << st.steps
                  << ", last resync error " << st.lastErrorNs << "ns, " << st.ticksPerNs << " ticks/ns" << std::endl;
        check(st.resyncs >= 10, "periodic re-sync happened (>=10 in 3s)");
        check(st.steps == 0, "no step needed while system_clock was stable");
        check(q(0.99) <= 1000, "p99 |error| <= 1us");
        // a sample can only exceed this if system_clock itself was slewed hard
        // by NTP or a reader was preempted between its brackets
        check(absErr.back() <= 50000, "max |error| <= 50us");
    }

    void testMonotonic(TscClock &c) {
        constexpr int kThreads = 4;
        std::atomic<bool> stop{false};
        std::vector<int64_t> backwards(kThreads, 0), reads(kThreads, 0);
        std::vector<std::thread> threads;
        uint64_t resyncsBefore = c.stats().resyncs;
        for (int k = 0; k < kThreads; ++k) {
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
        std::this_thread::sleep_for(std::chrono::seconds(2));
        stop = true;
        for (auto &t : threads) {
            t.join();
        }
        int64_t b = 0, n = 0;
        for (int k = 0; k < kThreads; ++k) {
            b += backwards[k];
            n += reads[k];
        }
        std::cout << "      " << n << " reads on " << kThreads << " threads, " << (c.stats().resyncs - resyncsBefore) << " re-syncs meanwhile" << std::endl;
        check(b == 0, "per-thread readings never go backwards across re-syncs");
    }

    void testFallback() {
        TscClock::Options o;
        o.forceSystemClock = true;
        TscClock c(o);
        int64_t a = sysNs();
        int64_t t = c.nowNs();
        int64_t b = sysNs();
        check(!c.usesTsc() && t >= a && t <= b, "forceSystemClock falls back to system_clock");
    }

    template <class F>
    double costNs(F &&f, double ghz) {
        constexpr int kN = 2000000;
        int64_t sink = 0;
        uint64_t t0 = __rdtsc();
        for (int ii = 0; ii < kN; ++ii) {
            sink += f();
        }
        uint64_t t1 = __rdtsc();
        asm volatile("" : : "g"(&sink) : "memory");
        return static_cast<double>(t1 - t0) / ghz / kN;
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
        check(t >= a - slack && t <= b + slack && t2 >= t, "TscClockComponent::resolveTime() is real time");
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
