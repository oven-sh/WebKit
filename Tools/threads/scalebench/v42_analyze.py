#!/usr/bin/env python3
"""§38 round-4 analyzer: medians + checksum verification from results-v42ab-raw.jsonl."""
import json, statistics as st, sys, os

SB = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(SB, "results-v42ab-raw.jsonl")

REF_FLAT = ("00000000686d6890", 4154468, "000000000fbbd673", "000000003af6b072", "00000000e1d22021")
REF_DEFAULT = ("00000000b3e65a68", 4158957, "0000000039c33392")  # checksumA, postings, checksumA2 prefix
REF_INTCS = ("000000008021f000", 4158957, "000000001fc7d941")

recs = []
with open(RAW) as f:
    for line in f:
        line = line.strip()
        if not line: continue
        try: recs.append(json.loads(line))
        except: pass

def med(xs): return st.median(xs) if xs else None

groups = {}
for r in recs:
    key = (r["label"], r["W"], r["arm"])
    groups.setdefault(key, []).append(r)

print(f"# §38 round-4 raw analysis ({len(recs)} records)")
nonzero_rc = [r for r in recs if r["rc"] != 0]
print(f"nonzero rc: {len(nonzero_rc)}")
for r in nonzero_rc:
    print(f"  rc={r['rc']} {r['label']} W={r['W']} err={r.get('err','')[:200]}")

print("\n## flat-arm checksum stability")
flat_recs = [r for r in recs if r["arm"] == "flat" and r["json"]]
ck_ok = 0; ck_bad = []
for r in flat_recs:
    j = r["json"]
    tup = (j.get("checksumA"), j.get("postings"), j.get("checksumA2"), j.get("checksumB"), j.get("checksumC"))
    if tup == REF_FLAT: ck_ok += 1
    else: ck_bad.append((r["label"], r["W"], r["rep"], tup))
print(f"flat checksum match: {ck_ok}/{len(flat_recs)}")
for b in ck_bad: print(f"  MISMATCH {b}")

print("\n## default/intcs reference tuples")
for arm, ref in (("default", REF_DEFAULT), ("intcs", REF_INTCS)):
    arecs = [r for r in recs if r["arm"] == arm and r["json"]]
    ok = 0; bad = []
    for r in arecs:
        j = r["json"]
        tup = (j.get("checksumA"), j.get("postings"), j.get("checksumA2"))
        if tup[:3] == ref[:3]: ok += 1
        else: bad.append((r["W"], r["rep"], tup))
    print(f"{arm}: {ok}/{len(arecs)} match")
    for b in bad: print(f"  MISMATCH {b}")

print("\n## per-cell medians (total_ms + phase breakdown)")
print(f"{'label':<14} {'W':>3} {'n':>3} {'total':>8} {'phaseA':>8} {'phaseB':>8} {'flat1':>7} {'flat2':>7} {'cs1':>6} {'cs2':>6} {'rss_mb':>8} {'load':>6}  reps")
for key in sorted(groups):
    label, W, arm = key
    g = groups[key]
    tots = [r["json"]["total_ms"] for r in g if r["json"]]
    if not tots: continue
    pa = [r["json"].get("phaseA_ms", 0) for r in g if r["json"]]
    pb = [r["json"].get("phaseB_ms", 0) for r in g if r["json"]]
    f1 = [r["json"].get("flatten1_ms", 0) for r in g if r["json"]]
    f2 = [r["json"].get("flatten2_ms", 0) for r in g if r["json"]]
    c1 = [r["json"].get("cs1_ms", r["json"].get("postingsChecksum1_ms", 0)) for r in g if r["json"]]
    c2 = [r["json"].get("cs2_ms", r["json"].get("postingsChecksum2_ms", 0)) for r in g if r["json"]]
    rss = [r["rss_kb"]/1024 for r in g if r.get("rss_kb")]
    loads = [float(r["load"]) for r in g]
    sreps = sorted(tots)
    print(f"{label:<14} {W:>3} {len(g):>3} {med(tots):>8.1f} {med(pa):>8.1f} {med(pb):>8.1f} {med(f1):>7.1f} {med(f2):>7.1f} {med(c1):>6.1f} {med(c2):>6.1f} {med(rss):>8.0f} {med(loads):>6.2f}  {' '.join(f'{x:.0f}' for x in sreps)}")

print("\n## W=32 stability detail")
stab = sorted(groups.get(("flat-stab32", 32, "flat"), []), key=lambda r: r["json"]["total_ms"] if r["json"] else 1e9)
if stab:
    pas = sorted(r["json"]["phaseA_ms"] for r in stab if r["json"])
    tots = sorted(r["json"]["total_ms"] for r in stab if r["json"])
    print(f"  n={len(stab)} total range {tots[0]:.0f}-{tots[-1]:.0f} (med {med(tots):.0f}, spread {(tots[-1]-tots[0])/med(tots)*100:.1f}%)")
    print(f"  phaseA sorted: {' '.join(f'{x:.0f}' for x in pas)}")
    slow = [x for x in pas if x > pas[0] * 1.4]
    print(f"  slow-mode (phaseA > 1.4× min): {len(slow)}/{len(pas)}")
