#!/usr/bin/env bash
# Runs the CpuAutoBind constraints binary under different process constraints and
# checks what the allocator derives from them. The binary takes one of:
#   full-cores <n>   every acquisition must be on a core whose PUs are all inside
#                    the process mask, and n distinct cores must be available
#   partial          SmtPolicy::RequireFullCore must reject the mask: no cores
#   node <index>     every acquisition must land on NUMA node <index>
#   count <n>        usable core count must be exactly n
set -u

BIN=${1:?usage: run_constraints.sh <constraints-binary>}
HERE=$(cd "$(dirname "$0")" && pwd)
failures=0

note() { printf '%s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*"; failures=$((failures + 1)); }

run() {
    # run <description> <expected-exit-0|1> -- <command...>
    local desc=$1 expected=$2
    shift 3
    local out rc
    out=$("$@" 2>&1)
    rc=$?
    if [ "$rc" -ne "$expected" ]; then
        fail "$desc (exit $rc, expected $expected)"
        printf '%s\n' "$out" | sed 's/^/    /'
    else
        note "ok:   $desc"
        [ -n "${VERBOSE:-}" ] && printf '%s\n' "$out" | sed 's/^/    /'
    fi
}

# ---- machine layout from sysfs -------------------------------------------
siblings_of() {
    local cpu=$1 file
    file=/sys/devices/system/cpu/cpu${cpu}/topology/thread_siblings_list
    [ -r "$file" ] || return 1
    local part out=()
    IFS=',' read -ra parts < "$file"
    for part in "${parts[@]}"; do
        if [[ $part == *-* ]]; then
            local lo=${part%-*} hi=${part#*-} c
            for ((c = lo; c <= hi; c++)); do out+=("$c"); done
        else
            out+=("$part")
        fi
    done
    printf '%s\n' "${out[@]}" | sort -n | tr '\n' ',' | sed 's/,$//'
}

# Prints "<lowest PU> <all siblings comma separated>" once per physical core.
core_list() {
    local cpu sib seen=""
    for cpu in $(seq 0 255); do
        sib=$(siblings_of "$cpu") || continue
        case ",$seen," in *",$sib,"*) continue ;; esac
        seen="$seen,$sib"
        printf '%s %s\n' "${sib%%,*}" "$sib"
    done
}

mapfile -t CORES < <(core_list)
if [ "${#CORES[@]}" -lt 4 ]; then
    note "skip: fewer than 4 physical cores visible"
    exit 0
fi
note "machine: ${#CORES[@]} physical cores, first is '${CORES[0]}'"

# A whole-core mask must contain every PU of the selected cores; the first two
# cores of each NUMA node are skipped because the allocator avoids them anyway.
SEL=("${CORES[@]:2:6}")
MASK=$(for c in "${SEL[@]}"; do echo "${c#* }"; done | paste -sd, -)

run "taskset whole cores ($MASK)" 0 -- taskset -c "$MASK" "$BIN" full-cores

# One PU per core only means "partial" when cores really have more than one PU.
# Without SMT (or with SMT disabled) such a mask is still whole cores, so the
# allocator is right to accept it: pick only multi-PU cores, else skip.
SMT_CORES=()
for c in "${CORES[@]:2}"; do
    set -- $c
    siblings=$2
    if [[ $siblings == *,* ]]; then
        SMT_CORES+=("$c")
    fi
done
if [ "${#SMT_CORES[@]}" -ge 2 ]; then
    PARTIAL=$(for c in "${SMT_CORES[@]:0:3}"; do
        # lowest PU only: its siblings are left out of the mask
        set -- $c
        echo "$1"
    done | paste -sd, -)
    run "taskset one PU per core ($PARTIAL)" 0 -- taskset -c "$PARTIAL" "$BIN" partial
else
    note "skip: no SMT sibling pairs, a one-PU-per-core mask is still whole cores"
fi

# ---- 2. numactl NUMA binding --------------------------------------------
if [ -d /sys/devices/system/node/node1 ] && command -v numactl >/dev/null 2>&1; then
    run "numactl --cpunodebind=1 --membind=1" 0 -- numactl --cpunodebind=1 --membind=1 "$BIN" node 1
else
    note "skip: numactl or NUMA node 1 unavailable"
fi

# ---- 3. whole machine ---------------------------------------------------
run "unconstrained" 0 -- "$BIN" count

note ""
if [ "$failures" -ne 0 ]; then
    printf '%d constraint check(s) failed\n' "$failures"
    exit 1
fi
note "ALL PASSED"
