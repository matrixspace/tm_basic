#include <tm_kit/basic/HwlocThreadCpuAllocator.hpp>
#include <tm_kit/infra/Environments.hpp>
#include <tm_kit/infra/RealTimeApp.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef __linux__
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Helper for run_cross_process.sh: one process of a multi-process scenario.
// The lock directory comes from TM_CPU_LOCK_DIR so tests never touch the
// machine-wide default. Output lines are parsed by the script:
//   cpu <n>        a thread was pinned to CPU n
//   READY          all bindings done, now holding
//   BINDERR <msg>  CpuAutoBindError / ThreadCpuBindError (exit code 3)
//
// Modes:
//   hold <threads> <seconds>           CrossProcess bindCurrentThread, then hold
//   excl <cpu> <threads> <seconds>     same with excludeCpus = {cpu}
//   explicit <cpu> <seconds>           bindCurrentThreadTo({cpu})
//   local <seconds>                    Local component (default behaviour)
//   mix                                Local then CrossProcess: must throw logic_error
//   stress <threads> <iters> <shm>     bind/reset loop, overlap counted in shared memory
//   threadexit <seconds>               bind on a thread that exits, then hold
//   fork <seconds>                     bind, fork a child that stays alive, release
//   graph <seconds>                    RealTimeApp node with CpuAutoBind()
//   exhaust                            bind until CpuAutoBindError, print count
#if defined(TM_KIT_BASIC_HAS_HWLOC_CPU_ALLOCATOR) && defined(__linux__)

using namespace dev::cd606::tm;

namespace
{
    using Alloc = basic::HwlocThreadCpuAllocator;
    using Cross = basic::HwlocCrossProcessCpuAllocatorComponent;
    using Local = basic::HwlocThreadCpuAllocatorComponent;
    using Lease = infra::ThreadCpuLease;

    std::mutex outMutex;
    void say(std::string const &s)
    {
        std::lock_guard<std::mutex> _(outMutex);
        std::cout << s << std::endl;
    }

    void configure(std::vector<int> exclude = {})
    {
        basic::HwlocCpuAllocatorConfig cfg;
        char const *dir = std::getenv("TM_CPU_LOCK_DIR");
        if (!dir || !*dir) {
            std::cerr << "TM_CPU_LOCK_DIR must be set\n";
            std::exit(2);
        }
        cfg.crossProcessLockDir = dir;
        cfg.excludeCpus = std::move(exclude);
        Alloc::configure(cfg);
    }

    void holdFor(int seconds)
    {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
    }

    // First PU of the physical core holding cpu (the allocator's lock key on
    // machines where sysfs lists siblings in ascending order).
    int coreKey(int cpu)
    {
        std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list");
        std::string s;
        std::getline(in, s);
        try {
            return std::stoi(s);
        } catch (...) {
            return cpu;
        }
    }

    // Binds n threads (each with its own lease) and keeps them until seconds pass.
    template <class Env>
    int bindThreadsAndHold(int n, int seconds)
    {
        std::atomic<int> ready{0}, failed{0};
        std::vector<std::thread> ts;
        for (int i = 0; i < n; ++i) {
            ts.emplace_back([&, i] {
                try {
                    auto l = Lease::bindCurrentThread<Env>("t" + std::to_string(i));
                    say("cpu " + std::to_string(l.cpu()));
                    ++ready;
                    holdFor(seconds);
                } catch (std::exception const &e) {
                    say(std::string("BINDERR ") + e.what());
                    ++failed;
                    ++ready;
                }
            });
        }
        while (ready.load() < n) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        say("READY");
        for (auto &t : ts) {
            t.join();
        }
        return failed.load() ? 3 : 0;
    }

    int stress(int threads, int iters, std::string const &shmPath)
    {
        int fd = ::open(shmPath.c_str(), O_RDWR);
        if (fd < 0) {
            std::cerr << "cannot open " << shmPath << '\n';
            return 2;
        }
        constexpr int kSlots = 1024;
        void *p = ::mmap(nullptr, sizeof(std::atomic<int>) * kSlots, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) {
            std::cerr << "mmap failed\n";
            return 2;
        }
        auto *occupancy = static_cast<std::atomic<int> *>(p);
        std::atomic<int> overlaps{0}, bound{0}, busy{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < threads; ++t) {
            ts.emplace_back([&, t] {
                for (int i = 0; i < iters; ++i) {
                    Lease l;
                    try {
                        l = Lease::bindCurrentThread<Cross>("stress" + std::to_string(t));
                    } catch (basic::CpuAutoBindError const &) {
                        ++busy; // every free core taken by someone right now
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                        continue;
                    }
                    int key = coreKey(l.cpu()) % kSlots;
                    if (occupancy[key].fetch_add(1) != 0) {
                        ++overlaps;
                    }
                    ++bound;
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                    occupancy[key].fetch_sub(1);
                    l.reset();
                }
            });
        }
        for (auto &t : ts) {
            t.join();
        }
        say("stress bound=" + std::to_string(bound.load()) + " busy=" + std::to_string(busy.load()) + " overlaps=" + std::to_string(overlaps.load()));
        return overlaps.load() == 0 && bound.load() > 0 ? 0 : 1;
    }

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
    using GraphEnv = infra::Environment<infra::CheckTimeComponent<false>, infra::TrivialExitControlComponent, ClockComponent, Cross>;
    using LocalGraphEnv = infra::Environment<infra::CheckTimeComponent<false>, infra::TrivialExitControlComponent, ClockComponent, Local>;
    template <class T>
    struct TypeTag {
        using type = T;
    };

    int graph(int seconds)
    {
        using M = infra::RealTimeApp<GraphEnv>;
        auto node = M::liftPure<int>([](int &&x) { return x; }, infra::LiftParameters<Clock::time_point>().SuggestThreaded(true));
        M::configureThreadedNode(node, infra::ThreadedNodeOptions().CpuAutoBind());
        auto h = M::threadHandle(node);
        cpu_set_t set;
        CPU_ZERO(&set);
        if (!h || pthread_getaffinity_np(*h, sizeof(set), &set) != 0 || CPU_COUNT(&set) != 1) {
            std::cerr << "node thread not pinned to one CPU\n";
            return 1;
        }
        for (int c = 0; c < CPU_SETSIZE; ++c) {
            if (CPU_ISSET(c, &set)) {
                say("cpu " + std::to_string(c));
            }
        }
        say("READY");
        holdFor(seconds);
        return 0;
    }

    int run(int argc, char **argv)
    {
        if (argc < 2) {
            std::cerr << "usage: see source\n";
            return 2;
        }
        std::string const mode = argv[1];
        auto arg = [&](int i) { return i < argc ? std::atoi(argv[i]) : 0; };

        if (mode == "hold") {
            configure();
            return bindThreadsAndHold<Cross>(arg(2), arg(3));
        }
        if (mode == "excl") {
            configure({arg(2)});
            return bindThreadsAndHold<Cross>(arg(3), arg(4));
        }
        if (mode == "local") {
            configure();
            return bindThreadsAndHold<Local>(1, arg(2));
        }
        if (mode == "explicit") {
            configure();
            auto l = Lease::bindCurrentThreadTo<Cross>({arg(2)}, "explicit");
            say("cpu " + std::to_string(l.cpu()));
            say("READY");
            holdFor(arg(3));
            return 0;
        }
        if (mode == "selfexcl") {
            // selfexcl <local|cross> <lease|graph> <cpu>: excludeCpus = {cpu}, then
            // an explicit binding to that CPU in the same process must throw.
            std::string const scope = argc > 2 ? argv[2] : "";
            std::string const how = argc > 3 ? argv[3] : "";
            int const cpu = arg(4);
            configure({cpu});
            auto attempt = [&](auto envTag) -> int {
                using Env = typename decltype(envTag)::type;
                try {
                    if (how == "lease") {
                        auto l = Lease::bindCurrentThreadTo<Env>({cpu}, "selfexcl");
                        say("cpu " + std::to_string(l.cpu()));
                    } else {
                        using M = infra::RealTimeApp<Env>;
                        auto node = M::template liftPure<int>([](int &&x) { return x; }, infra::LiftParameters<Clock::time_point>().SuggestThreaded(true));
                        bool ok = M::configureThreadedNode(node, infra::ThreadedNodeOptions().Cpus({cpu}));
                        say(std::string("configured ") + (ok ? "true" : "false"));
                    }
                } catch (basic::CpuAutoBindError const &e) {
                    say(std::string("REJECTED ") + e.what());
                    return 0;
                }
                std::cerr << "explicit binding to excluded CPU was accepted\n";
                return 1;
            };
            if (scope == "local") {
                return attempt(TypeTag<LocalGraphEnv>{});
            }
            return attempt(TypeTag<GraphEnv>{});
        }
        if (mode == "mix") {
            configure();
            auto l = Lease::bindCurrentThread<Local>("local");
            try {
                Cross::threadCpuAllocator();
            } catch (std::logic_error const &e) {
                say(std::string("MIXERR ") + e.what());
                return 0;
            }
            std::cerr << "mixing components did not throw\n";
            return 1;
        }
        if (mode == "stress") {
            configure();
            return stress(arg(2), arg(3), argc > 4 ? argv[4] : "");
        }
        if (mode == "threadexit") {
            configure();
            int cpu = -1;
            std::thread([&] {
                auto l = Lease::bindCurrentThread<Cross>("short-lived");
                cpu = l.cpu();
            }).join();
            say("released " + std::to_string(cpu));
            say("READY");
            holdFor(arg(2));
            return 0;
        }
        if (mode == "fork") {
            configure();
            auto l = Lease::bindCurrentThread<Cross>("parent");
            int const cpu = l.cpu();
            pid_t child = ::fork();
            if (child == 0) {
                // Stays alive without exec; must not keep the core busy.
                ::sleep((unsigned)arg(2));
                ::_exit(0);
            }
            say("child " + std::to_string(child));
            l.reset();
            say("released " + std::to_string(cpu));
            say("READY");
            int status = 0;
            ::waitpid(child, &status, 0);
            return 0;
        }
        if (mode == "graph") {
            configure();
            return graph(arg(2));
        }
        if (mode == "exclnow") {
            // exclnow <cpu> <seconds>: configure(cfg, CrossProcess) only, no
            // binding: excludeCpus must already keep other processes away.
            basic::HwlocCpuAllocatorConfig cfg;
            cfg.crossProcessLockDir = std::getenv("TM_CPU_LOCK_DIR");
            cfg.excludeCpus = {arg(2)};
            Alloc::configure(cfg, basic::CpuAllocatorScope::CrossProcess);
            say("READY");
            holdFor(arg(3));
            return 0;
        }
        if (mode == "forkexcl") {
            // forkexcl <cpu> <seconds>: excludeCpus={cpu}, parent binds, a child
            // forked without exec binds too (must succeed, off the excluded core).
            configure({arg(2)});
            auto l = Lease::bindCurrentThread<Cross>("parent");
            say("cpu " + std::to_string(l.cpu()));
            pid_t child = ::fork();
            if (child == 0) {
                // A fresh thread: the forked copy of this thread still owns the
                // parent's lease object, so it may not bind a second time.
                int rc = 0;
                std::thread([&] {
                    try {
                        auto c = Lease::bindCurrentThread<Cross>("child");
                        say("childcpu " + std::to_string(c.cpu()));
                    } catch (std::exception const &e) {
                        say(std::string("CHILDERR ") + e.what());
                        rc = 3;
                    }
                }).join();
                std::cout.flush();
                ::_exit(rc);
            }
            int status = 0;
            ::waitpid(child, &status, 0);
            say("childexit " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : 99));
            say("READY");
            holdFor(arg(3));
            return 0;
        }
        if (mode == "rollback") {
            // rollback <cpuA> <cpuB>: the lock file of B's core is made a
            // directory, so reserving {A, B} throws from tryLockCore after A's
            // core is locked. A's lock must be rolled back (printed as FREE).
            configure();
            auto &a = Cross::threadCpuAllocator();
            std::string const dir = std::getenv("TM_CPU_LOCK_DIR");
            int const keyB = coreKey(arg(3));
            ::mkdir(dir.c_str(), 01777);
            std::string const bad = dir + "/core-" + std::to_string(keyB) + ".lock";
            ::unlink(bad.c_str()); // an earlier scenario may have created the regular file
            if (::mkdir(bad.c_str(), 0755) != 0) {
                std::cerr << "cannot create " << bad << '\n';
                return 2;
            }
            bool threw = false;
            try {
                a.reserve({arg(2), arg(3)}, "rollback");
            } catch (basic::CpuAutoBindError const &e) {
                threw = true;
                say(std::string("THREW ") + e.what());
            }
            ::rmdir(bad.c_str());
            std::string const lockA = dir + "/core-" + std::to_string(coreKey(arg(2))) + ".lock";
            int fd = ::open(lockA.c_str(), O_RDWR | O_CLOEXEC);
            bool free = fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0;
            if (fd >= 0) {
                ::close(fd);
            }
            say(free ? "FREE" : "LEAKED");
            return threw && free ? 0 : 1;
        }
        if (mode == "forkrace") {
            // forkrace <forks>: one thread keeps binding/resetting while the
            // main thread forks children that stay alive. Once the binder has
            // stopped, this process holds nothing, so another process must get
            // every core although the children are still alive.
            configure();
            std::atomic<bool> stop{false};
            std::thread binder([&] {
                while (!stop.load()) {
                    try {
                        auto l = Lease::bindCurrentThread<Cross>("binder");
                    } catch (std::exception const &) {
                    }
                }
            });
            std::vector<pid_t> kids;
            for (int i = 0; i < arg(2); ++i) {
                pid_t c = ::fork();
                if (c == 0) {
                    ::sleep(30); // killed by the parent
                    ::_exit(0);
                }
                kids.push_back(c);
            }
            stop = true;
            binder.join();
            // Every lease is gone in this process; children are alive.
            for (pid_t k : kids) {
                say("child " + std::to_string(k));
            }
            say("READY");
            holdFor(30);
            return 0;
        }
        if (mode == "forkexclorphan") {
            // forkexclorphan <cpu> <seconds>: excludeCpus={cpu}; fork a child
            // that binds and holds while the parent is still alive; only after
            // the child has bound does the parent exit. The child never calls
            // acquire again, so the excluded core must stay locked purely
            // through the lock the child inherited.
            configure({arg(2)});
            Cross::threadCpuAllocator();
            pid_t const parentPid = ::getpid();
            int ready[2];
            if (::pipe(ready) != 0) {
                std::cerr << "pipe failed\n";
                return 2;
            }
            pid_t child = ::fork();
            if (child == 0) {
                ::close(ready[0]);
                ::setsid();
                std::thread([&] {
                    try {
                        auto c = Lease::bindCurrentThread<Cross>("orphan");
                        say("childcpu " + std::to_string(c.cpu()));
                        say("pid " + std::to_string(::getpid()));
                        std::cout.flush();
                        char ok = 'R';
                        (void)!::write(ready[1], &ok, 1);
                        ::close(ready[1]);
                        // wait until the parent is gone before reporting READY
                        while (::getppid() == parentPid) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                        say("READY");
                        holdFor(arg(3));
                    } catch (std::exception const &e) {
                        say(std::string("CHILDERR ") + e.what());
                        ::close(ready[1]);
                    }
                }).join();
                std::cout.flush();
                ::_exit(0);
            }
            ::close(ready[1]);
            char ok = 0;
            ssize_t n = ::read(ready[0], &ok, 1);
            ::close(ready[0]);
            // parent exits only after the child has bound
            return (n == 1 && ok == 'R') ? 0 : 3;
        }
        if (mode == "fillnuma") {
            // fillnuma <node> <leave> <seconds>: take every free core of NUMA
            // node, give `leave` back, then hold the rest.
            configure();
            auto &a = Cross::threadCpuAllocator();
            std::vector<int> got;
            try {
                for (;;) {
                    got.push_back(a.acquire(infra::CpuAutoBindRequest{arg(2)}, "fill"));
                }
            } catch (basic::CpuAutoBindError const &) {
            }
            for (int i = 0; i < arg(3) && !got.empty(); ++i) {
                a.release({got.back()});
                got.pop_back();
            }
            say("held " + std::to_string(got.size()));
            say("READY");
            holdFor(arg(4));
            return 0;
        }
        if (mode == "exhaust") {
            configure();
            auto &a = Cross::threadCpuAllocator();
            int n = 0;
            try {
                for (;;) {
                    a.acquire(infra::CpuAutoBindRequest{}, "exhaust");
                    ++n;
                }
            } catch (basic::CpuAutoBindError const &e) {
                say("count " + std::to_string(n));
                say(std::string("BINDERR ") + e.what());
            }
            return 0;
        }
        std::cerr << "unknown mode " << mode << '\n';
        return 2;
    }
} // namespace

int main(int argc, char **argv)
{
    try {
        return run(argc, argv);
    } catch (basic::CpuAutoBindError const &e) {
        say(std::string("BINDERR ") + e.what());
        return 3;
    } catch (infra::ThreadCpuBindError const &e) {
        say(std::string("BINDERR ") + e.what());
        return 3;
    } catch (std::exception const &e) {
        std::cerr << "ERROR " << e.what() << '\n';
        return 1;
    }
}

#else
#include <iostream>
int main()
{
    std::cout << "skip: hwloc CPU allocator unavailable or not Linux\n";
    return 77;
}
#endif
