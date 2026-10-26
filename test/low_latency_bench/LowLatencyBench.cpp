#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>

#include <tm_kit/basic/ByteData.hpp>
#include <tm_kit/basic/SpdLoggingComponent.hpp>
#include <tm_kit/basic/real_time_clock/ClockComponent.hpp>
// TscClockComponent lives in ClockComponent.hpp; probe its own header so the
// bench still builds against a tm_basic without it (for before/after runs)
#if __has_include(<tm_kit/basic/real_time_clock/TscClock.hpp>)
#define LOW_LATENCY_BENCH_HAS_TSC_CLOCK 1
#endif

#include <time.h>
#include <x86intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// In-process micro benchmark of the per-message costs of the TM Kit realtime
// hot path (importer publish -> Producer::publish -> exporter), and of the
// building blocks it is made of (clock reads, TimeChecker, std::function).
//
// Each sample brackets ONE call with lfence/rdtsc .. rdtscp/lfence; the
// reported "net" value subtracts the p50 of an empty bracket. Results are
// noisy when other processes run on the machine; pin with taskset and
// compare variants of the same run only.

using namespace dev::cd606::tm;

namespace {

    inline uint64_t stampBegin() {
        _mm_lfence();
        uint64_t t = __rdtsc();
        _mm_lfence();
        return t;
    }
    inline uint64_t stampEnd() {
        unsigned int aux;
        uint64_t t = __rdtscp(&aux);
        _mm_lfence();
        return t;
    }
    template <class T>
    inline void doNotOptimize(T const &v) {
        asm volatile("" : : "g"(&v) : "memory");
    }

    double calibrateTscGHz() {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC_RAW, &a);
        uint64_t t0 = __rdtsc();
        struct timespec req{0, 200000000};
        nanosleep(&req, nullptr);
        clock_gettime(CLOCK_MONOTONIC_RAW, &b);
        uint64_t t1 = __rdtsc();
        double ns = (b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
        return static_cast<double>(t1 - t0) / ns;
    }

    struct Result {
        double p50;
        double p90;
        double p99;
        double mean;
    };

    double g_ghz = 2.0;
    double g_emptyP50 = 0.0;
    std::size_t g_n = 1000000;

    Result summarize(std::vector<uint64_t> &d) {
        std::sort(d.begin(), d.end());
        double sum = 0;
        for (auto x : d) {
            sum += static_cast<double>(x);
        }
        auto at = [&d](double q) {
            return static_cast<double>(d[static_cast<std::size_t>(q * static_cast<double>(d.size() - 1))]) / g_ghz;
        };
        return {at(0.5), at(0.9), at(0.99), sum / static_cast<double>(d.size()) / g_ghz};
    }

    void print(char const *name, Result const &r) {
        std::printf("%-58s p50 %7.1f  p90 %7.1f  p99 %7.1f  mean %7.1f  | net p50 %6.1f ns\n", name, r.p50, r.p90, r.p99, r.mean, r.p50 - g_emptyP50);
        std::fflush(stdout);
    }

    // before() runs outside the timed region, f() inside it
    template <class Before, class F>
    Result measure(char const *name, Before &&before, F &&f) {
        std::vector<uint64_t> d(g_n);
        for (std::size_t ii = 0; ii < 20000; ++ii) {
            before(ii);
            f(ii);
        }
        for (std::size_t ii = 0; ii < g_n; ++ii) {
            before(ii);
            uint64_t t0 = stampBegin();
            f(ii);
            uint64_t t1 = stampEnd();
            d[ii] = t1 - t0;
        }
        auto r = summarize(d);
        print(name, r);
        return r;
    }
    template <class F>
    Result measure(char const *name, F &&f) {
        return measure(name, [](std::size_t) {}, std::forward<F>(f));
    }

    // ------------------------------------------------------------ graph

    template <class Clock, bool CheckTime>
    using BenchEnv = infra::Environment<
        infra::CheckTimeComponent<CheckTime>, infra::TrivialExitControlComponent, basic::TimeComponentEnhancedWithSpdLogging<Clock, false>>;

    constexpr std::size_t kBatch = 4096;

    // Keeps the moved-in strings alive until the end of the batch so that
    // no free() happens inside the timed region.
    struct Pool {
        std::vector<std::string> slots = std::vector<std::string>(kBatch);
    };

    template <class M>
    class BenchSource final : public M::template AbstractImporter<basic::ByteDataWithTopic> {
    public:
        virtual void start(typename M::EnvironmentType *) override final {}
        inline void push(typename M::EnvironmentType *env, basic::ByteDataWithTopic &&d) {
            this->publish(M::template pureInnerData<basic::ByteDataWithTopic>(env, std::move(d)));
        }
    };

    template <class M>
    class BenchSink final : public M::template AbstractExporter<basic::ByteDataWithTopic> {
    private:
        Pool *pool_;
        std::size_t idx_ = 0;

    public:
        explicit BenchSink(Pool *pool)
            : pool_(pool) {}
        virtual void start(typename M::EnvironmentType *) override final {}
        virtual void handle(typename M::template InnerData<basic::ByteDataWithTopic> &&d) override final {
            pool_->slots[(idx_++) % kBatch] = std::move(d.timedData.value.content);
        }
    };

    enum class SinkKind {
        Custom,
        Simple
    };

    template <class Clock, bool CheckTime, SinkKind Kind, bool ViaStdFunction = false>
    void graphBench(char const *name) {
        using Env = BenchEnv<Clock, CheckTime>;
        using M = infra::RealTimeApp<Env>;
        using R = infra::AppRunner<M>;
        auto *env = new Env();
        auto *r = new R(env);
        auto *pool = new Pool();
        auto *src = new BenchSource<M>();
        auto imp = M::importer(src);
        std::shared_ptr<typename M::template Exporter<basic::ByteDataWithTopic>> exp;
        if constexpr (Kind == SinkKind::Custom) {
            exp = M::exporter(new BenchSink<M>(pool));
        } else {
            exp = M::template simpleExporter<basic::ByteDataWithTopic>(
                [pool, idx = std::size_t(0)](typename M::template InnerData<basic::ByteDataWithTopic> &&d) mutable {
                    pool->slots[(idx++) % kBatch] = std::move(d.timedData.value.content);
                });
        }
        r->connect(r->importItem("src", imp), r->exporterAsSink("sink", exp));
        r->finalize();

        // mimics a transport handing data to its importer through std::function
        std::function<void(basic::ByteDataWithTopic &&)> cb = [src, env](basic::ByteDataWithTopic &&d) {
            src->push(env, std::move(d));
        };
        basic::ByteDataWithTopic next;
        measure(
            name,
            [&next, pool](std::size_t ii) {
                if (ii % kBatch == 0) {
                    for (auto &s : pool->slots) {
                        std::string().swap(s);
                    }
                }
                next = basic::ByteDataWithTopic{"ipc.profile", std::string(256, '\0')};
            },
            [&next, &cb, src, env](std::size_t) {
                if constexpr (ViaStdFunction) {
                    cb(std::move(next));
                } else {
                    src->push(env, std::move(next));
                }
            });
        // the graph (and its clock thread) is leaked on purpose: tearing down
        // a finalized runner is not needed for a measurement
    }

    // ------------------------------------------------------------ pieces

    template <class Env, bool MutexProtected>
    void timeCheckerBench(char const *name) {
        using TC = typename infra::RealTimeAppComponents<Env>::template TimeChecker<MutexProtected, basic::ByteDataWithTopic>;
        Env env;
        TC tc;
        auto d = infra::withtime_utils::pureTimedDataWithEnvironment<basic::ByteDataWithTopic, Env, typename Env::TimePointType>(
            &env, basic::ByteDataWithTopic{"ipc.profile", std::string(256, '\0')});
        bool ok = true;
        measure(name, [&](std::size_t ii) {
            d.timedData.timePoint += std::chrono::nanoseconds(ii & 1);
            ok = tc(d) && ok;
        });
        doNotOptimize(ok);
    }

    struct IVirt {
        virtual ~IVirt() = default;
        virtual void call(std::string &&) = 0;
    };
    struct Virt final : IVirt {
        std::size_t n = 0;
        virtual void call(std::string &&s) override {
            n += s.size();
        }
    };

} // namespace

int main(int argc, char **argv) {
    if (argc > 1) {
        g_n = static_cast<std::size_t>(std::atoll(argv[1]));
    }
    g_ghz = calibrateTscGHz();
    std::printf("TSC %.4f GHz, %zu samples per row (all values ns)\n", g_ghz, g_n);
    {
        g_emptyP50 = 0.0;
        auto r = measure("empty bracket", [](std::size_t) {});
        g_emptyP50 = r.p50;
    }

    std::printf("--- clock reads\n");
    measure("rdtsc", [](std::size_t) {
        auto t = __rdtsc();
        doNotOptimize(t);
    });
    measure("std::chrono::system_clock::now()", [](std::size_t) {
        auto t = std::chrono::system_clock::now();
        doNotOptimize(t);
    });
    measure("clock_gettime(CLOCK_REALTIME_COARSE)", [](std::size_t) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME_COARSE, &ts);
        doNotOptimize(ts);
    });
    {
        basic::real_time_clock::ClockComponent c;
        measure("ClockComponent::resolveTime()", [&c](std::size_t) {
            auto t = c.resolveTime();
            doNotOptimize(t);
        });
    }
#ifdef LOW_LATENCY_BENCH_HAS_TSC_CLOCK
    {
        basic::real_time_clock::TscClockComponent c;
        measure("TscClockComponent::resolveTime()", [&c](std::size_t) {
            auto t = c.resolveTime();
            doNotOptimize(t);
        });
    }
#endif

    std::printf("--- synchronization / indirection / allocation\n");
    {
        std::mutex m;
        measure("std::mutex lock+unlock (uncontended)", [&m](std::size_t) {
            std::lock_guard<std::mutex> _(m);
        });
    }
    {
        std::atomic<bool> b{false};
        measure("atomic<bool> relaxed load (+store once)", [&b](std::size_t) {
            if (!b.load(std::memory_order_relaxed)) {
                b.store(true, std::memory_order_relaxed);
            }
        });
    }
    {
        std::size_t n = 0;
        std::function<void(std::string &&)> f = [&n](std::string &&s) { n += s.size(); };
        std::string s(256, '\0');
        measure("std::function call", [&f, &s](std::size_t) { f(std::move(s)); });
        doNotOptimize(n);
    }
    {
        std::unique_ptr<IVirt> v = std::make_unique<Virt>();
        IVirt *p = v.get();
        doNotOptimize(p);
        std::string s(256, '\0');
        measure("virtual call", [p, &s](std::size_t) { p->call(std::move(s)); });
    }
    {
        std::string src(256, 'x');
        measure("std::string(256B) construct+destroy (malloc+copy+free)", [&src](std::size_t) {
            std::string s(src.data(), src.size());
            doNotOptimize(s);
        });
    }

    using DefaultEnv = BenchEnv<basic::real_time_clock::ClockComponent, false>;
    using DefaultEnvCT = BenchEnv<basic::real_time_clock::ClockComponent, true>;
    std::printf("--- TimeChecker (single input, ByteDataWithTopic)\n");
    timeCheckerBench<DefaultEnv, false>("TimeChecker<false>  CheckTime=false");
    timeCheckerBench<DefaultEnv, true>("TimeChecker<true>   CheckTime=false");
    timeCheckerBench<DefaultEnvCT, true>("TimeChecker<true>   CheckTime=true");

    using basic::real_time_clock::ClockComponent;
    using basic::real_time_clock::SimplifiedClockComponent;
    std::printf("--- graph: importer publish -> exporter (one hop, moves content out)\n");
    graphBench<ClockComponent, false, SinkKind::Simple>("graph Clock      + simpleExporter");
    graphBench<ClockComponent, false, SinkKind::Custom>("graph Clock      + custom exporter");
    graphBench<SimplifiedClockComponent, false, SinkKind::Simple>("graph Simplified + simpleExporter");
    graphBench<SimplifiedClockComponent, false, SinkKind::Custom>("graph Simplified + custom exporter");
    graphBench<ClockComponent, true, SinkKind::Simple>("graph Clock      + simpleExporter  CheckTime=true");
    graphBench<ClockComponent, false, SinkKind::Simple, true>("graph Clock      + simpleExporter  via std::function");
#ifdef LOW_LATENCY_BENCH_HAS_TSC_CLOCK
    using basic::real_time_clock::TscClockComponent;
    graphBench<TscClockComponent, false, SinkKind::Simple>("graph TscClock   + simpleExporter");
    graphBench<TscClockComponent, false, SinkKind::Custom>("graph TscClock   + custom exporter");
    graphBench<TscClockComponent, true, SinkKind::Simple>("graph TscClock   + simpleExporter  CheckTime=true");
#endif
    std::fflush(stdout);
    ::_exit(0);
}
