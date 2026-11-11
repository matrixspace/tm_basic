#!/usr/bin/env bash
# Multi-process scenarios for HwlocCrossProcessCpuAllocatorComponent. Every run
# uses a private lock directory (TM_CPU_LOCK_DIR) so the machine-wide default
# /dev/shm/tm_kit_cpu_locks is never touched.
set -u

BIN=${1:?usage: run_cross_process.sh <CrossProcessCpuTest binary>}
WORK=$(mktemp -d /dev/shm/tm_cpu_xproc_test.XXXXXX)
export TM_CPU_LOCK_DIR=$WORK/locks
failures=0
PIDS=()

cleanup() {
    for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill -9 "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

ok()   { printf 'ok:   %s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*"; failures=$((failures + 1)); }
check() { local desc=$1; shift; if "$@"; then ok "$desc"; else fail "$desc"; fi; }

# start <name> <args...>: runs in background, output in $WORK/<name>.out;
# waits until the process prints READY (or exits).
start() {
    local name=$1; shift
    start_wrapped "$name" "" "$@"
}
# start_wrapped <name> <wrapper command, may be empty> <args...>: like start,
# but runs "<wrapper> $BIN <args...>" (e.g. a taskset prefix).
start_wrapped() {
    local name=$1 wrapper=$2; shift 2
    local -a pre=()
    [ -n "$wrapper" ] && read -ra pre <<< "$wrapper"
    "${pre[@]}" "$BIN" "$@" > "$WORK/$name.out" 2>&1 &
    local pid=$!
    PIDS+=("$pid")
    eval "PID_$name=$pid"
    local i
    for ((i = 0; i < 500; i++)); do
        grep -q '^READY' "$WORK/$name.out" 2>/dev/null && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.02
    done
    return 1
}
cpus_of() { grep '^cpu ' "$WORK/$1.out" | awk '{print $2}'; }
count_of() { "$BIN" exhaust | awk '/^count /{print $2}'; }

# First PU of the physical core holding a CPU (the allocator's lock key).
key_of() { cut -d, -f1 < "/sys/devices/system/cpu/cpu$1/topology/thread_siblings_list" | cut -d- -f1; }
sibling_of() {
    local list
    list=$(cat "/sys/devices/system/cpu/cpu$1/topology/thread_siblings_list")
    tr ',' '\n' <<< "$list" | grep -vx "$1" | head -n1
}
# Predicates for check (compound conditions must be one command).
auto_avoids() { [ -n "$1" ] && [ "$(key_of "$1")" != "$(key_of "$2")" ]; }
rc_and_grep() { [ "$1" -eq "$2" ] && grep -q "$3" <<< "$4"; }
no_core_hits() {
    local c key=$(key_of "$1")
    for c in $2; do [ "$(key_of "$c")" = "$key" ] && return 1; done
    return 0
}
no_lock_held() {
    local f
    for f in "$TM_CPU_LOCK_DIR"/*.lock; do
        [ -e "$f" ] || continue
        flock -n "$f" true || return 1
    done
    return 0
}
distinct_cores() {
    local keys
    keys=$(for c in "$@"; do key_of "$c"; done | sort -n)
    [ "$(uniq <<< "$keys" | wc -l)" -eq "$#" ]
}

# ---- baseline -----------------------------------------------------------
TOTAL=$(count_of)
if [ -z "$TOTAL" ] || [ "$TOTAL" -lt 4 ]; then
    echo "skip: need at least 4 usable cores (got '${TOTAL:-none}')"
    exit 77
fi
ok "baseline: $TOTAL usable cores; exiting process released them all ($(count_of) again)"

# ---- 1. concurrent processes get distinct physical cores -----------------
N=$(( TOTAL < 8 ? TOTAL : 8 ))
for ((i = 0; i < N; i++)); do
    "$BIN" hold 1 10 > "$WORK/p$i.out" 2>&1 &
    PIDS+=("$!")
done
for ((i = 0; i < 300; i++)); do
    [ "$(cat "$WORK"/p*.out 2>/dev/null | grep -c '^READY')" -ge "$N" ] && break
    sleep 0.02
done
ALL=$(cat "$WORK"/p*.out | awk '/^cpu /{print $2}')
check "$N processes started together all bound ($(echo $ALL | tr '\n' ' '))" [ "$(wc -w <<< "$ALL")" -eq "$N" ]
check "$N concurrent processes landed on distinct physical cores" distinct_cores $ALL
check "remaining cores visible to another process" [ "$(count_of)" -eq $((TOTAL - N)) ]
for p in $(jobs -p); do kill -9 "$p" 2>/dev/null; done
wait
check "all cores free after the processes exit" [ "$(count_of)" -eq "$TOTAL" ]

# ---- 2. stress: processes x threads bind/reset ---------------------------
SHM=$WORK/occupancy
head -c 4096 /dev/zero > "$SHM"
for ((i = 0; i < 4; i++)); do
    "$BIN" stress 6 200 "$SHM" > "$WORK/s$i.out" 2>&1 &
    PIDS+=("$!")
done
src=0
for p in $(jobs -p); do wait "$p" || src=1; done
cat "$WORK"/s*.out | sed 's/^/    /'
check "stress: 4 processes x 6 threads x 200 rounds, no core overlap" [ "$src" -eq 0 ]

# ---- 3. kill -9 releases every lock --------------------------------------
check "holder of all cores starts" start full hold "$TOTAL" 30
check "no core left while the holder lives" [ "$(count_of)" -eq 0 ]
kill -9 "$PID_full"; wait "$PID_full" 2>/dev/null
check "kill -9 released all cores immediately" [ "$(count_of)" -eq "$TOTAL" ]

# ---- 4. thread exit releases its core ------------------------------------
check "threadexit process starts" start texit threadexit 5
check "core of an exited thread is free while its process lives" [ "$(count_of)" -eq "$TOTAL" ]
kill "$PID_texit" 2>/dev/null; wait "$PID_texit" 2>/dev/null

# ---- 5. explicit CPUs are exclusive across processes ----------------------
check "process A binds automatically" start A hold 1 30
X=$(cpus_of A)
out=$("$BIN" explicit "$X" 0); rc=$?
check "B: bindCurrentThreadTo({$X}) on A's core throws" [ "$rc" -eq 3 ]
check "B: error names A's pid" rc_and_grep 0 0 "pid $PID_A" "$out"
SIB=$(sibling_of "$X")
if [ -n "$SIB" ]; then
    out=$("$BIN" explicit "$SIB" 0); rc=$?
    check "B: SMT sibling $SIB of A's CPU $X also throws" [ "$rc" -eq 3 ]
fi
kill -9 "$PID_A"; wait "$PID_A" 2>/dev/null
check "B pins CPU $X explicitly and holds it" start B explicit "$X" 30
out=$("$BIN" hold 1 0)
Y=$(awk '/^cpu /{print $2}' <<< "$out")
check "automatic binding in another process avoids B's explicit core ($Y)" auto_avoids "$Y" "$X"
kill -9 "$PID_B"; wait "$PID_B" 2>/dev/null

# ---- 6. excludeCpus is honoured by other processes ------------------------
check "process E excludes CPU $X" start E excl "$X" 1 30
check "other processes skip E's excluded core and E's thread" [ "$(count_of)" -eq $((TOTAL - 2)) ]
OTHERS=$("$BIN" hold $((TOTAL - 2)) 0 | awk '/^cpu /{print $2}')
check "E's excluded core is not handed out elsewhere" no_core_hits "$X" "$OTHERS"
kill -9 "$PID_E"; wait "$PID_E" 2>/dev/null
check "excluded core free again after E exits" [ "$(count_of)" -eq "$TOTAL" ]
check "process A holds CPU $X" start A2 explicit "$X" 30
out=$("$BIN" excl "$X" 1 0); rc=$?
check "excludeCpus on a core held by another process throws" rc_and_grep "$rc" 3 excludeCpus "$out"
for scope in local cross; do
    for how in lease graph; do
        out=$("$BIN" selfexcl "$scope" "$how" "$X" 2>&1); rc=$?
        check "$scope scope: own excluded CPU $X rejected for $how explicit binding" rc_and_grep "$rc" 0 REJECTED "$out"
        if [ -n "$SIB" ]; then
            out=$("$BIN" selfexcl "$scope" "$how" "$SIB" 2>&1); rc=$?
            # excludeCpus={SIB}, binding SIB: same core rule
            check "$scope scope: own excluded sibling CPU $SIB rejected for $how" rc_and_grep "$rc" 0 REJECTED "$out"
        fi
    done
done
kill -9 "$PID_A2"; wait "$PID_A2" 2>/dev/null

# ---- 7. fork without exec does not extend the occupancy -------------------
check "fork: parent binds, forks a live child, releases" start F fork 5
check "core free while the forked child is still alive" [ "$(count_of)" -eq "$TOTAL" ]
wait "$PID_F" 2>/dev/null

# ---- 8. processes with different taskset masks share lock keys ------------
if [ -n "$SIB" ]; then
    check "taskset -c $X,$SIB process binds" start_wrapped T "taskset -c $X,$SIB" hold 1 30
    check "unconstrained process sees that core taken" [ "$(count_of)" -eq $((TOTAL - 1)) ]
    kill -9 "$PID_T"; wait "$PID_T" 2>/dev/null
fi

# ---- 9. graph node CpuAutoBind takes part ---------------------------------
check "graph node with CpuAutoBind starts" start G graph 30
check "graph node's core is taken for other processes" [ "$(count_of)" -eq $((TOTAL - 1)) ]
kill -9 "$PID_G"; wait "$PID_G" 2>/dev/null

# ---- 11. excludeCpus takes effect at configure(), before any binding --------
check "process C only configures excludeCpus={$X}" start C exclnow "$X" 30
check "configure() alone keeps other processes off the excluded core" [ "$(count_of)" -eq $((TOTAL - 1)) ]
out=$("$BIN" explicit "$X" 0); rc=$?
check "explicit binding to C's excluded core from another process throws" [ "$rc" -eq 3 ]
kill -9 "$PID_C"; wait "$PID_C" 2>/dev/null
check "C's exclusion released when it exits" [ "$(count_of)" -eq "$TOTAL" ]
start A3 explicit "$X" 30
out=$("$BIN" exclnow "$X" 0); rc=$?
check "configure() with excludeCpus on a core held elsewhere throws" rc_and_grep "$rc" 3 excludeCpus "$out"
kill -9 "$PID_A3"; wait "$PID_A3" 2>/dev/null

# ---- 12. fork child of an excludeCpus process can still bind ---------------
check "forkexcl: parent excludes $X and binds, child binds" start FX forkexcl "$X" 30
check "forked child bound without error" grep -q '^childexit 0' "$WORK/FX.out"
CC=$(awk '/^childcpu /{print $2}' "$WORK/FX.out")
check "forked child avoided the excluded core ($CC)" auto_avoids "$CC" "$X"
check "parent's exclusion still effective for other processes" no_core_hits "$X" "$("$BIN" hold 1 0 | awk '/^cpu /{print $2}')"
kill -9 "$PID_FX"; wait "$PID_FX" 2>/dev/null

# ---- 13. NUMA choice counts cores held by other processes ------------------
if [ -r /sys/devices/system/node/node1/cpulist ]; then
    on_node() { local n=$1 c; shift; for c in "$@"; do [ -e "/sys/devices/system/node/node$n/cpu$c" ] || return 1; done; }
    check "process N fills NUMA node 0 except 2 cores" start N fillnuma 0 2 30
    out=$("$BIN" hold 4 2)
    NC=$(awk '/^cpu /{print $2}' <<< "$out")
    check "4 threads of a new process stay on one node (node 1): $(echo $NC)" on_node 1 $NC
    check "the 4 concurrently held cores are distinct physical cores" distinct_cores $NC
    kill -9 "$PID_N"; wait "$PID_N" 2>/dev/null
else
    echo "skip: single NUMA node (scenario 13)"
fi

# ---- 14. review #1: an exception from a later core lock rolls back earlier ones
Z=$("$BIN" hold 2 0 | awk '/^cpu /{print $2}' | tail -n1)
out=$("$BIN" rollback "$X" "$Z" 2>&1); rc=$?
check "reserve {$X,$Z} with a broken lock file for $Z throws and frees $X's lock" rc_and_grep "$rc" 0 FREE "$out"
check "no core leaked after the failed reservation" [ "$(count_of)" -eq "$TOTAL" ]

# ---- 15. review #2: fork racing with bind/reset leaves no lock in children --
check "forkrace: 20 live children forked while another thread binds" start FR forkrace 20
check "all cores free while the forked children are alive" [ "$(count_of)" -eq "$TOTAL" ]
kill -9 "$PID_FR"; for k in $(awk '/^child /{print $2}' "$WORK/FR.out"); do kill -9 "$k" 2>/dev/null; done
wait "$PID_FR" 2>/dev/null

# ---- 16. review #3: a forked child keeps excludeCpus after its parent exits --
"$BIN" forkexclorphan "$X" 30 > "$WORK/OR.out" 2>&1
for ((i = 0; i < 300; i++)); do grep -q '^READY' "$WORK/OR.out" && break; sleep 0.02; done
OPID=$(awk '/^pid /{print $2}' "$WORK/OR.out")
PIDS+=("$OPID")
check "orphaned child bound after its parent exited" grep -q '^childcpu ' "$WORK/OR.out"
out=$("$BIN" explicit "$X" 0 2>&1); rc=$?
check "another process still cannot take the child's excluded core $X" [ "$rc" -eq 3 ]
[ -n "$OPID" ] && kill -9 "$OPID" 2>/dev/null
sleep 0.1
check "excluded core free after the child exits" [ "$(count_of)" -eq "$TOTAL" ]

# ---- 17. review #4: existing lock files are opened without O_CREAT ----------
if command -v strace > /dev/null; then
    "$BIN" hold 1 0 > /dev/null 2>&1   # make sure lock files exist
    strace -f -e trace=openat -o "$WORK/strace.out" "$BIN" hold 1 0 > /dev/null 2>&1
    check "an existing lock file is opened without O_CREAT" bash -c "! grep 'core-.*\.lock' '$WORK/strace.out' | grep -v ENOENT | grep -q O_CREAT"
else
    echo "skip: strace unavailable (scenario 17)"
fi

# ---- 10. default (Local) scope is unchanged; mixing is rejected ------------
check "local process L1" start L1 local 30
check "local process L2" start L2 local 30
check "Local scope: two processes pick the same CPU (no negotiation)" [ "$(cpus_of L1)" = "$(cpus_of L2)" ]
check "Local scope holds no cross-process lock" no_lock_held
kill -9 "$PID_L1" "$PID_L2"; wait 2>/dev/null
out=$("$BIN" mix); rc=$?
check "mixing Local and CrossProcess components throws logic_error" rc_and_grep "$rc" 0 MIXERR "$out"

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed"
    exit 1
fi
echo "ALL PASSED"
