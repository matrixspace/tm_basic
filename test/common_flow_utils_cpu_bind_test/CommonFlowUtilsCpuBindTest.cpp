#include <tm_kit/basic/CommonFlowUtils.hpp>
#include <tm_kit/basic/HwlocThreadCpuAllocator.hpp>
#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace dev::cd606::tm;

// CommonFlowUtils self-threaded nodes (bunch / bunchWithProcessing, delayer,
// actionWithBufferedState) must be pinnable through
// RealTimeApp::configureThreadedNode just like executor-threaded nodes. Their
// internal threads are kept joinable and joined on destruction; the native
// handle is cached while the thread is joinable. This test verifies that
// CpuAutoBind() and Cpus() actually change the node thread's affinity.

#ifdef TM_KIT_BASIC_HAS_HWLOC_CPU_ALLOCATOR

namespace
{
    int failures = 0;
    void check(bool ok, std::string const &what)
    {
        std::cout << (ok ? "ok:   " : "FAIL: ") << what << '\n';
        failures += !ok;
    }

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

    // ---- clock / environment -------------------------------------------
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
        void sleepFor(DurationType const &d) { std::this_thread::sleep_for(d); }
    };
    using Env = infra::Environment<infra::CheckTimeComponent<false>, infra::TrivialExitControlComponent, ClockComponent, basic::HwlocThreadCpuAllocatorComponent>;
    using M = infra::RealTimeApp<Env>;
    using CFU = basic::CommonFlowUtilComponents<M>;

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

    // Any CPU the allocator considers usable is safe to request explicitly.
    std::optional<int> aUsableCpu()
    {
        auto usable = basic::HwlocThreadCpuAllocator::instance().usableCores();
        if (usable.empty()) {
            return std::nullopt;
        }
        return usable.back().front();
    }

    // ---- node callbacks ------------------------------------------------
    // bunchWithProcessing's buncher is called as operator()(std::vector<T>&&).
    struct IntBuncher {
        std::vector<int> operator()(std::vector<int> &&v) { return std::move(v); }
    };

    // actionWithBufferedState callbacks: only need to compile and default-
    // construct; the node is never fed data here, so they never actually run.
    using StateT = int;
    struct Updater {
        void operator()(int &&, StateT &) {}
    };
    struct Producer {
        int operator()(StateT &, bool *) { return 0; }
    };
    struct Resetter {
        void operator()(StateT &) {}
    };

    // Each CommonFlowUtils self-threaded node keeps its worker thread joinable
    // and joins it on destruction. To keep the affinity assertions independent of
    // the node's own teardown, this test runs each node in a child process that
    // never destroys it: the child _exit()s and the OS reclaims both the thread
    // and the node. This mirrors the fork-based isolation in
    // test/cpu_cross_process_test/CrossProcessCpuTest.cpp.
    template <class MakeNode>
    void testNode(MakeNode makeNode, std::string const &name)
    {
#ifdef __linux__
        pid_t pid = ::fork();
        if (pid < 0) {
            check(false, name + ": fork failed");
            return;
        }
        if (pid == 0) {
            // Child: own copy of the failure counter. Both nodes stay alive until
            // _exit(), so their destructors never race with the detached threads.
            failures = 0;
            using Node = decltype(makeNode());
            Node node1 = makeNode();
            {
                bool ok = false;
                std::string msg;
                if (throws([&] { ok = M::configureThreadedNode(node1, infra::ThreadedNodeOptions().CpuAutoBind()); }, &msg)) {
                    check(false, name + ": CpuAutoBind threw: " + msg);
                } else {
                    auto h = M::threadHandle(node1);
                    auto aff = h ? affinityOf(*h) : std::vector<int>{};
                    check(ok && aff.size() == 1, name + ": CpuAutoBind pins the node thread to one CPU");
                }
            }
            {
                auto cpu = aUsableCpu();
                if (!cpu) {
                    std::cout << "skip: no usable CPU for explicit Cpus()\n";
                } else {
                    Node node2 = makeNode();
                    bool ok = false;
                    std::string msg;
                    if (throws([&] { ok = M::configureThreadedNode(node2, infra::ThreadedNodeOptions().Cpus({*cpu})); }, &msg)) {
                        check(false, name + ": Cpus({...}) threw: " + msg);
                    } else {
                        auto h = M::threadHandle(node2);
                        auto aff = h ? affinityOf(*h) : std::vector<int>{};
                        check(ok && aff.size() == 1 && aff.front() == *cpu, name + ": Cpus() pins the node thread to the requested CPU");
                    }
                }
            }
            std::cout.flush();
            ::_exit(failures ? 1 : 0);
        }
        int status = 0;
        if (::waitpid(pid, &status, 0) < 0) {
            check(false, name + ": waitpid failed");
            return;
        }
        if (!WIFEXITED(status)) {
            check(false, name + ": child did not exit normally");
            return;
        }
        failures += (WEXITSTATUS(status) != 0);
#else
        // No fork/affinity support here; the Linux-only build exercises the path
        // above, so this fallback just reports a skip rather than running checks
        // that cannot observe affinity.
        std::cout << "skip: " << name << " (no fork/affinity support)\n";
#endif
    }
} // namespace

int main()
{
    basic::HwlocThreadCpuAllocator::configure(basic::HwlocCpuAllocatorConfig{});

    testNode([] { return CFU::bunch<int>(); }, "bunch<int>");
    testNode([] { return CFU::bunchWithProcessing<int, IntBuncher>(); }, "bunchWithProcessing<int,IntBuncher>");
    testNode([] { return CFU::delayer<int>(std::chrono::milliseconds(5)); }, "delayer<int>");
    testNode([] { return CFU::actionWithBufferedState<int, int, StateT, Updater, Producer, Resetter>(); }, "actionWithBufferedState");

    std::cout << (failures ? "FAILED" : "ALL PASSED") << '\n';
    return failures ? 1 : 0;
}

#else
int main()
{
    std::cout << "skip: hwloc CPU allocator not built\n";
    return 0;
}
#endif
