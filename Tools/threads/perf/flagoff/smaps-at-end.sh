#!/bin/bash
# smaps-at-end.sh <main|off> <test> : run one JetStream test and, at the moment its resident set is largest (sampled every 0.2 s from
# /proc/<pid>/smaps_rollup, the smaps of that moment kept), print resident memory by kind of mapping: file-backed (text, data, libraries),
# executable memory, and anonymous regions by size class (the GC heap and the malloc arenas are the large ones), so two configurations can
# be compared where a difference is. (The scavenger returns most of the heap once a test is over, so the end state says little.)
. "$(dirname "$0")/common.sh"
CFG=$1; T=$2; J=$(bin_of "$CFG"); cd "$JETSTREAM" || exit 1
if [ "$T" = ALL ]; then LISTJS=$(testlist_js); else LISTJS="testList=[\"$T\"]"; fi
"$J" -e "$LISTJS" cli.js > /dev/null 2>&1 &
PID=$!; BEST=0; SNAP=$(mktemp)
while kill -0 $PID 2>/dev/null; do
    R=$(awk '/^Rss:/{print $2}' /proc/$PID/smaps_rollup 2>/dev/null)
    if [ -n "$R" ] && [ "$R" -gt "$BEST" ]; then BEST=$R; cp /proc/$PID/smaps "$SNAP" 2>/dev/null; fi
    sleep 0.2
done
wait $PID 2>/dev/null
python3 - "$SNAP" <<'PY'
import sys, re, collections
kinds = collections.Counter(); counts = collections.Counter()
cur = None; total = 0
def flush(cur):
    global total
    if not cur: return
    rss = cur.get("Rss", 0); total += rss
    name = cur["name"]; perms = cur["perms"]; size = cur.get("Size", 0)
    if name in ("[heap]", "[stack]", "[vdso]", "[vvar]", "[vsyscall]"): k = name
    elif name.startswith("/"):
        base = name.rsplit("/", 1)[-1]
        k = "file %s %s" % (re.sub(r"[-.][0-9a-f.]+$", "", base)[:24], perms[:3])
    elif "x" in perms: k = "anon executable"
    else: k = "anon %s size %s" % (perms[:3], "<1MB" if size < 1024 else "<16MB" if size < 16384 else "<256MB" if size < 262144 else ">=256MB")
    kinds[k] += rss; counts[k] += 1
hdr = re.compile(r"^([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ \S+\s*(.*)$")
for line in open(sys.argv[1]):
    m = hdr.match(line)
    if m:
        flush(cur); cur = {"perms": m.group(3), "name": m.group(4).strip()}
    elif cur and ":" in line:
        k, v = line.split(":", 1)
        if k in ("Rss", "Size", "Pss", "Anonymous"): cur[k] = int(v.split()[0])
flush(cur)
print("peak resident %d kB" % total)
for k, v in kinds.most_common(14): print("%9d kB  %3d mappings  %s" % (v, counts[k], k))
PY
rm -f "$SNAP"
