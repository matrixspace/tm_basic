#ifndef TM_KIT_BASIC_HWLOC_THREAD_CPU_ALLOCATOR_HPP_
#define TM_KIT_BASIC_HWLOC_THREAD_CPU_ALLOCATOR_HPP_
#include <tm_kit/infra/RealTimeThreadedExecutor.hpp>
#ifndef TM_KIT_BASIC_DISABLE_HWLOC_CPU_ALLOCATOR
#if __has_include(<hwloc.h>)
#include <hwloc.h>
#define TM_KIT_BASIC_HAS_HWLOC_CPU_ALLOCATOR 1
#endif
#endif

#ifdef TM_KIT_BASIC_HAS_HWLOC_CPU_ALLOCATOR
#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace dev
{
    namespace cd606
    {
        namespace tm
        {
            namespace basic
            {
                using infra::CpuAutoBindRequest;
                using infra::IThreadCpuAllocator;
                using infra::ThreadCpuAllocatorComponent;

                // Thrown when no CPU can be handed out (bad configuration, no free
                // core, wrong NUMA request, incompatible process constraints).
                class CpuAutoBindError : public std::runtime_error {
                public:
                    explicit CpuAutoBindError(std::string const &s) : std::runtime_error(s) {}
                };

                enum class SmtPolicy {
                    // A core is usable only when all of its PUs are available to the
                    // process. Under taskset masks that expose one thread per core,
                    // partial cores are rejected (no sharing with another tenant).
                    RequireFullCore,
                    // A core whose PUs are only partially available may still be used.
                    AllowPartialCore
                };

                struct HwlocCpuAllocatorConfig {
                    // OS CPU indices that must never be handed out. The whole physical
                    // core containing each one is avoided, so an SMT sibling of a CPU
                    // already taken by a third-party thread is not used either.
                    std::vector<int> excludeCpus;
                    // Skip the first core of every NUMA node: it carries most of the
                    // kernel's interrupt and housekeeping work (CPU 0's core on node
                    // 0, and the corresponding core on each other node).
                    bool avoidFirstCoreOfNumaNode = true;
                    SmtPolicy smt = SmtPolicy::RequireFullCore;
                };

                // Process-wide allocator for CpuAutoBind. Must be configured before
                // the first acquire(); later configure() calls throw std::logic_error
                // (same rule as SharedThreadPool::configure). Choose it in an
                // Environment with HwlocThreadCpuAllocatorComponent.
                class HwlocThreadCpuAllocator final : public IThreadCpuAllocator {
                public:
                    // Independent instances are for tests and custom components;
                    // normal code uses instance().
                    explicit HwlocThreadCpuAllocator(HwlocCpuAllocatorConfig cfg = {})
                        : cfg_(std::move(cfg))
                    {
                    }
                    HwlocThreadCpuAllocator(HwlocThreadCpuAllocator const &) = delete;
                    HwlocThreadCpuAllocator &operator=(HwlocThreadCpuAllocator const &) = delete;
                    ~HwlocThreadCpuAllocator() override
                    {
                        discardModel();
                    }

                    // Must be called before the first acquire(); afterwards, or while
                    // another thread is loading the topology, it throws.
                    static void configure(HwlocCpuAllocatorConfig const &cfg)
                    {
                        auto &self = instance();
                        std::lock_guard<std::mutex> _(self.mutex_);
                        if (self.loaded_) {
                            throw std::logic_error("HwlocThreadCpuAllocator::configure: the allocator is already in use");
                        }
                        self.cfg_ = cfg;
                    }

                    // Deliberately leaked: node threads may outlive static destruction.
                    static HwlocThreadCpuAllocator &instance()
                    {
                        static HwlocThreadCpuAllocator *a = new HwlocThreadCpuAllocator;
                        return *a;
                    }

                    int acquire(CpuAutoBindRequest const &req, std::string const &who) override
                    {
                        std::lock_guard<std::mutex> _(mutex_);
                        ensureLoaded();
                        auto candidates = candidateCores(req, who);
                        if (candidates.empty()) {
                            throw CpuAutoBindError(noCandidateMessage(req, who));
                        }
                        // Preference is expressed by candidateCores() order.
                        auto const &core = cores_[candidates.front()]; // index into cores_
                        int cpu = pickPu(core);
                        assignedCpus_[cpu] = who;
                        return cpu;
                    }

                    void reserve(std::vector<int> const &cpus, std::string const &who) override
                    {
                        // allowedString_ is read directly: allowedList() would lock mutex_
                        // again (non-recursive) and deadlock.
                        std::lock_guard<std::mutex> _(mutex_);
                        ensureLoaded();
                        for (int c : cpus) {
                            if (!pusAllowed_.count(c)) {
                                throw CpuAutoBindError("CpuAutoBind: CPU " + std::to_string(c) + " reserved by " + who + " is not a PU available to this process (" + allowedString_ + ")");
                            }
                            // Check the whole physical core: an automatic assignment on an
                            // SMT sibling also takes this core, so reserving this CPU
                            // would put two busy threads on one core.
                            for (int sibling : corePus(c)) {
                                auto it = assignedCpus_.find(sibling);
                                if (it != assignedCpus_.end()) {
                                    throw CpuAutoBindError("CpuAutoBind: CPU " + std::to_string(c) + " reserved by " + who
                                        + " shares a physical core with CPU " + std::to_string(sibling) + ", already automatically assigned to " + it->second);
                                }
                            }
                        }
                        for (int c : cpus) {
                            ++reservedCpus_[c];
                        }
                    }

                    void release(std::vector<int> const &cpus) noexcept override
                    {
                        std::lock_guard<std::mutex> _(mutex_);
                        for (int c : cpus) {
                            auto a = assignedCpus_.find(c);
                            if (a != assignedCpus_.end()) {
                                assignedCpus_.erase(a);
                                continue;
                            }
                            auto r = reservedCpus_.find(c);
                            if (r != reservedCpus_.end()) {
                                if (--(r->second) == 0) {
                                    reservedCpus_.erase(r);
                                }
                            }
                        }
                    }

                    // Diagnostics for tests and logs: the PUs this process may use.
                    std::string allowedList()
                    {
                        std::lock_guard<std::mutex> _(mutex_);
                        ensureLoaded();
                        return allowedString_;
                    }

                    // Diagnostics for tests: first allowed PU of every usable core
                    // (not excluded, SMT policy satisfied), whether free or not.
                    std::vector<std::vector<int>> usableCores()
                    {
                        std::lock_guard<std::mutex> _(mutex_);
                        ensureLoaded();
                        std::vector<std::vector<int>> r;
                        for (auto const &core : cores_) {
                            if (!core.excluded && (cfg_.smt == SmtPolicy::AllowPartialCore || core.full)) {
                                r.push_back(core.pus);
                            }
                        }
                        return r;
                    }

                private:
                    struct CoreInfo {
                        unsigned logical = 0;
                        std::vector<int> pus;   // PUs of this core that the process may use
                        int numaNode = -1;      // OS index, -1 when the machine has no NUMA objects
                        bool excluded = false;  // excludeCpus or first-core-of-node rule
                        bool firstOfNode = false;
                        bool full = true;       // every PU of the core is allowed
                    };

                    void ensureLoaded()
                    {
                        if (loaded_) {
                            return;
                        }
                        if (hwloc_topology_init(&topology_) != 0) {
                            topology_ = nullptr;
                            throw CpuAutoBindError("CpuAutoBind: hwloc_topology_init failed");
                        }
                        // Keep PUs the process may not use: otherwise a cgroup/taskset
                        // mask exposing one SMT thread per core makes the core look
                        // complete and RequireFullCore would accept it.
                        hwloc_topology_set_flags(topology_, HWLOC_TOPOLOGY_FLAG_INCLUDE_DISALLOWED);
                        if (hwloc_topology_load(topology_) != 0) {
                            hwloc_topology_destroy(topology_);
                            topology_ = nullptr;
                            throw CpuAutoBindError("CpuAutoBind: hwloc_topology_load failed");
                        }
                        try {
                            buildModel();
                        } catch (...) {
                            // Nothing may survive a failed load: a retry would append
                            // to the partially built model and double the core count.
                            discardModel();
                            throw;
                        }
                        loaded_ = true;
                    }

                    void discardModel() noexcept
                    {
                        if (cpuBind_) {
                            hwloc_bitmap_free(cpuBind_);
                            cpuBind_ = nullptr;
                        }
                        if (topology_) {
                            hwloc_topology_destroy(topology_);
                            topology_ = nullptr;
                        }
                        allowedString_.clear();
                        pusAllowed_.clear();
                        cores_.clear();
                    }

                    void buildModel()
                    {
                        // PUs the process may actually use: cgroup cpuset, taskset,
                        // numactl --physcpubind and --cpunodebind all narrow this.
                        cpuBind_ = hwloc_bitmap_alloc();
                        if (!cpuBind_ || hwloc_get_cpubind(topology_, cpuBind_, HWLOC_CPUBIND_PROCESS) != 0) {
                            throw CpuAutoBindError("CpuAutoBind: cannot read the process CPU binding");
                        }
                        hwloc_const_bitmap_t allowed = hwloc_topology_get_allowed_cpuset(topology_);
                        if (allowed) {
                            hwloc_bitmap_and(cpuBind_, cpuBind_, allowed);
                        }
                        if (hwloc_bitmap_iszero(cpuBind_)) {
                            throw CpuAutoBindError("CpuAutoBind: the process is allowed to use no CPU");
                        }
                        for (int c = hwloc_bitmap_first(cpuBind_); c >= 0; c = hwloc_bitmap_next(cpuBind_, c)) {
                            pusAllowed_.insert(c);
                        }
                        allowedString_ = bitmapList(cpuBind_);

                        // NUMA nodes: OS index -> allowed PUs in it.
                        std::map<int, hwloc_bitmap_t> nodeCpus;
                        int const nodeCount = hwloc_get_nbobjs_by_type(topology_, HWLOC_OBJ_NUMANODE);
                        for (int i = 0; i < nodeCount; ++i) {
                            hwloc_obj_t n = hwloc_get_obj_by_type(topology_, HWLOC_OBJ_NUMANODE, i);
                            if (!n || !n->cpuset) {
                                continue;
                            }
                            hwloc_bitmap_t s = hwloc_bitmap_alloc();
                            hwloc_bitmap_and(s, n->cpuset, cpuBind_);
                            if (!hwloc_bitmap_iszero(s)) {
                                nodeCpus[(int)n->os_index] = s;
                            }
                        }
                        // First PU of every NUMA node in the full topology, whether
                        // or not the process may use it: the core holding it is the
                        // one taking the node's interrupts and housekeeping.
                        std::set<int> nodeFirstPus;
                        for (int i = 0; i < nodeCount; ++i) {
                            hwloc_obj_t n = hwloc_get_obj_by_type(topology_, HWLOC_OBJ_NUMANODE, i);
                            if (n && n->cpuset && !hwloc_bitmap_iszero(n->cpuset)) {
                                nodeFirstPus.insert(hwloc_bitmap_first(n->cpuset));
                            }
                        }
                        if (nodeFirstPus.empty()) {
                            nodeFirstPus.insert(hwloc_bitmap_first(hwloc_topology_get_topology_cpuset(topology_)));
                        }

                        int const coreCount = hwloc_get_nbobjs_by_type(topology_, HWLOC_OBJ_CORE);
                        for (int i = 0; i < coreCount; ++i) {
                            hwloc_obj_t core = hwloc_get_obj_by_type(topology_, HWLOC_OBJ_CORE, i);
                            if (!core || !core->cpuset) {
                                continue;
                            }
                            CoreInfo info;
                            info.logical = core->logical_index;
                            int total = 0;
                            for (int c = hwloc_bitmap_first(core->cpuset); c >= 0; c = hwloc_bitmap_next(core->cpuset, c)) {
                                ++total;
                                if (pusAllowed_.count(c)) {
                                    info.pus.push_back(c);
                                }
                            }
                            info.full = ((int)info.pus.size() == total);
                            if (cfg_.avoidFirstCoreOfNumaNode) {
                                for (int f : nodeFirstPus) {
                                    if (hwloc_bitmap_isset(core->cpuset, f)) {
                                        info.excluded = true;
                                        info.firstOfNode = true;
                                    }
                                }
                            }
                            if (info.pus.empty()) {
                                continue; // entirely outside the process mask
                            }
                            info.numaNode = nodeOfPu(info.pus.front(), nodeCpus);
                            cores_.push_back(std::move(info));
                        }
                        for (auto &kv : nodeCpus) {
                            hwloc_bitmap_free(kv.second);
                        }
                        if (cores_.empty()) {
                            throw CpuAutoBindError("CpuAutoBind: no physical core is available to this process (" + allowedString_ + ")");
                        }

                        // Configuration: exclude whole cores, never single PUs.
                        for (int c : cfg_.excludeCpus) {
                            if (!pusAllowed_.count(c)) {
                                throw CpuAutoBindError("CpuAutoBind: excludeCpus contains CPU " + std::to_string(c) + ", which is not available to this process (" + allowedString_ + ")");
                            }
                            bool found = false;
                            for (auto &core : cores_) {
                                if (std::find(core.pus.begin(), core.pus.end(), c) != core.pus.end()) {
                                    core.excluded = true;
                                    found = true;
                                }
                            }
                            if (!found) {
                                throw CpuAutoBindError("CpuAutoBind: excludeCpus contains CPU " + std::to_string(c) + ", which has no physical core in this topology");
                            }
                        }
                    }

                    static int nodeOfPu(int pu, std::map<int, hwloc_bitmap_t> const &nodeCpus)
                    {
                        for (auto const &kv : nodeCpus) {
                            if (hwloc_bitmap_isset(kv.second, pu)) {
                                return kv.first;
                            }
                        }
                        return -1;
                    }

                    static std::string bitmapList(hwloc_const_bitmap_t b)
                    {
                        char *s = nullptr;
                        if (hwloc_bitmap_list_asprintf(&s, b) < 0 || !s) {
                            return "<unprintable>";
                        }
                        std::string r(s);
                        std::free(s);
                        return r;
                    }

                    // All PUs of the physical core holding cpu (cpu itself if none).
                    std::vector<int> corePus(int cpu) const
                    {
                        for (auto const &core : cores_) {
                            if (std::find(core.pus.begin(), core.pus.end(), cpu) != core.pus.end()) {
                                return core.pus;
                            }
                        }
                        return {cpu};
                    }

                    bool coreAvailable(CoreInfo const &core) const
                    {
                        if (core.excluded) {
                            return false;
                        }
                        if (cfg_.smt == SmtPolicy::RequireFullCore && !core.full) {
                            return false;
                        }
                        for (int c : core.pus) {
                            if (assignedCpus_.count(c) || reservedCpus_.count(c)) {
                                return false;
                            }
                        }
                        return true;
                    }

                    int freeCoreCountInNode(int node) const
                    {
                        int n = 0;
                        for (auto const &core : cores_) {
                            if (core.numaNode == node && coreAvailable(core)) {
                                ++n;
                            }
                        }
                        return n;
                    }

                    // Preferred NUMA nodes as expressed by the process' memory
                    // binding; empty unless the caller asked for a strict policy.
                    std::vector<int> preferredNodes()
                    {
                        std::vector<int> r;
                        auto *support = hwloc_topology_get_support(topology_);
                        if (!support || !support->membind->get_thisthread_membind) {
                            return r;
                        }
                        hwloc_bitmap_t set = hwloc_bitmap_alloc();
                        hwloc_membind_policy_t policy = HWLOC_MEMBIND_DEFAULT;
                        int const rc = hwloc_get_membind(topology_, set, &policy, HWLOC_MEMBIND_THREAD | HWLOC_MEMBIND_BYNODESET);
                        if (rc == 0 && policy == HWLOC_MEMBIND_BIND) {
                            for (int n = hwloc_bitmap_first(set); n >= 0; n = hwloc_bitmap_next(set, n)) {
                                r.push_back(n);
                            }
                        }
                        hwloc_bitmap_free(set);
                        return r;
                    }

                    // Indices into cores_, in preference order. Not the hwloc logical
                    // index: cores outside the process mask are skipped, so the two
                    // differ whenever the mask is narrower than the machine.
                    std::vector<std::size_t> candidateCores(CpuAutoBindRequest const &req, std::string const &who)
                    {
                        std::vector<int> wantedNodes;
                        if (req.numaNode) {
                            bool known = false;
                            for (auto const &core : cores_) {
                                if (core.numaNode == *req.numaNode) {
                                    known = true;
                                    break;
                                }
                            }
                            if (!known) {
                                throw CpuAutoBindError("CpuAutoBind for " + who + ": NUMA node " + std::to_string(*req.numaNode) + " has no CPU available to this process (" + allowedString_ + ")");
                            }
                            wantedNodes.push_back(*req.numaNode);
                        } else {
                            // Concentrate: stay on the node holding earlier automatic
                            // assignments while it has free cores, then fall back to the
                            // memory-binding preference and finally the roomiest node.
                            std::map<int, int> assignedPerNode;
                            for (auto const &kv : assignedCpus_) {
                                int node = -1;
                                for (auto const &core : cores_) {
                                    if (std::find(core.pus.begin(), core.pus.end(), kv.first) != core.pus.end()) {
                                        node = core.numaNode;
                                        break;
                                    }
                                }
                                if (node >= 0) {
                                    ++assignedPerNode[node];
                                }
                            }
                            for (auto const &kv : assignedPerNode) {
                                if (freeCoreCountInNode(kv.first) > 0) {
                                    wantedNodes.push_back(kv.first);
                                }
                            }
                            for (int n : preferredNodes()) {
                                if (freeCoreCountInNode(n) > 0 && std::find(wantedNodes.begin(), wantedNodes.end(), n) == wantedNodes.end()) {
                                    wantedNodes.push_back(n);
                                }
                            }
                            std::set<int> allNodes;
                            for (auto const &core : cores_) {
                                allNodes.insert(core.numaNode);
                            }
                            std::vector<int> remaining(allNodes.begin(), allNodes.end());
                            std::stable_sort(remaining.begin(), remaining.end(), [this](int a, int b) {
                                return freeCoreCountInNode(a) > freeCoreCountInNode(b);
                            });
                            for (int n : remaining) {
                                if (std::find(wantedNodes.begin(), wantedNodes.end(), n) == wantedNodes.end()) {
                                    wantedNodes.push_back(n);
                                }
                            }
                        }
                        std::vector<std::size_t> r;
                        for (int node : wantedNodes) {
                            for (std::size_t i = 0; i < cores_.size(); ++i) {
                                if (cores_[i].numaNode == node && coreAvailable(cores_[i])) {
                                    r.push_back(i);
                                }
                            }
                        }
                        return r;
                    }

                    int pickPu(CoreInfo const &core) const
                    {
                        // Inside a core the first available PU is as good as any other.
                        return core.pus.front();
                    }

                    std::string noCandidateMessage(CpuAutoBindRequest const &req, std::string const &who) const
                    {
                        std::ostringstream oss;
                        oss << "CpuAutoBind for " << who << ": no free physical core";
                        if (req.numaNode) {
                            oss << " on NUMA node " << *req.numaNode;
                        }
                        oss << " (available PUs " << allowedString_;
                        if (req.numaNode) {
                            oss << ", node has " << freeCoreCountInNode(*req.numaNode) << " free cores";
                        }
                        std::vector<int> excluded;
                        for (auto const &core : cores_) {
                            if (core.excluded) {
                                excluded.push_back(core.pus.front());
                            }
                        }
                        if (!excluded.empty()) {
                            oss << ", excluded cores " << join(excluded);
                        }
                        if (!cfg_.excludeCpus.empty()) {
                            oss << ", excludeCpus " << join(cfg_.excludeCpus);
                        }
                        std::vector<int> held;
                        for (auto const &kv : assignedCpus_) {
                            held.push_back(kv.first);
                        }
                        if (!held.empty()) {
                            oss << ", already assigned " << join(held);
                        }
                        if (!reservedCpus_.empty()) {
                            std::vector<int> res;
                            for (auto const &kv : reservedCpus_) {
                                res.push_back(kv.first);
                            }
                            oss << ", reserved " << join(res);
                        }
                        if (cfg_.smt == SmtPolicy::RequireFullCore) {
                            oss << ", SmtPolicy::RequireFullCore rejects partially available cores";
                        }
                        oss << ")";
                        return oss.str();
                    }

                    static std::string join(std::vector<int> const &v)
                    {
                        std::ostringstream oss;
                        oss << '[';
                        for (std::size_t i = 0; i < v.size(); ++i) {
                            oss << (i ? "," : "") << v[i];
                        }
                        oss << ']';
                        return oss.str();
                    }

                    std::mutex mutex_;
                    HwlocCpuAllocatorConfig cfg_;
                    bool loaded_ = false;
                    hwloc_topology_t topology_ = nullptr;
                    hwloc_bitmap_t cpuBind_ = nullptr;
                    std::string allowedString_;
                    std::set<int> pusAllowed_;
                    std::vector<CoreInfo> cores_;
                    std::map<int, std::string> assignedCpus_; // CPU -> who
                    std::map<int, unsigned> reservedCpus_;    // CPU -> explicit reservations
                };

                namespace hwloc_thread_cpu_allocator_detail
                {
                    struct Access {
                        static IThreadCpuAllocator &instance()
                        {
                            return HwlocThreadCpuAllocator::instance();
                        }
                    };
                } // namespace hwloc_thread_cpu_allocator_detail

                // Mix into an Environment to enable ThreadedNodeOptions::CpuAutoBind().
                struct HwlocThreadCpuAllocatorComponent
                    : ThreadCpuAllocatorComponent<hwloc_thread_cpu_allocator_detail::Access> {
                };
            } // namespace basic
        } // namespace tm
    } // namespace cd606
} // namespace dev
#endif // TM_KIT_BASIC_HAS_HWLOC_CPU_ALLOCATOR

#endif // TM_KIT_BASIC_HWLOC_THREAD_CPU_ALLOCATOR_HPP_
