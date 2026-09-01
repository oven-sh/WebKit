#!/usr/bin/env python3
# Round-3 victim-provenance correlator.
# Usage: correlate.py <prov.log> <provout.log>
# For each victim cell printed by repro.js's $vm dump, reports per-cycle
# endMarking liveness (mark/NA bit of its atom) and every SWEEP record for its
# block, ordered by the shared seq counter.
import re, sys

provf, outf = sys.argv[1], sys.argv[2]
BLOCK = 0x4000

snaps = {}   # block -> list of (seq, cycle, marksStale, naStale, cellSize, marksbits, nabits)
sweeps = {}  # block -> list of (seq, tid, toFL, empty, marks, na, ctx)
maxseq_per_cycle = {}

for line in open(provf):
    if line.startswith("SNAP"):
        m = re.match(r"SNAP seq=(\d+) cycle=(\d+) block=(0x[0-9a-f]+) marksStale=(\d) naStale=(\d) cellSize=(\d+) marks=([0-9a-f]+) na=([0-9a-f]+)", line)
        seq, cyc, blk = int(m.group(1)), int(m.group(2)), int(m.group(3), 16)
        marks = m.group(7); na = m.group(8)
        snaps.setdefault(blk, []).append((seq, cyc, int(m.group(4)), int(m.group(5)), int(m.group(6)), marks, na))
        maxseq_per_cycle[cyc] = max(maxseq_per_cycle.get(cyc, 0), seq)
    elif line.startswith("SWEEP"):
        m = re.match(r"SWEEP seq=(\d+) tid=(\d+) block=(0x[0-9a-f]+) toFL=(\d) empty=(\d) marks=(\d) na=(\d) ctx=(\S+)", line)
        blk = int(m.group(3), 16)
        sweeps.setdefault(blk, []).append((int(m.group(1)), int(m.group(2)), int(m.group(4)), int(m.group(5)), int(m.group(6)), int(m.group(7)), m.group(8)))

def bit(hexstr, atom):
    # hexstr is 16 u64 words printed %016llx concatenated, word i = atoms [64i,64i+64)
    word = int(hexstr[(atom >> 6) * 16:(atom >> 6) * 16 + 16], 16)
    return (word >> (atom & 63)) & 1

victims = []
for line in open(outf):
    for m in re.finditer(r"Object: (0x[0-9a-f]+)", line):
        victims.append((int(m.group(1), 16), "cell"))
    m = re.search(r"tagged butterfly word (\d+)", line)
    if m:
        w = int(m.group(1))
        # strip NaN-boxing-ish tag: low 48 bits
        victims.append((w & ((1 << 48) - 1), "butterfly-word"))

seen = set()
for addr, kind in victims:
    if addr in seen: continue
    seen.add(addr)
    blk = addr & ~(BLOCK - 1)
    atom = (addr - blk) // 16
    print(f"VICTIM {kind} addr={addr:#x} block={blk:#x} atom={atom}")
    ss = snaps.get(blk)
    if not ss:
        print("   no SNAP records for this block (block never snapshotted!)")
    else:
        for (seq, cyc, mstale, nastale, csz, marks, na) in ss:
            print(f"   SNAP seq={seq} cycle={cyc} cellSize={csz} marksStale={mstale} naStale={nastale} markedHere={bit(marks, atom)} naHere={bit(na, atom)} blockMarkPop={sum(bin(int(marks[i*16:i*16+16],16)).count('1') for i in range(16))} naPop={sum(bin(int(na[i*16:i*16+16],16)).count('1') for i in range(16))}")
    ws = sweeps.get(blk, [])
    for w in ws:
        print(f"   SWEEP seq={w[0]} tid={w[1]} toFL={w[2]} empty={w[3]}(0=IsEmpty) marksMode={w[4]}(0=NotStale?) naMode={w[5]} ctx={w[6]}")
    print()
print(f"cycles={sorted(maxseq_per_cycle)} totalSnapBlocks={len(snaps)} totalSweepBlocks={len(sweeps)}")
