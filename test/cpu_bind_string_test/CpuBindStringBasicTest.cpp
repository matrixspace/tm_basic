// Tests for tm_basic CPU bind string extensions:
//   HwlocCpuAllocatorConfig::fromString  (parsing only, no hwloc calls)
//   SharedThreadPoolConfig::cpusFromString
//
// These tests validate the string parsing layer independently of hwloc
// availability, so they run unconditionally.

#include <tm_kit/basic/HwlocThreadCpuAllocator.hpp>
#include <tm_kit/basic/RealTimeSharedPoolExecutor.hpp>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dev::cd606::tm;

namespace
{
    int failures = 0;
    void check(bool ok, std::string const &name)
    {
        std::printf("%s: %s\n", ok ? "ok" : "FAIL", name.c_str());
        failures += !ok;
    }
    bool throws(std::function<void()> f)
    {
        try { f(); return false; }
        catch (...) { return true; }
    }
    bool eq(std::vector<int> const &a, std::vector<int> const &b)
    {
        return a == b;
    }

#ifdef TM_KIT_BASIC_HAS_HWLOC_CPU_ALLOCATOR

    // ── HwlocCpuAllocatorConfig::fromString ───────────────────────────────────

    void testHwlocConfigFromString()
    {
        using basic::HwlocCpuAllocatorConfig;
        using basic::CpuAllocatorScope;
        using basic::SmtPolicy;

        // empty string -> defaults
        {
            auto cfg = HwlocCpuAllocatorConfig::fromString("");
            check(cfg.excludeCpus.empty(), "fromString(''): excludeCpus empty");
            check(cfg.avoidFirstCoreOfNumaNode == true,
                "fromString(''): avoidFirstCoreOfNumaNode default true");
            check(cfg.smt == SmtPolicy::RequireFullCore,
                "fromString(''): smt default RequireFullCore");
            check(cfg.crossProcessLockDir == "/dev/shm/tm_kit_cpu_locks",
                "fromString(''): lockdir default");
        }
        // exclude CPUs
        {
            auto cfg = HwlocCpuAllocatorConfig::fromString("exclude=8,9");
            check(eq(cfg.excludeCpus, {8, 9}), "fromString exclude=8,9");
        }
        // exclude with range
        {
            auto cfg = HwlocCpuAllocatorConfig::fromString("exclude=4-7");
            check(eq(cfg.excludeCpus, {4, 5, 6, 7}), "fromString exclude=4-7");
        }
        // avoid_first false
        {
            auto cfg = HwlocCpuAllocatorConfig::fromString("avoid_first=false");
            check(cfg.avoidFirstCoreOfNumaNode == false,
                "fromString avoid_first=false");
        }
        {
            auto cfg = HwlocCpuAllocatorConfig::fromString("avoid_first=0");
            check(cfg.avoidFirstCoreOfNumaNode == false,
                "fromString avoid_first=0");
        }
        {
            auto cfg = HwlocCpuAllocatorConfig::fromString("avoid_first=no");
            check(cfg.avoidFirstCoreOfNumaNode == false,
                "fromString avoid_first=no");
        }
        // avoid_first true (default) with explicit value
        {
            auto cfg = HwlocCpuAllocatorConfig::fromString("avoid_first=true");
            check(cfg.avoidFirstCoreOfNumaNode == true,
                "fromString avoid_first=true");
        }
        // smt partial
        {
            auto cfg = HwlocCpuAllocatorConfig::fromString("smt=partial");
            check(cfg.smt == SmtPolicy::AllowPartialCore,
                "fromString smt=partial");
        }
        {
            auto cfg = HwlocCpuAllocatorConfig::fromString("smt=allow_partial");
            check(cfg.smt == SmtPolicy::AllowPartialCore,
                "fromString smt=allow_partial");
        }
        {
            auto cfg = HwlocCpuAllocatorConfig::fromString("smt=require_full");
            check(cfg.smt == SmtPolicy::RequireFullCore,
                "fromString smt=require_full");
        }
        // scope output param
        {
            CpuAllocatorScope scope = CpuAllocatorScope::Local;
            HwlocCpuAllocatorConfig::fromString("scope=cross", &scope);
            check(scope == CpuAllocatorScope::CrossProcess,
                "fromString scope=cross");
        }
        {
            CpuAllocatorScope scope = CpuAllocatorScope::CrossProcess;
            HwlocCpuAllocatorConfig::fromString("scope=local", &scope);
            check(scope == CpuAllocatorScope::Local,
                "fromString scope=local");
        }
        // scope=cross_process
        {
            CpuAllocatorScope scope = CpuAllocatorScope::Local;
            HwlocCpuAllocatorConfig::fromString("scope=cross_process", &scope);
            check(scope == CpuAllocatorScope::CrossProcess,
                "fromString scope=cross_process");
        }
        // lockdir
        {
            auto cfg = HwlocCpuAllocatorConfig::fromString("lockdir=/tmp/my_locks");
            check(cfg.crossProcessLockDir == "/tmp/my_locks",
                "fromString lockdir=/tmp/my_locks");
        }
        // multiple keys, semicolon separated
        {
            CpuAllocatorScope scope = CpuAllocatorScope::Local;
            auto cfg = HwlocCpuAllocatorConfig::fromString(
                "exclude=8,9;avoid_first=false;smt=partial;scope=cross;lockdir=/run/locks",
                &scope);
            check(eq(cfg.excludeCpus, {8, 9}),
                "fromString multi: excludeCpus");
            check(cfg.avoidFirstCoreOfNumaNode == false,
                "fromString multi: avoidFirstCoreOfNumaNode");
            check(cfg.smt == SmtPolicy::AllowPartialCore,
                "fromString multi: smt");
            check(scope == CpuAllocatorScope::CrossProcess,
                "fromString multi: scope");
            check(cfg.crossProcessLockDir == "/run/locks",
                "fromString multi: lockdir");
        }
        // case-insensitive keys
        {
            auto cfg = HwlocCpuAllocatorConfig::fromString("EXCLUDE=2;SMT=PARTIAL");
            check(eq(cfg.excludeCpus, {2}), "fromString case-insensitive EXCLUDE");
            check(cfg.smt == SmtPolicy::AllowPartialCore,
                "fromString case-insensitive SMT=PARTIAL");
        }
        // whitespace around tokens
        {
            auto cfg = HwlocCpuAllocatorConfig::fromString("  exclude = 3 ");
            check(eq(cfg.excludeCpus, {3}), "fromString whitespace trim");
        }
        // unknown key throws
        {
            check(throws([] { basic::HwlocCpuAllocatorConfig::fromString("badkey=val"); }),
                "fromString unknown key throws");
        }
        // missing '=' throws
        {
            check(throws([] { basic::HwlocCpuAllocatorConfig::fromString("exclude"); }),
                "fromString missing = throws");
        }
        // bad smt value throws
        {
            check(throws([] { basic::HwlocCpuAllocatorConfig::fromString("smt=weird"); }),
                "fromString bad smt value throws");
        }
        // bad scope value throws
        {
            check(throws([] {
                basic::CpuAllocatorScope s = basic::CpuAllocatorScope::Local;
                basic::HwlocCpuAllocatorConfig::fromString("scope=weird", &s);
            }), "fromString bad scope value throws");
        }
    }

#else
    void testHwlocConfigFromString()
    {
        std::printf("skip: HwlocCpuAllocatorConfig::fromString (hwloc not available)\n");
    }
#endif

    // ── SharedThreadPoolConfig::cpusFromString ────────────────────────────────

    void testSharedPoolCpusFromString()
    {
        using basic::SharedThreadPoolConfig;

        // empty -> cpus cleared
        {
            SharedThreadPoolConfig cfg;
            cfg.cpus = {1, 2};
            cfg.cpusFromString("");
            check(cfg.cpus.empty(), "cpusFromString(''): clears cpus");
        }
        // "none" -> cleared
        {
            SharedThreadPoolConfig cfg;
            cfg.cpus = {1, 2};
            cfg.cpusFromString("none");
            check(cfg.cpus.empty(), "cpusFromString('none'): clears cpus");
        }
        {
            SharedThreadPoolConfig cfg;
            cfg.cpusFromString("NONE");
            check(cfg.cpus.empty(), "cpusFromString('NONE'): case-insensitive");
        }
        // single CPU
        {
            SharedThreadPoolConfig cfg;
            cfg.cpusFromString("3");
            check(eq(cfg.cpus, {3}), "cpusFromString('3')");
        }
        // comma list
        {
            SharedThreadPoolConfig cfg;
            cfg.cpusFromString("2,3");
            check(eq(cfg.cpus, {2, 3}), "cpusFromString('2,3')");
        }
        // range
        {
            SharedThreadPoolConfig cfg;
            cfg.cpusFromString("4-7");
            check(eq(cfg.cpus, {4, 5, 6, 7}), "cpusFromString('4-7')");
        }
        // "cpus:" prefix accepted
        {
            SharedThreadPoolConfig cfg;
            cfg.cpusFromString("cpus:2-5");
            check(eq(cfg.cpus, {2, 3, 4, 5}), "cpusFromString('cpus:2-5')");
        }
        // "CPUS:" case-insensitive
        {
            SharedThreadPoolConfig cfg;
            cfg.cpusFromString("CPUS:8,9");
            check(eq(cfg.cpus, {8, 9}), "cpusFromString('CPUS:8,9')");
        }
        // mixed comma + range
        {
            SharedThreadPoolConfig cfg;
            cfg.cpusFromString("2,3,8-11");
            check(eq(cfg.cpus, {2, 3, 8, 9, 10, 11}),
                "cpusFromString('2,3,8-11')");
        }
        // whitespace trimmed
        {
            SharedThreadPoolConfig cfg;
            cfg.cpusFromString("  4,5  ");
            check(eq(cfg.cpus, {4, 5}), "cpusFromString whitespace trim");
        }
        // chaining returns *this
        {
            SharedThreadPoolConfig cfg;
            cfg.cpusFromString("6").threadCount = 2;
            check(eq(cfg.cpus, {6}) && cfg.threadCount == 2,
                "cpusFromString chaining");
        }
        // bad input throws
        {
            check(throws([] {
                basic::SharedThreadPoolConfig cfg;
                cfg.cpusFromString("abc");
            }), "cpusFromString bad input throws");
        }
    }

    // Review regressions: the shared parser must reject trailing junk and
    // extra separators instead of silently keeping a numeric prefix.
    void testReviewRegressions()
    {
        for (char const *bad : {"cpus:2foo", "2-3-8", "2-3x", "-1"}) {
            check(throws([bad] {
                basic::SharedThreadPoolConfig cfg;
                cfg.cpusFromString(bad);
            }), std::string("review: cpusFromString '") + bad + "' throws");
        }
#ifdef TM_KIT_BASIC_HAS_HWLOC_CPU_ALLOCATOR
        for (char const *bad : {"exclude=8foo", "exclude=8-9-10", "exclude=-1"}) {
            check(throws([bad] { basic::HwlocCpuAllocatorConfig::fromString(bad); }),
                std::string("review: fromString '") + bad + "' throws");
        }
        {
            auto cfg = basic::HwlocCpuAllocatorConfig::fromString("exclude=8,9");
            check(eq(cfg.excludeCpus, {8, 9}), "review: fromString exclude=8,9 still accepted");
        }
#endif
    }

} // anonymous namespace

int main()
{
    testHwlocConfigFromString();
    testSharedPoolCpusFromString();
    testReviewRegressions();

    std::printf("\n%s (%d failure%s)\n",
        failures == 0 ? "PASS" : "FAIL",
        failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
