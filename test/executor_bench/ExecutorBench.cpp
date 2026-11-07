#include <tm_kit/infra/LegacyDedicatedThreadExecutor.hpp>
#include <tm_kit/basic/EnhancedDedicatedThreadedExecutor.hpp>
#include <tm_kit/infra/RealTimeSharedPoolExecutor.hpp>
#include <tm_kit/basic/MoodycamelThreadedExecutor.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>
#include <time.h>
#include <sched.h>

#ifdef EXECUTOR_BENCH_COUNTS
extern "C" void *__real_malloc(std::size_t);
extern "C" void __real_free(void *);
#endif

using namespace dev::cd606::tm;
namespace allocation_count
{
    std::atomic<bool> enabled{false};
    std::atomic<uint64_t> allocations{0}, cppAllocations{0}, cAllocations{0}, bytes{0}, frees{0}, moves{0};
    void *allocate(std::size_t n, std::size_t alignment = 0)
    {
        void *p = nullptr;
        if (alignment) {
            if (posix_memalign(&p, alignment, n ? n : 1) != 0) {
                throw std::bad_alloc();
            }
        } else {
#ifdef EXECUTOR_BENCH_COUNTS
            p = __real_malloc(n ? n : 1);
#else
            p = std::malloc(n ? n : 1);
#endif
            if (!p) {
                throw std::bad_alloc();
            }
        }
        if (enabled.load(std::memory_order_relaxed)) {
            allocations.fetch_add(1, std::memory_order_relaxed);
            cppAllocations.fetch_add(1, std::memory_order_relaxed);
            bytes.fetch_add(n, std::memory_order_relaxed);
        }
        return p;
    }
    void release(void *p)
    {
        if (p && enabled.load(std::memory_order_relaxed)) {
            frees.fetch_add(1, std::memory_order_relaxed);
        }
#ifdef EXECUTOR_BENCH_COUNTS
        __real_free(p);
#else
        std::free(p);
#endif
    }
} // namespace allocation_count
#ifdef EXECUTOR_BENCH_COUNTS
extern "C" void *__wrap_malloc(std::size_t n)
{
    void *p = __real_malloc(n);
    if (p && allocation_count::enabled.load(std::memory_order_relaxed)) {
        allocation_count::allocations.fetch_add(1, std::memory_order_relaxed);
        allocation_count::cAllocations.fetch_add(1, std::memory_order_relaxed);
        allocation_count::bytes.fetch_add(n, std::memory_order_relaxed);
    }
    return p;
}
extern "C" void __wrap_free(void *p)
{
    if (p && allocation_count::enabled.load(std::memory_order_relaxed)) {
        allocation_count::frees.fetch_add(1, std::memory_order_relaxed);
    }
    __real_free(p);
}
void *operator new(std::size_t n)
{
    return allocation_count::allocate(n);
}
void *operator new[](std::size_t n)
{
    return allocation_count::allocate(n);
}
void *operator new(std::size_t n, std::align_val_t a)
{
    return allocation_count::allocate(n, static_cast<std::size_t>(a));
}
void *operator new[](std::size_t n, std::align_val_t a)
{
    return allocation_count::allocate(n, static_cast<std::size_t>(a));
}
void operator delete(void *p) noexcept
{
    allocation_count::release(p);
}
void operator delete[](void *p) noexcept
{
    allocation_count::release(p);
}
void operator delete(void *p, std::size_t) noexcept
{
    allocation_count::release(p);
}
void operator delete[](void *p, std::size_t) noexcept
{
    allocation_count::release(p);
}
void operator delete(void *p, std::align_val_t) noexcept
{
    allocation_count::release(p);
}
void operator delete[](void *p, std::align_val_t) noexcept
{
    allocation_count::release(p);
}
void operator delete(void *p, std::size_t, std::align_val_t) noexcept
{
    allocation_count::release(p);
}
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept
{
    allocation_count::release(p);
}
#endif

namespace
{
    uint64_t ns(clockid_t clock = CLOCK_MONOTONIC)
    {
        timespec t;
        if (clock_gettime(clock, &t)) {
            throw std::runtime_error("clock_gettime failed");
        }
        return uint64_t(t.tv_sec) * 1000000000ULL + t.tv_nsec;
    }
    struct Usage {
        uint64_t cpu;
        rusage ru;
        static Usage read()
        {
            Usage u;
            u.cpu = ns(CLOCK_PROCESS_CPUTIME_ID);
            if (getrusage(RUSAGE_SELF, &u.ru)) {
                throw std::runtime_error("getrusage failed");
            }
            return u;
        }
    };
    std::vector<int> parseCpus(std::string const &text)
    {
        std::vector<int> result;
        cpu_set_t allowed;
        if (sched_getaffinity(0, sizeof(allowed), &allowed)) {
            throw std::runtime_error("sched_getaffinity failed");
        }
        std::size_t offset = 0;
        while (offset < text.size()) {
            auto end = text.find(',', offset);
            auto part = text.substr(offset, end == std::string::npos ? end : end - offset);
            if (part.empty() || part.find_first_not_of("0123456789") != std::string::npos) {
                throw std::runtime_error("invalid CPU list");
            }
            auto cpu = std::stoul(part);
            if (cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &allowed)) {
                throw std::runtime_error("CPU outside allowed affinity");
            }
            result.push_back(cpu);
            if (end == std::string::npos) {
                break;
            }
            offset = end + 1;
            if (offset == text.size()) {
                throw std::runtime_error("invalid CPU list");
            }
        }
        if (result.empty()) {
            throw std::runtime_error("empty CPU list");
        }
        return result;
    }
    void pin(int cpu)
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set)) {
            throw std::runtime_error("sched_setaffinity failed");
        }
    }
    struct Options {
        std::string executor = "dedicated", scenario = "throughput";
        uint64_t messages = 200000, warmup = 2000, sampleEvery = 0, intervalUs = 0;
        unsigned producers = 1, nodes = 1, payload = 0, poolThreads = 2, maxBatch = 64, idleMs = 300;
        bool idleWork = false;
        std::string producerCpuList, consumerCpuList;
        std::vector<int> producerCpus, consumerCpus;
    };
    Options parse(int argc, char **argv)
    {
        Options o;
        for (int i = 1; i < argc; ++i) {
            std::string k = argv[i];
            if (k == "--idle-work") {
                o.idleWork = true;
                continue;
            }
            if (k == "--help") {
                std::puts(
                    "--executor direct|inline|inline-erased|dedicated|pool|moody|legacy|enhanced --scenario throughput|latency|idle\n"
                    "--messages N (per producer) --warmup N --producers N --nodes N --payload 0|256\n"
                    "--sample-every N (0 disables stamps) --interval-us N --pool-threads N --max-batch N\n"
                    "--idle-ms N --idle-work --producer-cpus 20,21 --consumer-cpus 24,25; one case per process");
                std::exit(0);
            }
            if (++i >= argc) {
                throw std::runtime_error("missing value: " + k);
            }
            std::string v = argv[i];
            if (k == "--producer-cpus") {
                o.producerCpuList = v;
                o.producerCpus = parseCpus(v);
                continue;
            }
            if (k == "--consumer-cpus") {
                o.consumerCpuList = v;
                o.consumerCpus = parseCpus(v);
                continue;
            }
            if (k == "--executor") {
                o.executor = v;
                continue;
            }
            if (k == "--scenario") {
                o.scenario = v;
                continue;
            }
            if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos) {
                throw std::runtime_error("invalid number: " + v);
            }
            auto n = std::stoull(v);
            if (k == "--messages") {
                o.messages = n;
            } else if (k == "--warmup") {
                o.warmup = n;
            } else if (k == "--sample-every") {
                o.sampleEvery = n;
            } else if (k == "--interval-us") {
                o.intervalUs = n;
            } else {
                if (n > std::numeric_limits<unsigned>::max()) {
                    throw std::runtime_error("option too large");
                }
                if (k == "--producers") {
                    o.producers = n;
                } else if (k == "--nodes") {
                    o.nodes = n;
                } else if (k == "--payload") {
                    o.payload = n;
                } else if (k == "--pool-threads") {
                    o.poolThreads = n;
                } else if (k == "--max-batch") {
                    o.maxBatch = n;
                } else if (k == "--idle-ms") {
                    o.idleMs = n;
                } else {
                    throw std::runtime_error("unknown option: " + k);
                }
            }
        }
        if (!o.messages || o.messages > 100000000 || !o.producers || o.producers > 64 || !o.nodes || o.nodes > 4096 || o.nodes > o.messages || !o.warmup || !o.poolThreads || !o.maxBatch ||
            !o.idleMs || o.intervalUs > 1000000 || (o.payload != 0 && o.payload != 256)) {
            throw std::runtime_error("options out of range");
        }
        if (o.scenario != "throughput" && o.scenario != "latency" && o.scenario != "idle") {
            throw std::runtime_error("unknown scenario");
        }
        if (o.scenario == "latency") {
            o.sampleEvery = 1;
        }
        o.warmup = std::max<uint64_t>(o.warmup, o.nodes);
        return o;
    }
    // Move-only, non-default-constructible, inline payload: its own storage
    // does not allocate. The counts binary measures transfers separately.
    template <unsigned Payload>
    struct Item {
        unsigned producer;
        uint64_t seq, sent;
        bool warm;
        std::array<unsigned char, Payload> data;
        Item(unsigned p, uint64_t s, uint64_t t, bool w)
            : producer(p)
            , seq(s)
            , sent(t)
            , warm(w)
        {
            data.fill(0x5a);
        }
        Item(Item const &) = delete;
        Item &operator=(Item const &) = delete;
        Item(Item &&x) noexcept
            : producer(x.producer)
            , seq(x.seq)
            , sent(x.sent)
            , warm(x.warm)
            , data(std::move(x.data))
        {
#ifdef EXECUTOR_BENCH_COUNTS
            if (allocation_count::enabled.load(std::memory_order_relaxed)) {
                allocation_count::moves.fetch_add(1, std::memory_order_relaxed);
            }
#endif
        }
        Item &operator=(Item &&x) noexcept
        {
            producer = x.producer;
            seq = x.seq;
            sent = x.sent;
            warm = x.warm;
            data = std::move(x.data);
#ifdef EXECUTOR_BENCH_COUNTS
            if (allocation_count::enabled.load(std::memory_order_relaxed)) {
                allocation_count::moves.fetch_add(1, std::memory_order_relaxed);
            }
#endif
            return *this;
        }
    };
    struct alignas(64) Slot {
        uint64_t count = 0, expected = 0, target = 0, errors = 0, checksum = 0;
        std::atomic<uint64_t> ack{0};
        std::vector<uint64_t> samples;
    };
    struct Context {
        Options opt;
        bool latency;
        std::atomic<uint64_t> warmLeft{0}, remaining{0}, started{0};
        std::mutex mutex;
        std::condition_variable cond;
        uint64_t end = 0;
        explicit Context(Options const &o)
            : opt(o)
            , latency(o.scenario == "latency")
        {
        }
        template <class F>
        void wait(F pred)
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!cond.wait_for(lock, std::chrono::seconds(60), pred)) {
                std::fputs("benchmark timeout\n", stderr);
                std::_Exit(2);
            }
        }
        void notify()
        {
            std::lock_guard<std::mutex> lock(mutex);
            cond.notify_all();
        }
        void finishSlot()
        {
            if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                end = ns();
                // Mutex publishes endpoint fields to the coordinator.
                std::lock_guard<std::mutex> lock(mutex);
                finished = true;
                cond.notify_all();
            }
        }
        bool finished = false; // mutex protected
    };
    template <unsigned Payload>
    struct Host {
        Context *ctx;
        unsigned index;
        std::unique_ptr<Slot[]> slots;
        std::atomic<uint64_t> idle{0};
        Host(Context *c, unsigned i)
            : ctx(c)
            , index(i)
            , slots(new Slot[c->opt.producers])
        {
            for (unsigned p = 0; p < c->opt.producers; ++p) {
                slots[p].expected = i;
                slots[p].target = (c->opt.messages - 1 - i) / c->opt.nodes + 1;
                if (c->opt.sampleEvery) {
                    slots[p].samples.reserve((c->opt.messages - 1 - i) / c->opt.nodes + 1);
                }
            }
        }
        void executorWaitForStart()
        {
            if (ctx->opt.executor != "pool" && !ctx->opt.consumerCpus.empty()) {
                pin(ctx->opt.consumerCpus[index % ctx->opt.consumerCpus.size()]);
            }
            ctx->started.fetch_add(1, std::memory_order_release);
            ctx->notify();
        }
        void executorHandle(Item<Payload> &&x)
        {
            // Prevent elimination of payload storage/copies without doing
            // a business checksum over the whole payload in the callback.
            asm volatile("" : : "g"(&x) : "memory");
            if (x.warm) {
                if (ctx->warmLeft.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    ctx->notify();
                }
                return;
            }
            auto &s = slots[x.producer];
            if (x.sent) {
                s.samples.push_back(ns() - x.sent);
            }
            s.errors += x.seq != s.expected;
            s.expected += ctx->opt.nodes;
            s.checksum += x.seq;
            ++s.count;
            if (ctx->latency) {
                s.ack.store(x.seq + 1, std::memory_order_release);
            }
            if (s.count == s.target) {
                ctx->finishSlot();
            }
        }
        bool executorHasIdleWork()
        {
            return ctx->opt.idleWork;
        }
        void executorIdleWork()
        {
            idle.fetch_add(1, std::memory_order_relaxed);
        }
        void executorBeforeBatch(Item<Payload> const &, std::size_t)
        {
        }
    };
    // An optional statically typed submission hook is usable by the built-in
    // Inline executor; other/custom executors retain the original contract.
    template <class E, class I, class H>
    auto submit(E &e, I &&x, H *h, int) -> decltype(e.pushWithHost(std::move(x), h))
    {
        return e.pushWithHost(std::move(x), h);
    }
    template <class E, class I, class H>
    bool submit(E &e, I &&x, H *, long)
    {
        return e.push(std::move(x));
    }

    template <template <class> class Executor, unsigned Payload, bool Direct = false, bool Erased = false>
    int run(Options const &o)
    {
        using E = Executor<Item<Payload>>;
        auto *ctx = new Context(o); // process-isolated case; detached hosts stay alive to exit
        std::vector<E *> execs;
        std::vector<Host<Payload> *> hosts;
        for (unsigned i = 0; i < o.nodes; ++i) {
            hosts.push_back(new Host<Payload>(ctx, i));
            execs.push_back(new E(infra::ThreadedExecOptions{}));
            execs.back()->start(hosts.back());
        }
        if constexpr (E::kOwnContext) {
            ctx->wait([&] { return ctx->started.load(std::memory_order_acquire) == o.nodes; });
        }
        auto send = [&](unsigned p, uint64_t seq, bool warm) {
            auto i = o.nodes == 1 ? 0 : seq % o.nodes;
            Item<Payload> x(p, seq, 0, warm);
            if (!warm && o.sampleEvery && seq % o.sampleEvery == 0) {
                x.sent = ns();
            }
            if constexpr (Direct) {
                hosts[i]->executorHandle(std::move(x));
                return true;
            } else if constexpr (Erased) {
                return execs[i]->push(std::move(x));
            } else {
                return submit(*execs[i], std::move(x), hosts[i], 0);
            }
        };
        Usage beginUsage{}, endUsage{};
        uint64_t begin = 0, end = 0, producerCpu = 0;
        if (o.scenario == "idle") {
            // Let initial registration/idle capability discovery settle.
            for (auto *e : execs) {
                e->wakeIdle();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            for (auto *h : hosts) {
                h->idle.store(0);
            }
            beginUsage = Usage::read();
            begin = ns();
            allocation_count::enabled.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(o.idleMs));
            end = ns();
            endUsage = Usage::read();
            allocation_count::enabled.store(false);
        } else {
            ctx->warmLeft = o.warmup * o.producers;
            ctx->remaining = uint64_t(o.nodes) * o.producers;
            std::atomic<unsigned> ready{0};
            std::atomic<bool> go{false};
            std::vector<uint64_t> cpu(o.producers, 0);
            std::vector<std::thread> producers;
            for (unsigned p = 0; p < o.producers; ++p) {
                producers.emplace_back([&, p] {
                    if (!o.producerCpus.empty()) {
                        pin(o.producerCpus[p % o.producerCpus.size()]);
                    }
                    for (uint64_t i = 0; i < o.warmup; ++i) {
                        if (!send(p, i, true)) {
                            std::abort();
                        }
                    }
                    ready.fetch_add(1, std::memory_order_release);
                    while (!go.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    // Cache scenario decisions outside the measured loop;
                    // string comparisons are not Executor overhead.
                    bool const closedLoop = ctx->latency;
                    auto const intervalUs = o.intervalUs;
                    auto const messages = o.messages;
                    auto startCpu = ns(CLOCK_THREAD_CPUTIME_ID);
                    auto deadline = ns() + 60000000000ULL;
                    for (uint64_t i = 0; i < messages; ++i) {
                        if (!send(p, i, false)) {
                            std::abort();
                        }
                        if (closedLoop) {
                            auto &ack = hosts[i % o.nodes]->slots[p].ack;
                            while (ack.load(std::memory_order_acquire) < i + 1) {
                                if (ns() > deadline) {
                                    std::abort();
                                }
                                std::this_thread::yield();
                            }
                        }
                        if (intervalUs) {
                            std::this_thread::sleep_for(std::chrono::microseconds(intervalUs));
                        }
                    }
                    cpu[p] = ns(CLOCK_THREAD_CPUTIME_ID) - startCpu;
                });
            }
            while (ready.load(std::memory_order_acquire) != o.producers) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            ctx->wait([&] { return ctx->warmLeft.load(std::memory_order_acquire) == 0; });
            // Warm-up completion is callback completion; allow its queue
            // bookkeeping to finish before enabling allocation counters.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            beginUsage = Usage::read();
            begin = ns();
            allocation_count::enabled.store(true);
            go.store(true, std::memory_order_release);
            ctx->wait([&] { return ctx->finished; });
            allocation_count::enabled.store(false);
            for (auto &t : producers) {
                t.join();
            }
            end = ctx->end;
            endUsage = Usage::read();
            for (auto c : cpu) {
                producerCpu += c;
            }
        }
        uint64_t count = 0, errors = 0, checksum = 0, idle = 0;
        std::vector<uint64_t> samples;
        for (auto *h : hosts) {
            idle += h->idle.load();
            if (o.scenario != "idle") {
                for (unsigned p = 0; p < o.producers; ++p) {
                    auto &s = h->slots[p];
                    count += s.count;
                    errors += s.errors;
                    checksum += s.checksum;
                    samples.insert(samples.end(), s.samples.begin(), s.samples.end());
                }
            }
        }
        auto expected = o.scenario == "idle" ? 0 : o.messages * o.producers;
        bool ok = count == expected && errors == 0 && (o.scenario == "idle" || checksum == o.producers * (o.messages * (o.messages - 1) / 2));
        std::sort(samples.begin(), samples.end());
        auto pct = [&](double q) -> uint64_t { return samples.empty() ? 0 : samples[static_cast<std::size_t>(q * (samples.size() - 1))]; };
        auto elapsed = end - begin;
        auto processCpu = endUsage.cpu - beginUsage.cpu;
        // For Inline, callbacks are included in producer CPU; for async
        // executors the remainder also includes coordinator/background CPU.
        auto backgroundCpu = processCpu > producerCpu ? processCpu - producerCpu : 0;
#ifdef EXECUTOR_BENCH_COUNTS
        constexpr bool instrumented = true;
#else
        constexpr bool instrumented = false;
#endif
        std::printf(
            "{\"executor\":\"%s\",\"scenario\":\"%s\",\"instrumented\":%s,\"ok\":%s,"
            "\"producer_cpus\":\"%s\",\"consumer_cpus\":\"%s\",\"producers\":%u,\"nodes\":%u,\"item_bytes\":%zu,\"payload\":%u,\"sample_every\":%llu,\"interval_us\":%llu,"
            "\"messages_per_producer\":%llu,\"warmup\":%llu,\"pool_threads\":%u,\"max_batch\":%u,\"idle_work\":%s,\"items\":%llu,\"wall_ns\":%llu,"
            "\"process_cpu_ns\":%llu,\"producer_cpu_ns\":%llu,\"background_cpu_ns\":%llu,"
            "\"voluntary_cs\":%ld,\"involuntary_cs\":%ld,\"samples\":%zu,\"p50_ns\":%llu,\"p99_ns\":%llu,\"p999_ns\":%llu,"
            "\"cpp_allocations\":%llu,\"c_allocations\":%llu,\"allocations\":%llu,\"allocated_bytes\":%llu,\"frees\":%llu,\"moves\":%llu,\"idle_calls\":%llu}\n",
            o.executor.c_str(),
            o.scenario.c_str(),
            instrumented ? "true" : "false",
            ok ? "true" : "false",
            o.producerCpuList.c_str(),
            o.consumerCpuList.c_str(),
            o.producers,
            o.nodes,
            sizeof(Item<Payload>),
            Payload,
            (unsigned long long)o.sampleEvery,
            (unsigned long long)o.intervalUs,
            (unsigned long long)o.messages,
            (unsigned long long)o.warmup,
            o.poolThreads,
            o.maxBatch,
            o.idleWork ? "true" : "false",
            (unsigned long long)count,
            (unsigned long long)elapsed,
            (unsigned long long)processCpu,
            (unsigned long long)producerCpu,
            (unsigned long long)backgroundCpu,
            endUsage.ru.ru_nvcsw - beginUsage.ru.ru_nvcsw,
            endUsage.ru.ru_nivcsw - beginUsage.ru.ru_nivcsw,
            samples.size(),
            (unsigned long long)pct(.5),
            (unsigned long long)pct(.99),
            (unsigned long long)pct(.999),
            (unsigned long long)allocation_count::cppAllocations.load(),
            (unsigned long long)allocation_count::cAllocations.load(),
            (unsigned long long)allocation_count::allocations.load(),
            (unsigned long long)allocation_count::bytes.load(),
            (unsigned long long)allocation_count::frees.load(),
            (unsigned long long)allocation_count::moves.load(),
            (unsigned long long)idle);
        for (auto *e : execs) {
            e->stop();
        }
        std::fflush(stdout);
        // No destructor may race a detached callback or callback epilogue.
        std::_Exit(ok ? 0 : 1);
    }
    template <template <class> class E, bool Direct = false, bool Erased = false>
    int dispatch(Options const &o)
    {
        return o.payload ? run<E, 256, Direct, Erased>(o) : run<E, 0, Direct, Erased>(o);
    }
} // namespace
int main(int argc, char **argv)
{
    try {
        auto o = parse(argc, argv);
#if !defined(EXECUTOR_BENCH_ENGINE) || EXECUTOR_BENCH_ENGINE == 5
        infra::SharedThreadPoolConfig cfg;
        cfg.cpus = o.consumerCpus;
        cfg.threadCount = o.poolThreads;
        cfg.maxBatch = o.maxBatch;
        infra::SharedThreadPool::configure(cfg);
#endif
#if !defined(EXECUTOR_BENCH_ENGINE) || EXECUTOR_BENCH_ENGINE == 1
        if (o.executor == "direct") {
            return dispatch<infra::InlineExecutor, true>(o);
        }
#endif
#if !defined(EXECUTOR_BENCH_ENGINE) || EXECUTOR_BENCH_ENGINE == 2
        if (o.executor == "inline") {
            return dispatch<infra::InlineExecutor>(o);
        }
#endif
#if !defined(EXECUTOR_BENCH_ENGINE) || EXECUTOR_BENCH_ENGINE == 3
        if (o.executor == "inline-erased") {
            return dispatch<infra::InlineExecutor, false, true>(o);
        }
#endif
#if !defined(EXECUTOR_BENCH_ENGINE) || EXECUTOR_BENCH_ENGINE == 4
        if (o.executor == "dedicated") {
            return dispatch<infra::DedicatedThreadExecutor>(o);
        }
#endif
#if !defined(EXECUTOR_BENCH_ENGINE) || EXECUTOR_BENCH_ENGINE == 5
        if (o.executor == "pool") {
            return dispatch<infra::RealTimeSharedPoolExecutor>(o);
        }
#endif
#if defined(TM_KIT_BASIC_HAS_MOODYCAMEL_THREADED_EXECUTOR) && (!defined(EXECUTOR_BENCH_ENGINE) || EXECUTOR_BENCH_ENGINE == 6)
        if (o.executor == "moody") {
            return dispatch<basic::MoodycamelThreadedExecutor>(o);
        }
#endif
#if !defined(EXECUTOR_BENCH_ENGINE) || EXECUTOR_BENCH_ENGINE == 7
        if (o.executor == "legacy") {
            return dispatch<infra::LegacyDedicatedThreadExecutor>(o);
        }
#endif
#if !defined(EXECUTOR_BENCH_ENGINE) || EXECUTOR_BENCH_ENGINE == 8
        if (o.executor == "enhanced") {
            return dispatch<basic::EnhancedDedicatedThreadedExecutor>(o);
        }
#endif
        throw std::runtime_error("unknown/unavailable executor");
    } catch (std::exception const &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }
}
