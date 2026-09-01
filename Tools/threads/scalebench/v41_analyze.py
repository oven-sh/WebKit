#!/usr/bin/env python3
# §37 analyzer: medians from results-v41ab-raw.jsonl
import json, statistics as st, sys
from collections import defaultdict

raw = [json.loads(l) for l in open('Tools/threads/scalebench/results-v41ab-raw.jsonl')]
groups = defaultdict(list)
for r in raw:
    if r['rc'] != 0:
        print(f"!! rc={r['rc']} {r['label']} W={r['W']} arm={r['arm']}: {r.get('err','')}", file=sys.stderr)
        continue
    j = r['json']
    groups[(r['label'], r['W'], r['arm'])].append(r)

REF_FLAT = '686d6890|4154468|0fbbd673|3af6b072|e1d22021'
REF_DEFAULT = 'b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|c4bdd580f85ee058|af028188d7a56a96'
REF_INTCS_A = '8021f000'

def cs(j):
    # Strip the 16-hex-char zero-pad to the significant width (8 for the
    # flat-arm 32-bit checksums, 16 for default's 64-bit) — preserves leading
    # nibble zeros inside the value (e.g. 0fbbd673).
    def s(x): return x[8:] if len(x) == 16 and x[:8] == '00000000' else x
    return f"{s(j['checksumA'])}|{j['postings']}|{s(j['checksumA2'])}|{s(j['checksumB'])}|{s(j['checksumC'])}"

print(f"{'label':12s} W  arm      n  total_med    reps                          phaseA  phaseB  fl1   cs1   pc2   flGC  rss_mb  cs_ok")
for (label, W, arm), rs in sorted(groups.items(), key=lambda x: (x[0][2], x[0][0], x[0][1])):
    totals = sorted(r['json']['total_ms'] for r in rs)
    med = st.median(totals)
    pA = st.median(r['json']['phaseA_ms'] for r in rs)
    pB = st.median(r['json']['phaseB_ms'] for r in rs)
    fl1 = st.median(r['json'].get('flatten1_ms', 0) for r in rs)
    cs1 = st.median(r['json'].get('cs1_ms', 0) for r in rs)
    pc2 = st.median(r['json']['postingsChecksum2_ms'] for r in rs)
    flgc = st.median(r['json'].get('flattenGC_ms', 0) for r in rs)
    rss = st.median(r['rss_kb'] for r in rs) / 1024
    css = set(cs(r['json']) for r in rs)
    ok = '?'
    if arm == 'flat': ok = 'OK' if css == {REF_FLAT} else f'FAIL {css}'
    elif arm == 'default': ok = 'OK' if css == {REF_DEFAULT} else f'FAIL {css}'
    elif arm == 'intcs':
        ok = 'OK' if all(c.startswith(REF_INTCS_A + '|') for c in css) and len(css) == 1 else f'FAIL {css}'
    reps_s = ','.join(f'{t:.0f}' for t in totals)
    print(f"{label:12s} {W:2d} {arm:8s} {len(rs):2d}  {med:8.1f}   [{reps_s:28s}] {pA:6.1f} {pB:6.1f} {fl1:5.1f} {cs1:5.1f} {pc2:5.1f} {flgc:5.1f} {rss:6.0f}  {ok}")

# stab32 spread
stab = [r['json']['total_ms'] for r in raw if r['label'] == 'flat-stab32' and r['rc'] == 0]
if stab:
    stab.sort()
    spread = (max(stab) - min(stab)) / st.median(stab) * 100
    pA = sorted(r['json']['phaseA_ms'] for r in raw if r['label'] == 'flat-stab32' and r['rc'] == 0)
    print(f"\nW=32 stability ({len(stab)} reps): med={st.median(stab):.1f} min={min(stab):.1f} max={max(stab):.1f} spread={spread:.1f}%")
    print(f"  phaseA sorted: {','.join(f'{x:.0f}' for x in pA)}")

nz = [r for r in raw if r['rc'] != 0]
print(f"\nrc!=0: {len(nz)}/{len(raw)}")
