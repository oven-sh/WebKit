#!/bin/bash
# cycles-breakdown.sh <rounds> <phase: full|six|first|llint|baseline|dfg> <name=jsc>...
# Where the main thread's cycles go, for any number of binaries, interleaved per round; one of the names must be "main".
# The 36 tests run in one process, as in multi.sh. The hardware events are read in groups of at most four so that
# none is multiplexed; every group carries cycles and instructions, which also says how much the runs of one binary
# differ among themselves. User mode only, main thread only (perf stat -i).
# Output: $OUT/breakdown.txt, lines "<phase> <name> <round> <group> <event> <count> <percent of time counted>";
# cycles-breakdown-compare.py prints each binary against main.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
JETSTREAM=${JETSTREAM:-$REPO/PerformanceTests/JetStream2}
OUT=${OUT:-$PWD/flagoff-out}; mkdir -p "$OUT"; O=$OUT/breakdown.txt
R=$1; PH=$2; shift 2
TESTS=${TESTS:-"Air Basic ML Babylon cdjs first-inspector-code-load multi-inspector-code-load Box2D octane-code-load crypto delta-blue earley-boyer gbemu mandreel navier-stokes pdfjs raytrace regexp richards splay typescript octane-zlib FlightPlanner OfflineAssembler UniPoker async-fs float-mm.c hash-map ai-astar gaussian-blur stanford-crypto-aes stanford-crypto-pbkdf2 stanford-crypto-sha256 json-stringify-inspector json-parse-inspector WSL"}
LIST='testList=['; first=1; for t in $TESTS; do [ $first = 1 ] || LIST+=','; LIST+="\"$t\""; first=0; done; LIST+=']'
# Groups. slots: the top-down level 1 split. front: what starves the front end. data: what the loads wait for.
# tlb: page walks. branch: mispredictions and clears. exec: the back end's ports and the store buffer.
GROUPS_DEFAULT="slots slots2 front front2 front3 data data2 data3 tlb tlb2 branch exec exec2"
GROUPS_LIST=${GROUPS_LIST:-$GROUPS_DEFAULT}
# Four events to a group besides cycles and instructions: a larger group is refused in a virtual machine (reported as
# "not supported" on one of its events). topdown.slots and the frontend_retired.* events are not available there at
# all: slots are six per cycle on this core, and idq_bubbles.core counts the slots the front end left empty.
events_of() {
  local e
  case $1 in
    slots)  e="idq_bubbles.core topdown.backend_bound_slots topdown.bad_spec_slots uops_retired.slots" ;;
    slots2) e="topdown.memory_bound_slots topdown.br_mispredict_slots uops_issued.any int_misc.recovery_cycles" ;;
    front)  e="icache_data.stalls icache_tag.stalls l2_rqsts.code_rd_miss itlb_misses.walk_active" ;;
    front2) e="idq.dsb_uops idq.mite_uops idq.ms_uops dsb2mite_switches.penalty_cycles" ;;
    front3) e="baclears.any itlb_misses.walk_completed itlb_misses.stlb_hit int_misc.clear_resteer_cycles" ;;
    data)   e="memory_activity.stalls_l1d_miss memory_activity.stalls_l2_miss memory_activity.stalls_l3_miss cycle_activity.stalls_total" ;;
    data2)  e="mem_inst_retired.all_loads mem_inst_retired.all_stores mem_load_retired.l1_miss mem_load_retired.l2_miss" ;;
    data3)  e="mem_load_retired.l3_miss mem_load_retired.fb_hit l1d.replacement l2_rqsts.demand_data_rd_miss" ;;
    tlb)    e="dtlb_load_misses.walk_active dtlb_load_misses.walk_completed dtlb_store_misses.walk_active dtlb_store_misses.walk_completed" ;;
    tlb2)   e="dtlb_load_misses.stlb_hit dtlb_store_misses.stlb_hit l2_rqsts.rfo_miss l1d_pend_miss.pending_cycles" ;;
    branch) e="br_inst_retired.all_branches br_misp_retired.all_branches br_misp_retired.indirect machine_clears.count" ;;
    exec)   e="exe_activity.exe_bound_0_ports exe_activity.1_ports_util exe_activity.2_ports_util exe_activity.bound_on_stores" ;;
    exec2)  e="resource_stalls.sb resource_stalls.scoreboard ld_blocks.store_forward exe_activity.bound_on_loads" ;;
  esac
  local out="cycles:u,instructions:u"; for x in $e; do out+=",$x:u"; done; echo "$out"
}
opts_of() {
  case $1 in
    full) echo "" ;; six) echo "" ;; first) echo "" ;;
    llint) echo "--useJIT=0" ;; baseline) echo "--useDFGJIT=0" ;; dfg) echo "--useFTLJIT=0" ;;
  esac
}
js_of() { case $1 in full) echo "" ;; first) echo "testIterationCount=1;" ;; *) echo "testIterationCount=6;" ;; esac; }
cd "$JETSTREAM" || exit 1
for r in $(seq 1 "$R"); do
  for g in $GROUPS_LIST; do
    ev=$(events_of $g)
    for nb in "$@"; do
      n=${nb%%=*}; J=${nb#*=}
      tmp=$(mktemp)
      perf stat -i -x, -o "$tmp" -e "{$ev}" "$J" $(opts_of $PH) -e "$(js_of $PH)$LIST" cli.js > /dev/null 2>&1
      grep -v '^#' "$tmp" | awk -F, -v ph=$PH -v n=$n -v r=$r -v g=$g 'NF>=5 && $1 ~ /^[0-9]/ {print ph, n, r, g, tolower($3), $1, $5} NF>=5 && $1 !~ /^[0-9]/ {print "cycles-breakdown: " g ": " $3 " " $1 > "/dev/stderr"}' >> "$O"
      rm -f "$tmp"
    done
  done
done
python3 "$HERE/cycles-breakdown-compare.py" "$OUT"
