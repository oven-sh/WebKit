#!/usr/bin/env python3
"""§42 giloff-tax-bughunter analyzer: medians + checksum + RSS from results-v42b-raw.jsonl."""
import json, statistics as st, sys, os

SB = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(SB, "results-v42b-raw.jsonl")

# §41 / §39b / §40 reference tuples (checksumA, postings, checksumA2, checksumB, checksumC)
REF = {
    "intcs":   ("00000000e85d66e7", 4158480, "0000000015cf18bb", "00000000651b594b", "00000000abc7704f"),
    "nomap":   ("0000000098972b27", 4158480, "0000000064cd1705", "00000000dcf4c2d2", "00000000abc7704f"),
    "default": ("00000000b3e65a68", 4158957, "0000000039c33392", "c4bdd580f85ee058", "af028188d7a56a96"),
    "flat":    ("00000000686d6890", 4154468, "000000000fbbd673", "000000003af6b072", "00000000e1d22021"),
}

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

print(f"# §42 raw analysis ({len(recs)} records)")
nonzero = [r for r in recs if r["rc"] != 0]
print(f"nonzero rc: {len(nonzero)}")
for r in nonzero:
    print(f"  rc={r['rc']} {r['label']} W={r['W']} rep={r['rep']} err={r.get('err','')[:180]}")

print("\n## checksum verification vs §41 refs")
ck_ok = 0; ck_tot = 0; ck_bad = []
for r in recs:
    if not r["json"]: continue
    j = r["json"]; arm = r["arm"]
    ref = REF.get(arm)
    if not ref: continue
    tup = (j.get("checksumA"), j.get("postings"), j.get("checksumA2"), j.get("checksumB"), j.get("checksumC"))
    ck_tot += 1
    if tup == ref: ck_ok += 1
    else: ck_bad.append((r["label"], r["W"], r["rep"], arm, tup))
print(f"match: {ck_ok}/{ck_tot}")
for b in ck_bad: print(f"  MISMATCH {b}")

print("\n## per-cell medians (total_ms, RSS)")
print(f"{'label':<14} {'W':>3} {'n':>2} {'total_med':>9} {'rss_mb':>7} {'load_med':>6}  reps_ms / reps_rss_kb")
for key in sorted(groups):
    label, W, arm = key
    g = groups[key]
    tots = [r["json"]["total_ms"] for r in g if r["json"]]
    rss = [r["rss_kb"] for r in g if r.get("rss_kb")]
    loads = [float(r["load"]) for r in g]
    if not tots:
        print(f"{label:<14} {W:>3} {len(g):>2}  ALL FAILED")
        continue
    print(f"{label:<14} {W:>3} {len(g):>2} {med(tots):>9.1f} {med(rss)/1024:>7.0f} {med(loads):>6.2f}  {' '.join(f'{x:.0f}' for x in sorted(tots))} / {' '.join(str(x) for x in sorted(rss))}")
