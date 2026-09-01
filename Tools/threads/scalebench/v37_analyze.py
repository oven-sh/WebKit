#!/usr/bin/env python3
import json, statistics as st, sys

raw = "/root/WebKit/Tools/threads/scalebench/results-v37ab-raw.jsonl"
recs = []
for l in open(raw):
    l=l.strip()
    if not l.startswith("{"): continue
    try: recs.append(json.loads(l))
    except: pass

REF_CS = "b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|c4bdd580f85ee058|af028188d7a56a96"
JAVA = {8:1.99, 16:1.99, 32:1.75}
# §31 v36 medians (for vs-§31 column)
V36 = {1:20730.2, 2:21474.9, 4:16412.9, 8:15002.3, 16:13928.9, 32:14624.5}
V36_GILON = 13714.1
V36_PC12 = {1:4244, 2:None, 4:None, 8:None, 16:6658, 32:None}
V36_PA = {}  # not strictly needed
V36_GILON_PC12 = 2696
V36_CPU = {1:1.05, 2:0.99, 4:0.82, 8:0.65, 16:0.40, 32:0.38}
V36_RSS = {1:422, 2:1594, 4:1299, 8:1305, 16:1323, 32:1335}
V36_PARSEC = 2.27  # parallelizable-section speedup W=16
V36_SIBINT = 1.569 # pc-sibling-interference W=16/W=1
V36_OFFON = 1.574  # GIL-off/on pc ratio

def med(xs): return st.median(xs) if xs else None
def cell(label): return [r for r in recs if r["label"]==label and r["rc"]==0]

def summarize(label):
    rs=cell(label)
    tot=[r["total_ms"] for r in rs]
    rss=[r["rss_kb"] for r in rs]
    user=[float(r["user_s"]) for r in rs]
    syss=[float(r["sys_s"]) for r in rs]
    pc1=[r["json"]["postingsChecksum1_ms"] for r in rs if isinstance(r.get("json"),dict)]
    pc2=[r["json"]["postingsChecksum2_ms"] for r in rs if isinstance(r.get("json"),dict)]
    pa=[r["json"]["phaseA_ms"] for r in rs if isinstance(r.get("json"),dict)]
    pb=[r["json"]["phaseB_ms"] for r in rs if isinstance(r.get("json"),dict)]
    pc=[r["json"]["phaseC_ms"] for r in rs if isinstance(r.get("json"),dict)]
    cs=set(r["cs"] for r in rs)
    loads=[r["load"] for r in rs]
    return dict(n=len(rs),wall=med(tot),wall_all=sorted(tot),rss=med(rss),user=med(user),sys=med(syss),
                pc1=med(pc1),pc2=med(pc2),pA=med(pa),pB=med(pb),pC=med(pc),cs_ok=(cs=={REF_CS}),loads=loads)

print("="*100)
print("Gate (0) W=32 stability 30 reps + bimodality")
print("="*100)
st32=[r for r in recs if r["label"]=="stab32-v37"]
rcs=[r["rc"] for r in st32]
nz=[r for r in st32 if r["rc"]!=0]
ok=[r for r in st32 if r["rc"]==0]
tots=sorted([r["total_ms"] for r in ok])
cs=set(r["cs"] for r in ok)
pas=sorted([r["json"]["phaseA_ms"] for r in ok if isinstance(r.get("json"),dict)])
syss=sorted([float(r["sys_s"]) for r in ok])
print(f"reps={len(st32)} nonzero_rc={len(nz)} SIGSEGV={sum(1 for r in st32 if r['rc'] in (139,11))}")
if nz:
    for r in nz: print(f"  FAIL rep={r['rep']} rc={r['rc']}")
print(f"cs_unique={len(cs)} cs_ok={cs=={REF_CS}}")
if tots:
    print(f"wall: min={tots[0]:.1f} med={st.median(tots):.1f} max={tots[-1]:.1f} spread={(tots[-1]/tots[0]-1)*100:.0f}%")
if pas:
    print(f"phaseA: min={pas[0]:.1f} med={st.median(pas):.1f} max={pas[-1]:.1f} spread={(pas[-1]/pas[0]-1)*100:.0f}%")
    # bimodality: slow-mode = phaseA > median*1.25 (heuristic matching §31 5/30 ~12.5s vs ~normal)
    pamed=st.median(pas)
    slow=[p for p in pas if p > pamed*1.25]
    print(f"bimodal slow-mode (phaseA>1.25*med): {len(slow)}/{len(pas)}  slow={[f'{p:.0f}' for p in slow]}")
if syss:
    print(f"sys_s: min={syss[0]:.1f} med={st.median(syss):.1f} max={syss[-1]:.1f}")
loads_st=[float(r["load"]) for r in st32]
if loads_st: print(f"loadavg range: {min(loads_st):.1f}..{max(loads_st):.1f}")

print()
print("="*100)
print("Gate (1) ladder: 3-rep medians, base vs v37, speedup-vs-self, Java bar")
print("="*100)
v37_w1=None
rows=[]
for W in [1,2,4,8,16,32]:
    sb=summarize(f"giloff-w{W}-base")
    sv=summarize(f"giloff-w{W}-v37")
    if W==1: v37_w1=sv["wall"]
    speedup_self = v37_w1/sv["wall"] if v37_w1 and sv["wall"] else 0
    cpu = (sv["user"]+sv["sys"])/(sv["wall"]/1000)/W if sv["wall"] else 0
    delta_base = (sv["wall"]/sb["wall"]-1)*100 if sb["wall"] and sv["wall"] else 0
    delta_v36 = (sv["wall"]/V36[W]-1)*100 if sv["wall"] else 0
    bar = JAVA.get(W)
    bar_ok = (speedup_self > bar) if bar else None
    rows.append((W,sb,sv,speedup_self,cpu,delta_base,delta_v36,bar,bar_ok))
    print(f"W={W:2d}  base={sb['wall'] or 0:8.1f}  v37={sv['wall'] or 0:8.1f}  Δbase={delta_base:+6.2f}%  Δ§31={delta_v36:+6.2f}%  "
          f"rss={(sv['rss'] or 0)/1024:6.1f}M  cpu={cpu:.2f}  speedup-self={speedup_self:.3f}x  "
          f"Java={bar or '-'}  {'PASS' if bar_ok else ('FAIL' if bar_ok is False else '-')}  "
          f"pc1+2={(sv['pc1'] or 0)+(sv['pc2'] or 0):7.1f}  pA={sv['pA'] or 0:7.1f}  pB={sv['pB'] or 0:6.1f}  cs_ok={sv['cs_ok']}")
    print(f"       walls v37={sv['wall_all']}  base loads={sb['loads']}  v37 loads={sv['loads']}")

print()
print("W=1-neutrality check (vs §31 20730.2ms, ±3%):")
if v37_w1:
    d=(v37_w1/20730.2-1)*100
    print(f"  v37 W=1 GIL-off = {v37_w1:.1f}ms  Δ={d:+.2f}%  {'PASS' if abs(d)<=3 else 'FAIL'}")

print()
print("="*100)
print("Gate (2) GIL-on W=1 5-rep interleaved A/B")
print("="*100)
gb=summarize("gilon-w1-base"); gv=summarize("gilon-w1-v37")
print(f"base reps: {gb['wall_all']}")
print(f"v37  reps: {gv['wall_all']}")
if gv['wall']:
    d=(gv['wall']/gb['wall']*100-100)
    print(f"base med={gb['wall']:.1f}  v37 med={gv['wall']:.1f}  Δ={d:+.2f}%  cs_ok={gv['cs_ok']}  (≤2% gate {'PASS' if d<=2 else 'FAIL'})")
    print(f"§31 v36 GIL-on W=1: {V36_GILON}  Δ§31={(gv['wall']/V36_GILON*100-100):+.2f}%")
    print(f"v37 GIL-on pc1+2={gv['pc1']+gv['pc2']:.1f}  (v36: {V36_GILON_PC12})")

print()
print("="*100)
print("Gate (5) congc W=4 5x")
print("="*100)
cg=[r for r in recs if r["label"]=="congc-w4-v37"]
rcs=[r["rc"] for r in cg]; tots=[r["total_ms"] for r in cg if r["rc"]==0]
cs=set(r["cs"] for r in cg if r["rc"]==0)
if cg:
    print(f"reps={len(cg)} rcs={rcs} med={med(tots):.1f}  cs_ok={cs=={REF_CS}}")

print()
print("="*100)
print("Attribution (W=16 + GIL-off/on ratio)")
print("="*100)
sv16=summarize("giloff-w16-v37")
sv1=summarize("giloff-w1-v37")
if sv16['wall'] and sv1['wall'] and gv.get('wall'):
    pc12_16=sv16['pc1']+sv16['pc2']; pc12_1=sv1['pc1']+sv1['pc2']; pc12_gilon=gv['pc1']+gv['pc2']
    print(f"v37 W=16 pc1+2 = {pc12_16:.1f} = {pc12_16/sv16['wall']*100:.0f}% of {sv16['wall']:.1f}  (v36: 6658 = 48% of 13929)")
    print(f"v37 GIL-off/GIL-on W=1 pc1+2 ratio: {pc12_1/pc12_gilon:.3f}x  (v36: {V36_OFFON}x)")
    print(f"v37 W=16/W=1 pc-sibling-interference: {pc12_16/pc12_1:.3f}x  (v36: {V36_SIBINT}x)")
    par16 = sv16['wall'] - pc12_16
    par1 = sv1['wall'] - pc12_1
    print(f"v37 parallelizable-section speedup (W=16): {par1/par16:.2f}x  (v36: {V36_PARSEC}x)")

print()
print("="*100)
print("ACCEPTANCE SUMMARY")
print("="*100)
if rows and all(r[2]['wall'] for r in rows):
    worst_delta_v36 = max(r[6] for r in rows)
    print(f"Max vs-§31 regression: {worst_delta_v36:+.2f}% (gate: <=+5%)")
    for W in [8,16,32]:
        r=[x for x in rows if x[0]==W][0]
        print(f"W={W}: speedup-self={r[3]:.3f}x vs Java {r[7]}x -> {'PASS' if r[8] else 'FAIL'}")
