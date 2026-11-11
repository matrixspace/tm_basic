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
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef __linux__
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#endif

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
                    // Cross-process mode only: directory holding one flock file per
                    // physical core. Every process meant to negotiate must use the
                    // same directory (tmpfs; network filesystems are not supported).
                    std::string crossProcessLockDir = "/dev/shm/tm_kit_cpu_locks";
                };

                // Local: cores are shared out among the threads of this process
                // only (default). CrossProcess: every core is also locked with a
                // per-core flock file, so tm_kit processes on the same machine never
                // pick the same core; a lock vanishes when its process exits.
                enum class CpuAllocatorScope {
                    Local,
                    CrossProcess
                };

                // Process-wide allocator for CpuAutoBind. Must be configured before
                // the first acquire(); later configure() calls throw std::logic_error
                // (same rule as SharedThreadPool::configure). Choose it in an
                // Environment with HwlocThreadCpuAllocatorComponent (Local scope) or
                // HwlocCrossProcessCpuAllocatorComponent (CrossProcess scope). Both
                // components share this instance and its configuration, so
                // excludeCpus, avoidFirstCoreOfNumaNode and smt apply in either
                // scope; one process cannot mix the two components.
                class HwlocThreadCpuAllocator final : public IThreadCpuAllocator {
                public:
                    // Independent instances are for tests and custom components;
                    // normal code uses instance(). Their scope is fixed here.
                    explicit HwlocThreadCpuAllocator(HwlocCpuAllocatorConfig cfg = {}, CpuAllocatorScope scope = CpuAllocatorScope::Local)
                        : cfg_(std::move(cfg))
                        , scope_(scope)
                        , scopeFixed_(true)
                    {
                    }
                    HwlocThreadCpuAllocator(HwlocThreadCpuAllocator const &) = delete;
                    HwlocThreadCpuAllocator &operator=(HwlocThreadCpuAllocator const &) = delete;
                    ~HwlocThreadCpuAllocator() override
                    {
                        closeAllLocks();
                        discardModel();
                    }

                    // Must be called before the first acquire(); afterwards, or while
                    // another thread is loading the topology, it throws. Applies to
                    // both components (Local and CrossProcess).
                    static void configure(HwlocCpuAllocatorConfig const &cfg)
                    {
                        auto &self = instance();
                        std::lock_guard<std::mutex> _(self.mutex_);
                        if (self.loaded_) {
                            throw std::logic_error("HwlocThreadCpuAllocator::configure: the allocator is already in use");
                        }
                        self.cfg_ = cfg;
                        if (self.scopeFixed_ && self.scope_ == CpuAllocatorScope::CrossProcess) {
                            self.publishNowLocked();
                        }
                    }
                    // Same, also committing the process to scope. For CrossProcess,
                    // excludeCpus is published to other processes before this
                    // returns (not at the first binding); a conflict throws
                    // CpuAutoBindError and leaves the allocator unconfigured.
                    static void configure(HwlocCpuAllocatorConfig const &cfg, CpuAllocatorScope scope)
                    {
                        auto &self = instance();
                        std::lock_guard<std::mutex> _(self.mutex_);
                        if (self.loaded_) {
                            throw std::logic_error("HwlocThreadCpuAllocator::configure: the allocator is already in use");
                        }
                        if (self.scopeFixed_ && self.scope_ != scope) {
                            throw std::logic_error("HwlocThreadCpuAllocator::configure: this process already uses the other scope");
                        }
                        bool const wasFixed = self.scopeFixed_;
                        auto const oldCfg = self.cfg_;
                        self.cfg_ = cfg;
                        self.scope_ = scope;
                        self.scopeFixed_ = true;
                        if (scope == CpuAllocatorScope::CrossProcess) {
                            try {
                                self.publishNowLocked();
                            } catch (...) {
                                self.cfg_ = oldCfg;
                                self.scopeFixed_ = wasFixed;
                                throw;
                            }
                        }
                    }

                    // Deliberately leaked: node threads may outlive static destruction.
                    static HwlocThreadCpuAllocator &instance()
                    {
                        static HwlocThreadCpuAllocator *a = new HwlocThreadCpuAllocator(SingletonTag{});
                        return *a;
                    }

                    // The singleton, committed to scope s. Throws std::logic_error when
                    // the process already uses the other scope.
                    static HwlocThreadCpuAllocator &instanceFor(CpuAllocatorScope s)
                    {
                        auto &a = instance();
                        a.requireScope(s);
                        return a;
                    }

                    CpuAllocatorScope scope()
                    {
                        std::lock_guard<std::mutex> _(mutex_);
                        return scope_;
                    }

                    int acquire(CpuAutoBindRequest const &req, std::string const &who) override
                    {
                        std::lock_guard<std::mutex> _(mutex_);
                        scopeFixed_ = true;
                        ensureLoaded();
                        if (scope_ == CpuAllocatorScope::CrossProcess) {
                            publishExclusions();
                            refreshForeignBusy();
                        }
                        auto candidates = candidateCores(req, who);
                        if (!foreignBusy_.empty()) {
                            // The snapshot only orders the search: cores it marks as
                            // held elsewhere are still tried last, the try-lock decides.
                            auto hint = std::move(foreignBusy_);
                            foreignBusy_.clear();
                            for (std::size_t i : candidateCores(req, who)) {
                                if (std::find(candidates.begin(), candidates.end(), i) == candidates.end()) {
                                    candidates.push_back(i);
                                }
                            }
                            foreignBusy_ = std::move(hint);
                        }
                        if (candidates.empty()) {
                            throw CpuAutoBindError(noCandidateMessage(req, who, {}));
                        }
                        // Preference is expressed by candidateCores() order. In the
                        // CrossProcess scope, cores locked by other processes are
                        // skipped; the flock try-lock is atomic across processes.
                        std::vector<std::string> foreign;
                        for (std::size_t idx : candidates) {
                            auto const &core = cores_[idx]; // index into cores_
                            if (scope_ == CpuAllocatorScope::CrossProcess) {
                                std::string holder;
                                if (!tryLockCore(core.lockKey, who, holder)) {
                                    foreign.push_back("core of CPU " + std::to_string(core.lockKey) + " (" + holder + ")");
                                    continue;
                                }
                            }
                            int cpu = pickPu(core);
                            assignedCpus_[cpu] = who;
                            return cpu;
                        }
                        throw CpuAutoBindError(noCandidateMessage(req, who, foreign));
                    }

                    void reserve(std::vector<int> const &cpus, std::string const &who) override
                    {
                        reserveReplacing(cpus, {}, who);
                    }

                    // Explicit CPUs are exclusive per physical core, inside this
                    // process exactly as across processes: a core already held by
                    // another explicit reservation or an automatic assignment is a
                    // conflict, unless the CPU is in `previous` (the caller's own,
                    // about to be released: a node reconfigured onto the same core).
                    // CPUs of one call may share a core: they belong to one thread.
                    void reserveReplacing(std::vector<int> const &cpus, std::vector<int> const &previous, std::string const &who) override
                    {
                        // allowedString_ is read directly: allowedList() would lock mutex_
                        // again (non-recursive) and deadlock.
                        std::lock_guard<std::mutex> _(mutex_);
                        scopeFixed_ = true;
                        ensureLoaded();
                        for (int c : cpus) {
                            if (!pusAllowed_.count(c)) {
                                throw CpuAutoBindError("CpuAutoBind: CPU " + std::to_string(c) + " reserved by " + who + " is not a PU available to this process (" + allowedString_ + ")");
                            }
                            // excludeCpus forbids the whole core to every busy thread,
                            // explicit ones included, in both scopes (in CrossProcess
                            // the published lock also keeps other processes away).
                            for (auto const &core : cores_) {
                                if (core.userExcluded && std::find(core.pus.begin(), core.pus.end(), c) != core.pus.end()) {
                                    throw CpuAutoBindError("CpuAutoBind: CPU " + std::to_string(c) + " reserved by " + who
                                        + " is on a physical core excluded by excludeCpus " + join(cfg_.excludeCpus));
                                }
                            }
                            // Check the whole physical core: an automatic assignment on an
                            // SMT sibling also takes this core, so reserving this CPU
                            // would put two busy threads on one core.
                            for (int sibling : corePus(c)) {
                                if (std::find(previous.begin(), previous.end(), sibling) != previous.end()) {
                                    continue; // the caller's own core
                                }
                                auto it = assignedCpus_.find(sibling);
                                if (it != assignedCpus_.end()) {
                                    throw CpuAutoBindError("CpuAutoBind: CPU " + std::to_string(c) + " reserved by " + who
                                        + " shares a physical core with CPU " + std::to_string(sibling) + ", already automatically assigned to " + it->second);
                                }
                                auto r = reservedBy_.find(sibling);
                                if (r != reservedBy_.end()) {
                                    throw CpuAutoBindError("CpuAutoBind: CPU " + std::to_string(c) + " reserved by " + who
                                        + " shares a physical core with CPU " + std::to_string(sibling) + ", already explicitly reserved by " + r->second);
                                }
                            }
                        }
                        if (scope_ == CpuAllocatorScope::CrossProcess) {
                            publishExclusions();
                            // Exclusive across processes: every core touched must be
                            // lockable, otherwise roll back what this call took.
                            std::vector<int> taken;
                            for (int c : cpus) {
                                int const key = lockKeyOf(c);
                                std::string holder;
                                if (!tryLockCore(key, who, holder)) {
                                    for (int k : taken) {
                                        unlockCore(k);
                                    }
                                    throw CpuAutoBindError("CpuAutoBind: CPU " + std::to_string(c) + " reserved by " + who
                                        + " is on a physical core held by another process (" + holder + ", lock " + lockPath(key) + ")");
                                }
                                taken.push_back(key);
                            }
                        }
                        for (int c : cpus) {
                            if (reservedCpus_[c]++ == 0) {
                                reservedBy_[c] = who;
                            }
                        }
                    }

                    void release(std::vector<int> const &cpus) noexcept override
                    {
                        std::lock_guard<std::mutex> _(mutex_);
                        for (int c : cpus) {
                            auto a = assignedCpus_.find(c);
                            if (a != assignedCpus_.end()) {
                                assignedCpus_.erase(a);
                                unlockCore(lockKeyOf(c));
                                continue;
                            }
                            auto r = reservedCpus_.find(c);
                            if (r != reservedCpus_.end()) {
                                if (--(r->second) == 0) {
                                    reservedCpus_.erase(r);
                                    reservedBy_.erase(c);
                                }
                                unlockCore(lockKeyOf(c));
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
                    // Singleton only: its scope is chosen by the first component used.
                    struct SingletonTag {};
                    explicit HwlocThreadCpuAllocator(SingletonTag)
                        : cfg_()
                        , scope_(CpuAllocatorScope::Local)
                        , scopeFixed_(false)
                    {
                    }

                    void requireScope(CpuAllocatorScope s)
                    {
                        std::lock_guard<std::mutex> _(mutex_);
                        if (!scopeFixed_) {
                            scope_ = s;
                            scopeFixed_ = true;
                            if (s == CpuAllocatorScope::CrossProcess) {
                                try {
                                    publishNowLocked();
                                } catch (...) {
                                    scopeFixed_ = false;
                                    scope_ = CpuAllocatorScope::Local;
                                    throw;
                                }
                            }
                            return;
                        }
                        if (scope_ != s) {
                            throw std::logic_error(std::string("HwlocThreadCpuAllocator: this process already uses the ")
                                + (scope_ == CpuAllocatorScope::Local ? "Local (HwlocThreadCpuAllocatorComponent)" : "CrossProcess (HwlocCrossProcessCpuAllocatorComponent)")
                                + " scope; one process cannot mix the two components");
                        }
                    }

                    struct CoreInfo {
                        unsigned logical = 0;
                        std::vector<int> pus;   // PUs of this core that the process may use
                        int numaNode = -1;      // OS index, -1 when the machine has no NUMA objects
                        bool excluded = false;  // excludeCpus or first-core-of-node rule
                        bool firstOfNode = false;
                        bool userExcluded = false; // excludeCpus (published to other processes in CrossProcess scope)
                        bool full = true;       // every PU of the core is allowed
                        // Smallest OS PU of the full core cpuset (disallowed PUs
                        // included): the same key in every process whatever its
                        // taskset/cgroup mask, used to name the core's lock file.
                        int lockKey = -1;
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
                            info.lockKey = hwloc_bitmap_first(core->cpuset);
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
                                    core.userExcluded = true;
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
                        // CrossProcess: cores other processes hold count as taken for
                        // NUMA ordering and free-core counts (empty in Local scope).
                        return foreignBusy_.count(core.lockKey) == 0;
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

                    std::string noCandidateMessage(CpuAutoBindRequest const &req, std::string const &who, std::vector<std::string> const &foreign) const
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
                        if (!foreign.empty()) {
                            oss << ", held by other processes (lock dir " << cfg_.crossProcessLockDir << "): ";
                            for (std::size_t i = 0; i < foreign.size(); ++i) {
                                oss << (i ? "; " : "") << foreign[i];
                            }
                        }
                        if (cfg_.smt == SmtPolicy::RequireFullCore) {
                            oss << ", SmtPolicy::RequireFullCore rejects partially available cores";
                        }
                        oss << ")";
                        return oss.str();
                    }

                    // ---- cross-process locks (one flock file per physical core) ----

                    // Lock key of the core holding cpu: the lowest OS PU of the
                    // core's complete cpuset, identical for every process whatever
                    // its taskset / cgroup mask.
                    int lockKeyOf(int cpu) const
                    {
                        for (auto const &core : cores_) {
                            if (std::find(core.pus.begin(), core.pus.end(), cpu) != core.pus.end()) {
                                return core.lockKey;
                            }
                        }
                        return cpu;
                    }

                    std::string lockPath(int key) const
                    {
                        return cfg_.crossProcessLockDir + "/core-" + std::to_string(key) + ".lock";
                    }

#ifdef __linux__
                    // fds of held locks, readable without mutex_ from the fork child
                    // handler (mutex_ may be held by another thread at fork time).
                    static constexpr int kLockSlots = 4096;
                    static std::atomic<int> *lockSlots()
                    {
                        static std::atomic<int> *s = [] {
                            auto *p = new std::atomic<int>[kLockSlots]; // leaked: used at fork in any thread
                            for (int i = 0; i < kLockSlots; ++i) {
                                p[i].store(-1, std::memory_order_relaxed);
                            }
                            return p;
                        }();
                        return s;
                    }
                    // A child forked without exec closes its inherited copies, so it
                    // does not keep cores busy after the parent releases them. The
                    // parent's descriptors stay open: its locks are unaffected.
                    static void closeInheritedLocksInChild()
                    {
                        auto *s = lockSlots();
                        for (int i = 0; i < kLockSlots; ++i) {
                            int fd = s[i].exchange(-1, std::memory_order_relaxed);
                            if (fd >= 0) {
                                ::close(fd);
                            }
                        }
                    }
                    static void installForkHandler()
                    {
                        static std::once_flag once;
                        std::call_once(once, [] {
                            lockSlots();
                            if (::pthread_atfork(nullptr, nullptr, &closeInheritedLocksInChild) != 0) {
                                throw CpuAutoBindError("CpuAutoBind: pthread_atfork failed");
                            }
                        });
                    }
                    static bool addSlot(int fd)
                    {
                        auto *s = lockSlots();
                        for (int i = 0; i < kLockSlots; ++i) {
                            int expected = -1;
                            if (s[i].compare_exchange_strong(expected, fd, std::memory_order_relaxed)) {
                                return true;
                            }
                        }
                        return false;
                    }
                    static void removeSlot(int fd) noexcept
                    {
                        auto *s = lockSlots();
                        for (int i = 0; i < kLockSlots; ++i) {
                            int expected = fd;
                            if (s[i].compare_exchange_strong(expected, -1, std::memory_order_relaxed)) {
                                return;
                            }
                        }
                    }

                    void ensureLockDir()
                    {
                        auto const &dir = cfg_.crossProcessLockDir;
                        if (dir.empty()) {
                            throw CpuAutoBindError("CpuAutoBind: crossProcessLockDir is empty");
                        }
                        if (::mkdir(dir.c_str(), 01777) == 0) {
                            // mkdir is subject to umask; every user's process must be
                            // able to create lock files here (sticky, like /tmp).
                            ::chmod(dir.c_str(), 01777);
                        } else if (errno != EEXIST) {
                            throw CpuAutoBindError("CpuAutoBind: cannot create lock directory " + dir + ": " + std::strerror(errno));
                        }
                        struct stat st;
                        if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
                            throw CpuAutoBindError("CpuAutoBind: lock directory " + dir + " is not a directory");
                        }
                    }

                    static std::string readHolder(int fd)
                    {
                        char buf[256];
                        ssize_t n = ::pread(fd, buf, sizeof(buf) - 1, 0);
                        if (n <= 0) {
                            return "holder unknown";
                        }
                        std::string s(buf, (std::size_t)n);
                        while (!s.empty() && (s.back() == '\n' || s.back() == '\0')) {
                            s.pop_back();
                        }
                        return "last written by " + s;
                    }
#endif

                    // Takes (or, if this process already holds it, re-counts) the lock
                    // of core key. Returns false, with holder describing the other
                    // process, when another process holds it. Caller holds mutex_.
                    bool tryLockCore(int key, std::string const &who, std::string &holder)
                    {
                        auto it = heldLocks_.find(key);
#ifdef __linux__
                        if (it != heldLocks_.end() && it->second.ownerPid != ::getpid()) {
                            // Inherited from the parent across fork: the child's copy
                            // of the descriptor is closed, so this process holds nothing.
                            heldLocks_.erase(it);
                            it = heldLocks_.end();
                        }
#endif
                        if (it != heldLocks_.end()) {
                            ++it->second.refs;
                            return true;
                        }
#ifdef __linux__
                        installForkHandler();
                        ensureLockDir();
                        std::string const path = lockPath(key);
                        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0666);
                        if (fd < 0) {
                            throw CpuAutoBindError("CpuAutoBind: cannot open lock file " + path + ": " + std::strerror(errno));
                        }
                        ::fchmod(fd, 0666); // umask must not lock out other users; fails harmlessly if not owner
                        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
                            int const err = errno;
                            if (err == EWOULDBLOCK) {
                                holder = readHolder(fd);
                                ::close(fd);
                                return false;
                            }
                            ::close(fd);
                            throw CpuAutoBindError("CpuAutoBind: flock on " + path + " failed: " + std::strerror(err));
                        }
                        if (!addSlot(fd)) {
                            ::close(fd);
                            throw CpuAutoBindError("CpuAutoBind: too many cross-process core locks held");
                        }
                        // Diagnostics only: the lock, not this text, is the occupancy.
                        std::string const text = "pid " + std::to_string(::getpid()) + " " + who + "\n";
                        if (::ftruncate(fd, 0) == 0) {
                            ssize_t ignored = ::pwrite(fd, text.data(), text.size(), 0);
                            (void)ignored;
                        }
                        heldLocks_[key] = CoreLock{fd, 1u, ::getpid()};
                        return true;
#else
                        (void)who;
                        (void)holder;
                        throw CpuAutoBindError("CpuAutoBind: cross-process negotiation is only supported on Linux");
#endif
                    }

                    void unlockCore(int key) noexcept
                    {
                        auto it = heldLocks_.find(key);
                        if (it == heldLocks_.end()) {
                            return;
                        }
                        if (--it->second.refs > 0) {
                            return;
                        }
#ifdef __linux__
                        // In a forked child the descriptor was already closed (and its
                        // number may be reused): only forget the entry there.
                        if (it->second.ownerPid == ::getpid()) {
                            removeSlot(it->second.fd);
                            ::close(it->second.fd); // releases the flock
                        }
#endif
                        heldLocks_.erase(it);
                    }

                    // CrossProcess scope: excludeCpus is not only a local preference.
                    // The cores it names are locked by this process for its whole
                    // lifetime, so every other negotiating process skips them as if
                    // a busy thread lived there; the kernel drops the locks when this
                    // process exits. Done when the process commits to CrossProcess
                    // (configure with that scope, or first component access), else
                    // on first use. A core already held
                    // by another process cannot be vacated, so that is an error
                    // (rolled back, retried on the next call). Caller holds mutex_.
                    // Loads the model and publishes excludeCpus right away. On a
                    // conflict nothing survives: the model is discarded so that
                    // configure() may be called again. Caller holds mutex_.
                    void publishNowLocked()
                    {
                        if (cfg_.excludeCpus.empty()) {
                            return;
                        }
                        ensureLoaded();
                        try {
                            publishExclusions();
                        } catch (...) {
                            closeAllLocks();
                            discardModel();
                            loaded_ = false;
                            throw;
                        }
                    }

                    void publishExclusions()
                    {
#ifdef __linux__
                        // A child forked without exec has closed the inherited locks;
                        // while the ancestor lives its excludeCpus locks still keep
                        // every process away, so they are accepted in place of ours.
                        if (exclusionsPid_ != 0 && exclusionsPid_ != ::getpid()) {
                            acceptedExclusionHolders_.insert(exclusionsPid_);
                            exclusionsPublished_ = false;
                            exclusionLocks_.clear();
                            exclusionsPid_ = 0;
                        }
#endif
                        if (exclusionsPublished_) {
                            return;
                        }
                        std::vector<int> taken;
                        bool pending = false;
                        for (auto const &core : cores_) {
                            if (!core.userExcluded
                                || std::find(exclusionLocks_.begin(), exclusionLocks_.end(), core.lockKey) != exclusionLocks_.end()) {
                                continue;
                            }
                            std::string holder;
                            if (tryLockCore(core.lockKey, "excludeCpus", holder)) {
                                taken.push_back(core.lockKey);
                                continue;
                            }
                            if (heldByAcceptedAncestor(holder)) {
                                pending = true; // retried on later calls, taken once the ancestor exits
                                continue;
                            }
                            for (int k : taken) {
                                unlockCore(k);
                            }
                            throw CpuAutoBindError("CpuAutoBind: excludeCpus core of CPU " + std::to_string(core.lockKey)
                                + " cannot be published to other processes: it is already held by another process ("
                                + holder + ", lock " + lockPath(core.lockKey) + ")");
                        }
                        exclusionLocks_.insert(exclusionLocks_.end(), taken.begin(), taken.end());
                        exclusionsPublished_ = !pending;
#ifdef __linux__
                        exclusionsPid_ = ::getpid();
#endif
                    }

                    // holder text is "last written by pid <n> excludeCpus".
                    bool heldByAcceptedAncestor(std::string const &holder) const
                    {
                        if (acceptedExclusionHolders_.empty() || holder.find(" excludeCpus") == std::string::npos) {
                            return false;
                        }
                        auto p = holder.find("pid ");
                        if (p == std::string::npos) {
                            return false;
                        }
                        try {
                            return acceptedExclusionHolders_.count(std::stoi(holder.substr(p + 4))) > 0;
                        } catch (...) {
                            return false;
                        }
                    }

                    // CrossProcess scope: snapshot of cores locked by other processes,
                    // read from /proc/locks without touching the locks (a probing
                    // try-lock would make a concurrent process skip a free core).
                    // Only a hint for NUMA ordering and diagnostics: the try-lock in
                    // acquire()/reserve() remains the decision. Caller holds mutex_.
                    void refreshForeignBusy()
                    {
                        foreignBusy_.clear();
#ifdef __linux__
                        std::map<std::pair<std::pair<unsigned, unsigned>, unsigned long>, int> files;
                        for (auto const &core : cores_) {
                            auto h = heldLocks_.find(core.lockKey);
                            if (h != heldLocks_.end() && h->second.ownerPid == ::getpid()) {
                                continue;
                            }
                            struct stat st;
                            if (::stat(lockPath(core.lockKey).c_str(), &st) == 0) {
                                files[{{(unsigned)major(st.st_dev), (unsigned)minor(st.st_dev)}, (unsigned long)st.st_ino}] = core.lockKey;
                            }
                        }
                        if (files.empty()) {
                            return;
                        }
                        std::ifstream in("/proc/locks");
                        std::string line;
                        while (std::getline(in, line)) {
                            if (line.find("->") != std::string::npos) {
                                continue; // a blocked waiter, not a holder
                            }
                            std::istringstream ss(line);
                            std::string id, kind, mode, access, pidText, devIno;
                            if (!(ss >> id >> kind >> mode >> access >> pidText >> devIno) || kind != "FLOCK") {
                                continue;
                            }
                            unsigned maj = 0, min = 0;
                            unsigned long ino = 0;
                            if (std::sscanf(devIno.c_str(), "%x:%x:%lu", &maj, &min, &ino) != 3) {
                                continue;
                            }
                            auto f = files.find({{maj, min}, ino});
                            if (f != files.end() && pidText != std::to_string(::getpid())) {
                                foreignBusy_[f->second] = "pid " + pidText;
                            }
                        }
#endif
                    }

                    void closeAllLocks() noexcept
                    {
#ifdef __linux__
                        for (auto &kv : heldLocks_) {
                            if (kv.second.ownerPid == ::getpid()) {
                                removeSlot(kv.second.fd);
                                ::close(kv.second.fd);
                            }
                        }
#endif
                        heldLocks_.clear();
                        exclusionLocks_.clear();
                        exclusionsPublished_ = false;
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
                    std::map<int, std::string> reservedBy_;   // CPU -> first explicit holder
                    std::map<int, std::string> foreignBusy_;  // lock key -> "pid n", CrossProcess snapshot
                    std::set<int> acceptedExclusionHolders_;  // ancestors whose excludeCpus locks cover ours
                    struct CoreLock {
                        int fd = -1;
                        unsigned refs = 0;
                        int ownerPid = 0; // process that opened fd (differs after fork)
                    };
                    std::map<int, CoreLock> heldLocks_;        // lock key -> flock file
                    bool exclusionsPublished_ = false;         // excludeCpus cores locked (CrossProcess)
                    std::vector<int> exclusionLocks_;          // their lock keys, held until destruction
                    int exclusionsPid_ = 0;                    // process that published them
                    CpuAllocatorScope scope_ = CpuAllocatorScope::Local;
                    bool scopeFixed_ = false; // set by the first use or instanceFor()
                };

                namespace hwloc_thread_cpu_allocator_detail
                {
                    struct Access {
                        static IThreadCpuAllocator &instance()
                        {
                            return HwlocThreadCpuAllocator::instanceFor(CpuAllocatorScope::Local);
                        }
                    };
                    struct CrossProcessAccess {
                        static IThreadCpuAllocator &instance()
                        {
                            return HwlocThreadCpuAllocator::instanceFor(CpuAllocatorScope::CrossProcess);
                        }
                    };
                } // namespace hwloc_thread_cpu_allocator_detail

                // Mix into an Environment to enable ThreadedNodeOptions::CpuAutoBind().
                // Cores are shared out among this process' threads only (default).
                struct HwlocThreadCpuAllocatorComponent
                    : ThreadCpuAllocatorComponent<hwloc_thread_cpu_allocator_detail::Access> {
                };

                // Opt-in replacement for HwlocThreadCpuAllocatorComponent: tm_kit
                // processes on the same machine (sharing crossProcessLockDir) also
                // negotiate cores through per-core flock files, so their busy
                // threads never share a physical core. HwlocThreadCpuAllocator::
                // configure() still applies: excludeCpus, avoidFirstCoreOfNumaNode
                // and smt are honoured exactly as in the Local scope.
                struct HwlocCrossProcessCpuAllocatorComponent
                    : ThreadCpuAllocatorComponent<hwloc_thread_cpu_allocator_detail::CrossProcessAccess> {
                };
            } // namespace basic
        } // namespace tm
    } // namespace cd606
} // namespace dev
#endif // TM_KIT_BASIC_HAS_HWLOC_CPU_ALLOCATOR

#endif // TM_KIT_BASIC_HWLOC_THREAD_CPU_ALLOCATOR_HPP_
