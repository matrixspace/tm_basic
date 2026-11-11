#include <tm_kit/basic/HwlocThreadCpuAllocator.hpp>
#include <tm_kit/basic/MoodycamelThreadedExecutor.hpp>
#include <tm_kit/basic/RealTimeSharedPoolExecutor.hpp>
#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

using namespace dev::cd606::tm;

// HwlocThreadCpuAllocator and its integration with RealTimeApp::configureThreadedNode.
//  1. every acquired CPU is a PU the process may use; no two acquisitions share
//     a physical core (SMT siblings never handed to two busy threads)
//  2. the first core of every NUMA node is avoided by default
//  3. excludeCpus removes the whole physical core, siblings included
//  4. explicit NUMA requests are honoured, unknown nodes throw
//  5. exhaustion throws CpuAutoBindError instead of returning a bad CPU
//  6. release() makes a core reusable; configure() after first use throws
//  7. concurrent acquire/release never hands out the same core twice
//  8. graph nodes: Dedicated and Moodycamel nodes get exactly one CPU;
//     CpuAutoBindOnNuma(n) stays on node n; a SharedPool node rejects
//     CpuAutoBind with an error
// Run under stderr for diagnostics: the failure messages list the topology.

#ifdef TM_KIT_BASIC_HAS_HWLOC_CPU_ALLOCATOR

namespace
{
    int failures = 0;
    void check(bool ok, std::string const &what)
    {
        std::cout << (ok ? "ok:   " : "FAIL: ") << what << '\n';
        failures += !ok;
    }

    // thread_siblings_list for one CPU, e.g. "0,28" -> {0,28}.
    std::vector<int> siblingsOf(int cpu)
    {
        std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list");
        std::vector<int> r;
        std::string item;
        while (std::getline(in, item, ',')) {
            auto dash = item.find('-');
            if (dash == std::string::npos) {
                r.push_back(std::stoi(item));
            } else {
                int lo = std::stoi(item.substr(0, dash));
                int hi = std::stoi(item.substr(dash + 1));
                for (int c = lo; c <= hi; ++c) {
                    r.push_back(c);
                }
            }
        }
        return r;
    }
    std::set<int> coreSiblings(int cpu)
    {
        auto s = siblingsOf(cpu);
        return {s.begin(), s.end()};
    }

    using Alloc = basic::HwlocThreadCpuAllocator;
    using Cfg = basic::HwlocCpuAllocatorConfig;

    template <class F>
    bool throws(F f, std::string *message = nullptr)
    {
        try {
            f();
        } catch (std::exception const &e) {
            if (message) {
                *message = e.what();
            }
            return true;
        }
        return false;
    }

    void acquisitionTest()
    {
        Alloc alloc{Cfg{}};
        auto usable = alloc.usableCores();
        check(!usable.empty(), "topology exposes usable cores");
        auto allowed = alloc.allowedList();
        std::vector<int> got;
        for (std::size_t i = 0; i < usable.size(); ++i) {
            got.push_back(alloc.acquire(infra::CpuAutoBindRequest{}, "t" + std::to_string(i)));
        }
        std::set<std::set<int>> usedCores;
        bool distinct = true, insideMask = true;
        for (int c : got) {
            distinct = distinct && usedCores.insert(coreSiblings(c)).second;
            insideMask = insideMask && alloc.allowedList() == allowed && coreSiblings(c).count(c) == 1;
        }
        check(distinct, "every acquisition takes a different physical core");
        check(insideMask, "every acquired CPU is a PU of the process mask");
        check(got.size() == usable.size(), "one CPU per usable core (" + std::to_string(got.size()) + ")");
        std::string msg;
        check(throws([&] { alloc.acquire(infra::CpuAutoBindRequest{}, "overflow"); }, &msg) && msg.find("no free physical core") != std::string::npos,
            "exhaustion throws CpuAutoBindError with a diagnostic");
        std::cout << "      exhaustion message: " << msg << '\n';
        alloc.release({got.back()});
        int again = -1;
        check(!throws([&] { again = alloc.acquire(infra::CpuAutoBindRequest{}, "reuse"); }) && again == got.back(),
            "released core is handed out again");
    }

    void firstCoreTest()
    {
        // Every NUMA node's lowest PU must be absent: its core carries interrupts.
        std::ifstream numa("/sys/devices/system/node/node0/cpulist");
        if (!numa) {
            std::cout << "skip: no NUMA sysfs\n";
            return;
        }
        Alloc alloc{Cfg{}};
        auto usable = alloc.usableCores();
        std::set<std::set<int>> cores;
        for (auto const &c : usable) {
            cores.insert({c.begin(), c.end()});
        }
        for (int node = 0; node < 4; ++node) {
            std::ifstream in("/sys/devices/system/node/node" + std::to_string(node) + "/cpulist");
            if (!in) {
                continue;
            }
            std::string first;
            std::getline(in, first, ',');
            auto dash = first.find('-');
            int lowest = std::stoi(first.substr(0, dash));
            check(!cores.count(coreSiblings(lowest)), "NUMA node " + std::to_string(node) + " first core (PU " + std::to_string(lowest) + ") avoided");
        }
    }

    void excludeTest()
    {
        Alloc probe{Cfg{}};
        auto usable = probe.usableCores();
        if (usable.size() < 3) {
            std::cout << "skip: not enough cores\n";
            return;
        }
        int excludedCpu = usable[1].front();
        auto excludedCore = std::set<int>(usable[1].begin(), usable[1].end());
        Cfg cfg;
        cfg.excludeCpus = {excludedCpu};
        Alloc alloc{cfg};
        auto now = alloc.usableCores();
        check(now.size() == usable.size() - 1, "excludeCpus removes exactly one core");
        bool any = false;
        for (auto const &c : now) {
            any = any || (std::set<int>(c.begin(), c.end()) == excludedCore);
        }
        check(!any, "the sibling PUs of the excluded CPU are avoided too");
        std::string msg;
        Cfg bad;
        bad.excludeCpus = {-1};
        check(throws([&] { Alloc a{bad}; a.allowedList(); }, &msg), "invalid excluded CPU throws");
    }

    void numaTest()
    {
        std::ifstream node1("/sys/devices/system/node/node1/cpulist");
        if (!node1) {
            std::cout << "skip: single NUMA node\n";
            return;
        }
        Alloc alloc{Cfg{}};
        int cpu = alloc.acquire(infra::CpuAutoBindRequest{1}, "numa1");
        std::ifstream in("/sys/devices/system/node/node1/cpulist");
        std::string list;
        std::getline(in, list);
        // A CPU is on node 1 iff its PU number appears in node1/cpulist.
        bool found = false;
        std::stringstream ss(list);
        std::string part;
        while (std::getline(ss, part, ',')) {
            auto dash = part.find('-');
            if (dash == std::string::npos) {
                found = found || (std::stoi(part) == cpu);
            } else {
                found = found || (cpu >= std::stoi(part.substr(0, dash)) && cpu <= std::stoi(part.substr(dash + 1)));
            }
        }
        check(found, "explicit NUMA node 1 request stays on node 1 (CPU " + std::to_string(cpu) + ")");
        std::string msg;
        check(throws([&] { alloc.acquire(infra::CpuAutoBindRequest{99}, "bad"); }, &msg) && msg.find("NUMA node 99") != std::string::npos,
            "unknown NUMA node throws");
    }

    void reserveTest()
    {
        // Exhaust the allocator while one core is explicitly reserved: the
        // reserved core must be the only one left over.
        Alloc probe{Cfg{}};
        auto usable = probe.usableCores();
        if (usable.size() < 2) {
            std::cout << "skip: not enough cores\n";
            return;
        }
        auto reservedCore = std::set<int>(usable.front().begin(), usable.front().end());
        Alloc alloc{Cfg{}};
        alloc.reserve({usable.front().front()}, "explicit");
        std::vector<int> got;
        for (std::size_t i = 0; i + 1 < usable.size(); ++i) {
            got.push_back(alloc.acquire(infra::CpuAutoBindRequest{}, "a" + std::to_string(i)));
        }
        bool reservedTaken = false;
        for (int c : got) {
            reservedTaken = reservedTaken || (coreSiblings(c) == reservedCore);
        }
        check(!reservedTaken && got.size() + 1 == usable.size(), "an explicitly reserved core is never auto-assigned");
        check(throws([&] { alloc.acquire(infra::CpuAutoBindRequest{}, "overflow"); }), "only the reserved core remains, so acquire throws");
        std::string msg;
        int assigned = got.front();
        check(throws([&] { alloc.reserve({assigned}, "conflicting"); }, &msg) && msg.find("already automatically assigned") != std::string::npos,
            "reserving an automatically assigned CPU throws");
    }

    void configureOnceTest()
    {
        // Force the process-wide instance to load, then configure() must refuse.
        Alloc::instance().allowedList();
        std::string msg;
        check(throws([&] { Alloc::configure(Cfg{}); }, &msg) && std::string(msg).find("already in use") != std::string::npos,
            "configure() after first use throws std::logic_error");
    }

    void concurrentTest()
    {
        Alloc alloc{Cfg{}};
        auto usable = alloc.usableCores();
        std::atomic<int> conflicts{0}, acquired{0};
        std::mutex m;
        std::set<std::set<int>> seen;
        std::vector<std::thread> ts;
        for (int t = 0; t < 8; ++t) {
            ts.emplace_back([&, t] {
                for (int i = 0; i < 200; ++i) {
                    int c = -1;
                    try {
                        c = alloc.acquire(infra::CpuAutoBindRequest{}, "c" + std::to_string(t));
                    } catch (std::exception const &) {
                        continue; // exhausted, fine
                    }
                    auto sib = coreSiblings(c);
                    {
                        std::lock_guard<std::mutex> _(m);
                        if (!seen.insert(sib).second) {
                            ++conflicts;
                        }
                    }
                    acquired.fetch_add(1);
                    // Erase before releasing: the allocator still holds the core, so
                    // no other thread can acquire it inside this window.
                    {
                        std::lock_guard<std::mutex> _(m);
                        seen.erase(sib);
                    }
                    alloc.release({c});
                }
            });
        }
        for (auto &t : ts) {
            t.join();
        }
        check(acquired.load() > 0 && conflicts.load() == 0,
            "concurrent acquire/release never shares a core (" + std::to_string(acquired.load()) + " acquisitions)");
        (void)usable;
    }

    // ---- graph integration ----------------------------------------------
    using Clock = std::chrono::system_clock;
    struct ClockComponent {
        using TimePointType = Clock::time_point;
        using DurationType = Clock::duration;
        static constexpr bool PreserveInputRelativeOrder = false;
        static constexpr bool CanBeActualTimeClock = true;
        TimePointType now() { return Clock::now(); }
        TimePointType resolveTime() { return Clock::now(); }
        TimePointType resolveTime(TimePointType const &) { return Clock::now(); }
        void log(infra::LogLevel, std::string const &) {}
    };
    using BaseEnv = infra::Environment<infra::CheckTimeComponent<false>, infra::TrivialExitControlComponent, ClockComponent>;
    using DedicatedEnv = infra::Environment<infra::CheckTimeComponent<false>, infra::TrivialExitControlComponent, ClockComponent, basic::HwlocThreadCpuAllocatorComponent>;
    using MoodyEnv = infra::Environment<infra::CheckTimeComponent<false>, infra::TrivialExitControlComponent, ClockComponent, basic::MoodycamelThreadedExecutorComponent, basic::HwlocThreadCpuAllocatorComponent>;
    // SharedPool has no dedicated per-node thread, so it must reject CpuAutoBind
    // even when an allocator is available.
    using PoolEnv = infra::Environment<infra::CheckTimeComponent<false>, infra::TrivialExitControlComponent, ClockComponent, basic::RealTimeSharedPoolExecutorComponent, basic::HwlocThreadCpuAllocatorComponent>;

    std::vector<int> affinityOf(std::thread::native_handle_type h)
    {
        std::vector<int> r;
#ifdef __linux__
        cpu_set_t set;
        CPU_ZERO(&set);
        if (pthread_getaffinity_np(h, sizeof(set), &set) == 0) {
            for (int c = 0; c < CPU_SETSIZE; ++c) {
                if (CPU_ISSET(c, &set)) {
                    r.push_back(c);
                }
            }
        }
#endif
        return r;
    }

    template <class Env>
    void graphAutoBind(std::string const &tag)
    {
        using M = infra::RealTimeApp<Env>;
        using TP = Clock::time_point;
        auto node = M::template liftPure<int>([](int &&x) { return x; }, infra::LiftParameters<TP>().SuggestThreaded(true));
        bool ok = false;
        std::string msg;
        if (throws([&] { ok = M::configureThreadedNode(node, infra::ThreadedNodeOptions().CpuAutoBind()); }, &msg)) {
            check(false, "graph " + tag + ": CpuAutoBind threw: " + msg);
            return;
        }
        auto h = M::threadHandle(node);
        auto aff = h ? affinityOf(*h) : std::vector<int>{};
        check(ok && aff.size() == 1, "graph " + tag + ": CpuAutoBind pins the node thread to one CPU");
        if (!aff.empty()) {
            std::cout << "      " << tag << " node pinned to CPU " << aff.front() << '\n';
        }
        // nodes deliberately leaked: their threads are joined only on destruction
    }

    void graphOnNumaTest()
    {
        std::ifstream node1("/sys/devices/system/node/node1/cpulist");
        if (!node1) {
            std::cout << "skip: single NUMA node\n";
            return;
        }
        using M = infra::RealTimeApp<DedicatedEnv>;
        using TP = Clock::time_point;
        auto node = M::template liftPure<int>([](int &&x) { return x; }, infra::LiftParameters<TP>().SuggestThreaded(true));
        std::string msg;
        bool ok = false;
        if (throws([&] { ok = M::configureThreadedNode(node, infra::ThreadedNodeOptions().CpuAutoBindOnNuma(1)); }, &msg)) {
            check(false, "graph CpuAutoBindOnNuma(1) threw: " + msg);
            return;
        }
        auto h = M::threadHandle(node);
        auto aff = h ? affinityOf(*h) : std::vector<int>{};
        std::set<int> n1;
        std::string list;
        std::getline(node1, list);
        std::stringstream ss(list);
        std::string part;
        while (std::getline(ss, part, ',')) {
            auto dash = part.find('-');
            int lo = std::stoi(part.substr(0, dash));
            int hi = dash == std::string::npos ? lo : std::stoi(part.substr(dash + 1));
            for (int c = lo; c <= hi; ++c) {
                n1.insert(c);
            }
        }
        check(ok && aff.size() == 1 && n1.count(aff.front()), "graph CpuAutoBindOnNuma(1): node thread pinned to one CPU on NUMA node 1");
        check(throws([&] { M::configureThreadedNode(node, infra::ThreadedNodeOptions().CpuAutoBindOnNuma(99)); }),
            "graph CpuAutoBindOnNuma(99): unknown NUMA node throws");
    }

    void graphTest()
    {
        graphAutoBind<DedicatedEnv>("dedicated");
        graphAutoBind<MoodyEnv>("moodycamel");
        graphOnNumaTest();
        using M = infra::RealTimeApp<PoolEnv>;
        using TP = Clock::time_point;
        auto node = M::template liftPure<int>([](int &&x) { return x; }, infra::LiftParameters<TP>().SuggestThreaded(true));
        std::string msg;
        check(throws([&] { M::configureThreadedNode(node, infra::ThreadedNodeOptions().CpuAutoBind()); }, &msg),
            "graph sharedpool: CpuAutoBind rejected with an error");
        std::cout << "      sharedpool message: " << msg << '\n';
    }
} // namespace

int main()
{
    // The graph tests use the process-wide allocator.
    basic::HwlocThreadCpuAllocator::configure(basic::HwlocCpuAllocatorConfig{});
    acquisitionTest();
    firstCoreTest();
    excludeTest();
    numaTest();
    reserveTest();
    configureOnceTest();
    concurrentTest();
    graphTest();
    std::cout << (failures ? "FAILED" : "ALL PASSED") << '\n';
    return failures ? 1 : 0;
}

#else
int main()
{
    std::cout << "hwloc CPU allocator not built\n";
    return 0;
}
#endif
