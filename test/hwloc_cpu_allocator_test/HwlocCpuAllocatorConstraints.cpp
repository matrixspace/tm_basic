#include <tm_kit/basic/HwlocThreadCpuAllocator.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Checks what HwlocThreadCpuAllocator derives from the process constraints it is
// started under (taskset / numactl). Driven by run_constraints.sh, which knows
// the expected layout; this binary only inspects and reports.
//
//   full-cores  every acquisition must sit on a core whose PUs are all in the
//               process mask; reports the number of usable cores
//   partial     SmtPolicy::RequireFullCore must find no usable core; exits 0
//               only when allocate() throws (the expected outcome)
//   node <n>    every acquisition must be on NUMA node <n>
//   count       prints the usable core count and succeeds
#ifdef TM_KIT_BASIC_HAS_HWLOC_CPU_ALLOCATOR

using namespace dev::cd606::tm;

namespace
{
    using basic::HwlocThreadCpuAllocator;
    using Cfg = basic::HwlocCpuAllocatorConfig;

    int fail(std::string const &s)
    {
        std::cerr << "FAIL: " << s << '\n';
        return 1;
    }

    std::set<int> siblingsOf(int cpu)
    {
        std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list");
        std::set<int> r;
        std::string part;
        while (std::getline(in, part, ',')) {
            auto dash = part.find('-');
            if (dash == std::string::npos) {
                r.insert(std::stoi(part));
            } else {
                int lo = std::stoi(part.substr(0, dash)), hi = std::stoi(part.substr(dash + 1));
                for (int c = lo; c <= hi; ++c) {
                    r.insert(c);
                }
            }
        }
        return r;
    }

    std::set<int> nodeCpus(int node)
    {
        std::ifstream in("/sys/devices/system/node/node" + std::to_string(node) + "/cpulist");
        std::set<int> r;
        std::string part;
        while (std::getline(in, part, ',')) {
            auto dash = part.find('-');
            if (dash == std::string::npos) {
                r.insert(std::stoi(part));
            } else {
                int lo = std::stoi(part.substr(0, dash)), hi = std::stoi(part.substr(dash + 1));
                for (int c = lo; c <= hi; ++c) {
                    r.insert(c);
                }
            }
        }
        return r;
    }

    bool coreInsideMask(int cpu, std::string const &allowedList)
    {
        // allowedList is hwloc's comma/range list; expand it for the comparison.
        std::set<int> allowed;
        std::string part;
        std::stringstream ss(allowedList);
        while (std::getline(ss, part, ',')) {
            auto dash = part.find('-');
            if (dash == std::string::npos) {
                allowed.insert(std::stoi(part));
            } else {
                int lo = std::stoi(part.substr(0, dash)), hi = std::stoi(part.substr(dash + 1));
                for (int c = lo; c <= hi; ++c) {
                    allowed.insert(c);
                }
            }
        }
        auto sib = siblingsOf(cpu);
        return !sib.empty() && std::includes(allowed.begin(), allowed.end(), sib.begin(), sib.end());
    }

    int runFullCores(bool requireAll)
    {
        HwlocThreadCpuAllocator a{Cfg{}};
        auto allowed = a.allowedList();
        auto usable = a.usableCores();
        std::cout << "allowed=" << allowed << " usable_cores=" << usable.size() << '\n';
        if (requireAll && usable.empty()) {
            return fail("no usable core in a whole-cores mask");
        }
        std::set<std::set<int>> seen;
        std::size_t n = 0;
        while (true) {
            int cpu = -1;
            try {
                cpu = a.acquire(infra::CpuAutoBindRequest{}, "constraints");
            } catch (std::exception const &) {
                break;
            }
            ++n;
            if (requireAll && !coreInsideMask(cpu, allowed)) {
                return fail("CPU " + std::to_string(cpu) + " comes from a partially available core");
            }
            if (!seen.insert(siblingsOf(cpu)).second) {
                return fail("core of CPU " + std::to_string(cpu) + " handed out twice");
            }
        }
        if (n != usable.size()) {
            return fail("acquired " + std::to_string(n) + " of " + std::to_string(usable.size()) + " usable cores");
        }
        std::cout << "acquired=" << n << '\n';
        return 0;
    }

    int runPartial()
    {
        // Under a mask with one PU per core, whole cores are never available and
        // RequireFullCore must report it instead of quietly sharing cores.
        HwlocThreadCpuAllocator a{Cfg{}};
        auto usable = a.usableCores();
        std::cout << "allowed=" << a.allowedList() << " usable_cores=" << usable.size() << '\n';
        if (!usable.empty()) {
            return fail("RequireFullCore accepted partially available cores");
        }
        try {
            a.acquire(infra::CpuAutoBindRequest{}, "partial");
        } catch (basic::CpuAutoBindError const &e) {
            std::cout << "expected error: " << e.what() << '\n';
            return 0;
        }
        return fail("CpuAutoBind did not throw without a full core");
    }

    int runNode(int node)
    {
        HwlocThreadCpuAllocator a{Cfg{}};
        auto allowed = a.allowedList();
        auto cpus = nodeCpus(node);
        std::cout << "allowed=" << allowed << " node=" << node << '\n';
        int n = 0;
        while (true) {
            int cpu = -1;
            try {
                cpu = a.acquire(infra::CpuAutoBindRequest{node}, "node");
            } catch (std::exception const &) {
                break;
            }
            ++n;
            if (!cpus.count(cpu)) {
                return fail("CPU " + std::to_string(cpu) + " is not on NUMA node " + std::to_string(node));
            }
        }
        if (n == 0) {
            return fail("no CPU handed out on NUMA node " + std::to_string(node));
        }
        std::cout << "acquired=" << n << '\n';
        return 0;
    }
} // namespace

int main(int argc, char **argv)
{
    std::string mode = argc > 1 ? argv[1] : "count";
    basic::HwlocThreadCpuAllocator::configure(Cfg{});
    if (mode == "full-cores") {
        return runFullCores(true);
    }
    if (mode == "partial") {
        return runPartial();
    }
    if (mode == "node" && argc > 2) {
        return runNode(std::stoi(argv[2]));
    }
    if (mode == "count") {
        HwlocThreadCpuAllocator a{Cfg{}};
        std::cout << "allowed=" << a.allowedList() << " usable_cores=" << a.usableCores().size() << '\n';
        return 0;
    }
    std::cerr << "usage: " << argv[0] << " full-cores|partial|node <n>|count\n";
    return 2;
}

#else
int main()
{
    std::cout << "hwloc CPU allocator not built\n";
    return 0;
}
#endif
