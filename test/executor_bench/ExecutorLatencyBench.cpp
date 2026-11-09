#include <tm_kit/infra/RealTimeThreadedExecutor.hpp>
#include <tm_kit/infra/RealTimeThreadedExecutor.hpp>
#include <tm_kit/basic/EnhancedDedicatedThreadedExecutor.hpp>
#include <tm_kit/basic/RealTimeSharedPoolExecutor.hpp>
#include <tm_kit/basic/MoodycamelThreadedExecutor.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <sstream>
#include <thread>
#include <vector>
#include <sched.h>
#include <time.h>

using namespace dev::cd606::tm;
namespace
{
    uint64_t now(clockid_t clock = CLOCK_MONOTONIC)
    {
        timespec t;
        clock_gettime(clock, &t);
        return uint64_t(t.tv_sec) * 1000000000ULL + t.tv_nsec;
    }
    void relax()
    {
#if defined(__i386__) || defined(__x86_64__)
        __asm__ __volatile__("pause");
#elif defined(__aarch64__)
        __asm__ __volatile__("yield");
#else
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    }
    void checkCpu(int cpu)
    {
        cpu_set_t allowed;
        if (sched_getaffinity(0, sizeof(allowed), &allowed) || cpu < 0 || cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &allowed)) {
            throw std::runtime_error("CPU is outside allowed affinity");
        }
    }
    void pin(int cpu)
    {
        checkCpu(cpu);
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set)) {
            throw std::runtime_error("pin failed");
        }
    }
    struct Options {
        std::string engine = "dedicated", mode = "closed";
        unsigned hops = 1, messages = 20000, warmup = 2000, spinUs = 0, intervalUs = 0, poolThreads = 2;
        int producerCpu = 20, consumerCpu = 24;
        std::string consumerCpuList;
        std::vector<int> consumerCpus;
        int consumer(unsigned index) const
        {
            return consumerCpus.empty() ? consumerCpu + index % 4 : consumerCpus[index % consumerCpus.size()];
        }
    };
    Options parse(int argc, char **argv)
    {
        Options o;
        for (int i = 1; i < argc; ++i) {
            std::string k = argv[i];
            if (++i == argc) {
                throw std::runtime_error("missing argument");
            }
            std::string v = argv[i];
            if (k == "--executor") {
                o.engine = v;
                continue;
            }
            if (k == "--mode") {
                o.mode = v;
                continue;
            }
            if (k == "--consumer-cpus") {
                if (v.empty() || v.back() == ',') {
                    throw std::runtime_error("invalid CPU list");
                }
                o.consumerCpuList = v;
                std::istringstream input(v);
                std::string cpu;
                while (std::getline(input, cpu, ',')) {
                    if (cpu.empty() || cpu.find_first_not_of("0123456789") != std::string::npos || std::stoull(cpu) >= CPU_SETSIZE) {
                        throw std::runtime_error("invalid CPU list");
                    }
                    o.consumerCpus.push_back(std::stoi(cpu));
                }
                continue;
            }
            if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos) {
                throw std::runtime_error("invalid number");
            }
            auto n = std::stoull(v);
            if (n > 10000000) {
                throw std::runtime_error("number too large");
            }
            if (k == "--hops") {
                o.hops = n;
            } else if (k == "--messages") {
                o.messages = n;
            } else if (k == "--warmup") {
                o.warmup = n;
            } else if (k == "--spin-us") {
                o.spinUs = n;
            } else if (k == "--interval-us") {
                o.intervalUs = n;
            } else if (k == "--pool-threads") {
                o.poolThreads = n;
            } else if (k == "--producer-cpu") {
                o.producerCpu = n;
            } else if (k == "--consumer-cpu") {
                o.consumerCpu = n;
            } else {
                throw std::runtime_error("unknown option");
            }
        }
        if (!o.hops || o.hops > 16 || !o.messages || !o.warmup || o.spinUs > 10000 || o.intervalUs > 1000000 || !o.poolThreads || o.poolThreads > 16 || (o.mode != "closed" && o.mode != "periodic")) {
            throw std::runtime_error("options out of range");
        }
#ifdef EXECUTOR_LATENCY_BASELINE
        if (o.spinUs) {
            throw std::runtime_error("baseline does not support spin-us");
        }
#endif
        return o;
    }
    struct Item {
        uint64_t seq, sent, due;
    };
    template <template <class> class Exec>
    struct Pipeline {
        struct alignas(64) Host {
            Pipeline *p;
            unsigned index;
            uint64_t expected = 0, errors = 0;
            void executorWaitForStart()
            {
                if (p->opt.engine != "pool") {
                    pin(p->opt.consumer(index));
                }
                p->started.fetch_add(1, std::memory_order_release);
            }
            void executorHandle(Item &&x)
            {
                errors += x.seq != expected++;
                if (index + 1 < p->opt.hops) {
                    p->submit(index + 1, std::move(x));
                } else {
                    if (x.sent) {
                        auto end = now();
                        p->samples[x.seq - p->opt.warmup] = end - x.sent;
                        p->arrival[x.seq - p->opt.warmup] = end - x.due;
                    }
                    p->ack.store(x.seq + 1, std::memory_order_release);
                }
            }
            bool executorHasIdleWork()
            {
                return false;
            }
            void executorIdleWork()
            {
            }
            void executorBeforeBatch(Item const &, std::size_t)
            {
            }
        };
        using E = Exec<Item>;
        Options opt;
        std::vector<std::unique_ptr<Host>> hosts;
        std::vector<std::unique_ptr<E>> execs;
        std::vector<uint64_t> samples, arrival, late;
        std::atomic<uint64_t> ack{0}, started{0};
        explicit Pipeline(Options const &o)
            : opt(o)
            , samples(o.messages)
            , arrival(o.messages)
            , late(o.messages)
        {
            infra::ThreadedExecOptions opts;
#ifndef EXECUTOR_LATENCY_BASELINE
            opts.spinWait = std::chrono::microseconds(o.spinUs);
#endif
            for (unsigned i = 0; i < o.hops; ++i) {
                hosts.emplace_back(new Host{this, i});
                execs.emplace_back(new E(opts));
            }
            for (unsigned i = 0; i < o.hops; ++i) {
                execs[i]->start(hosts[i].get());
            }
        }
        void submit(unsigned i, Item &&x)
        {
            if (!infra::realtime_threaded_executor_detail::submit(*execs[i], std::move(x), hosts[i].get(), 0)) {
                std::abort();
            }
        }
        void wait(uint64_t seq)
        {
            auto deadline = now() + 60000000000ULL;
            unsigned n = 0;
            while (ack.load(std::memory_order_acquire) < seq) {
                relax();
                if ((++n & 4095) == 0 && now() > deadline) {
                    throw std::runtime_error("pipeline timeout");
                }
            }
        }
    };
    template <template <class> class E>
    int run(Options const &o)
    {
        // The detached executor hosts intentionally live until this isolated process exits.
        auto *p = new Pipeline<E>(o);
        if constexpr (E<Item>::kOwnContext) {
            auto deadline = now() + 60000000000ULL;
            while (p->started.load(std::memory_order_acquire) != o.hops) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                if (now() > deadline) {
                    throw std::runtime_error("start timeout");
                }
            }
        }
        pin(o.producerCpu);
        for (uint64_t i = 0; i < o.warmup; ++i) {
            p->submit(0, Item{i, 0, 0});
            p->wait(i + 1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto begin = now(), cpu = now(CLOCK_PROCESS_CPUTIME_ID), producer = now(CLOCK_THREAD_CPUTIME_ID);
        uint64_t due = begin;
        for (uint64_t i = 0; i < o.messages; ++i) {
            if (o.intervalUs) {
                if (o.mode == "closed") {
                    std::this_thread::sleep_for(std::chrono::microseconds(o.intervalUs));
                    due = now();
                } else {
                    due = begin + i * uint64_t(o.intervalUs) * 1000;
                    // Absolute pacing retains scheduled-arrival delay when the producer is late.
                    timespec ts{time_t(due / 1000000000), long(due % 1000000000)};
                    int rc;
                    do {
                        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
                    } while (rc == EINTR);
                    if (rc) {
                        throw std::runtime_error("clock_nanosleep failed");
                    }
                }
            }
            auto sent = now();
            if (!o.intervalUs || o.mode == "closed") {
                due = sent;
            }
            p->late[i] = sent - due;
            p->submit(0, Item{o.warmup + i, sent, due});
            if (o.mode == "closed") {
                p->wait(o.warmup + i + 1);
            }
        }
        p->wait(uint64_t(o.warmup) + o.messages);
        auto wall = now() - begin, producerCpu = now(CLOCK_THREAD_CPUTIME_ID) - producer, processCpu = now(CLOCK_PROCESS_CPUTIME_ID) - cpu;
        uint64_t errors = 0;
        for (auto &h : p->hosts) {
            errors += h->errors + (h->expected != uint64_t(o.warmup) + o.messages);
        }
        for (auto &e : p->execs) {
            e->stop();
        }
        for (auto *v : {&p->samples, &p->arrival, &p->late}) {
            std::sort(v->begin(), v->end());
        }
        auto percentile = [](std::vector<uint64_t> const &v, double q) { return v[std::size_t((v.size() - 1) * q)]; };
        std::printf(
            "{\"executor\":\"%s\",\"mode\":\"%s\",\"hops\":%u,\"messages\":%u,\"warmup\":%u,\"spin_us\":%u,\"interval_us\":%u,\"pool_threads\":%u,\"producer_cpu\":%d,\"consumer_cpu\":%d,\"consumer_"
            "cpus\":\"%s\","
            "\"wall_ns\":%llu,\"process_"
            "cpu_ns\":%llu,\"producer_cpu_ns\":%llu,\"p50_ns\":%llu,\"p99_ns\":%llu,\"p999_ns\":%llu,\"arrival_p99_ns\":%llu,\"late_p99_ns\":%llu,\"errors\":%llu,\"ok\":%s}\n",
            o.engine.c_str(),
            o.mode.c_str(),
            o.hops,
            o.messages,
            o.warmup,
            o.spinUs,
            o.intervalUs,
            o.poolThreads,
            o.producerCpu,
            o.consumerCpu,
            o.consumerCpuList.c_str(),
            (unsigned long long)wall,
            (unsigned long long)processCpu,
            (unsigned long long)producerCpu,
            (unsigned long long)percentile(p->samples, .5),
            (unsigned long long)percentile(p->samples, .99),
            (unsigned long long)percentile(p->samples, .999),
            (unsigned long long)percentile(p->arrival, .99),
            (unsigned long long)percentile(p->late, .99),
            (unsigned long long)errors,
            errors ? "false" : "true");
        return errors ? 1 : 0;
    }
} // namespace
int main(int argc, char **argv)
{
    try {
        auto o = parse(argc, argv);
        checkCpu(o.producerCpu);
        if (o.engine != "inline") {
            for (unsigned i = 0; i < std::max(o.hops, o.poolThreads); ++i) {
                checkCpu(o.consumer(i));
            }
        }
        if (o.engine == "pool") {
            basic::SharedThreadPoolConfig cfg;
            cfg.threadCount = o.poolThreads;
            for (unsigned i = 0; i < o.poolThreads; ++i) {
                cfg.cpus.push_back(o.consumer(i));
            }
#ifndef EXECUTOR_LATENCY_BASELINE
            cfg.spinWait = std::chrono::microseconds(o.spinUs);
#endif
            basic::SharedThreadPool::configure(cfg);
            return run<basic::RealTimeSharedPoolExecutor>(o);
        }
        if (o.engine == "inline") {
            return run<infra::InlineExecutor>(o);
        }
        if (o.engine == "enhanced") {
            return run<basic::EnhancedDedicatedThreadedExecutor>(o);
        }
        if (o.engine == "dedicated") {
            return run<infra::DedicatedThreadExecutor>(o);
        }
        if (o.engine == "moody") {
            return run<basic::MoodycamelThreadedExecutor>(o);
        }
        throw std::runtime_error("unknown executor");
    } catch (std::exception const &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }
}
