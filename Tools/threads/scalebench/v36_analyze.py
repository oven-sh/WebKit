#!/usr/bin/env python3
import json, statistics as st, sys

raw = "/root/WebKit/Tools/threads/scalebench/results-v36ab-raw.jsonl"
recs = []
for l in open(raw):
    l=l.strip()
    if not l.startswith("{"): continue
    try: recs.append(json.loads(l))
    except: pass

REF_CS = "b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|c4bdd580f85ee058|af028188d7a56a96"
JAVA = {8:1.99, 16:1.99, 32:1.75}
# §30 v35 medians (for vs-§30 column)
V35 = {1:22333.2, 2:22630.1, 4:17728.0, 8:15348.6, 16:14478.1, 32:15358.2}
V35_GILON = 16087.2
V35_PC12 = {1:5073, 16:7059}
V35_PA = {1:14358, 2:14572, 4:9952, 8:7161, 16:6358, 32:6535}  # approx from §30 deltas
V35_GILON_PC12 = 3540

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
print("Gate (0) W=32 stability 30 reps")
print("="*100)
st32=[r for r in recs if r["label"]=="stab32-v36"]
rcs=[r["rc"] for r in st32]
nz=[r for r in st32 if r["rc"]!=0]
tots=sorted([r["total_ms"] for r in st32 if r["rc"]==0])
cs=set(r["cs"] for r in st32 if r["rc"]==0)
print(f"reps={len(st32)} nonzero_rc={len(nz)} SIGSEGV={sum(1 for r in st32 if r['rc'] in (139,11))}")
print(f"cs_unique={len(cs)} cs_ok={cs=={REF_CS}}")
print(f"wall: min={tots[0]:.1f} med={st.median(tots):.1f} max={tots[-1]:.1f}")
loads_st=[float(r["load"]) for r in st32]
print(f"loadavg range: {min(loads_st):.1f}..{max(loads_st):.1f}")

print()
print("="*100)
print("Gate (1) ladder: 3-rep medians, base vs v36, speedup-vs-self, Java bar")
print("="*100)
v36_w1=None
rows=[]
for W in [1,2,4,8,16,32]:
    sb=summarize(f"giloff-w{W}-base")
    sv=summarize(f"giloff-w{W}-v36")
    if W==1: v36_w1=sv["wall"]
    speedup_self = v36_w1/sv["wall"] if v36_w1 else 0
    cpu = (sv["user"]+sv["sys"])/(sv["wall"]/1000)/W
    delta_base = (sv["wall"]/sb["wall"]-1)*100
    delta_v35 = (sv["wall"]/V35[W]-1)*100
    bar = JAVA.get(W)
    bar_ok = (speedup_self > bar) if bar else None
    rows.append((W,sb,sv,speedup_self,cpu,delta_base,delta_v35,bar,bar_ok))
    print(f"W={W:2d}  base={sb['wall']:8.1f}  v36={sv['wall']:8.1f}  Δbase={delta_base:+6.2f}%  Δv35={delta_v35:+6.2f}%  "
          f"rss={sv['rss']/1024:6.1f}M  cpu={cpu:.2f}  speedup-self={speedup_self:.3f}x  "
          f"Java={bar or '-'}  {'PASS' if bar_ok else ('FAIL' if bar_ok is False else '-')}  "
          f"pc1+2={sv['pc1']+sv['pc2']:7.1f}  pA={sv['pA']:7.1f}  pB={sv['pB']:6.1f}  cs_ok={sv['cs_ok']}")
    print(f"       base loads={sb['loads']}  v36 loads={sv['loads']}")

print()
print("Per-section delta v36 vs §30 v35 (medians):")
for W,sb,sv,*_ in rows:
    print(f"  W={W:2d}: pc1+2={sv['pc1']+sv['pc2']:7.1f} (v35 W=1:{V35_PC12.get(1,'?')} W=16:{V35_PC12.get(16,'?')})  pA={sv['pA']:7.1f}  pB={sv['pB']:6.1f}  pC={sv['pC']:5.1f}")

print()
print("="*100)
print("Gate (2) GIL-on W=1 5-rep interleaved A/B")
print("="*100)
gb=summarize("gilon-w1-base"); gv=summarize("gilon-w1-v36")
print(f"base reps: {gb['wall_all']}")
print(f"v36  reps: {gv['wall_all']}")
if gv['wall']:
    print(f"base med={gb['wall']:.1f}  v36 med={gv['wall']:.1f}  Δ={gv['wall']/gb['wall']*100-100:+.2f}%  cs_ok={gv['cs_ok']}")
    print(f"§30 v35 GIL-on W=1: {V35_GILON}  Δv35={gv['wall']/V35_GILON*100-100:+.2f}%")
    print(f"v36 GIL-on pc1+2={gv['pc1']+gv['pc2']:.1f}  (v35: {V35_GILON_PC12})")

print()
print("="*100)
print("Gate (5) congc W=4 5x")
print("="*100)
cg=[r for r in recs if r["label"]=="congc-w4-v36"]
rcs=[r["rc"] for r in cg]; tots=[r["total_ms"] for r in cg if r["rc"]==0]
cs=set(r["cs"] for r in cg if r["rc"]==0)
if cg:
    print(f"reps={len(cg)} rcs={rcs} med={med(tots):.1f}  cs_ok={cs=={REF_CS}}")

print()
print("="*100)
print("Attribution (W=16 + GIL-off/on ratio)")
print("="*100)
sv16=summarize("giloff-w16-v36")
sv1=summarize("giloff-w1-v36")
if sv16['wall'] and sv1['wall'] and gv.get('wall'):
    pc12_16=sv16['pc1']+sv16['pc2']; pc12_1=sv1['pc1']+sv1['pc2']; pc12_gilon=gv['pc1']+gv['pc2']
    print(f"v36 W=16 pc1+2 = {pc12_16:.1f} = {pc12_16/sv16['wall']*100:.0f}% of {sv16['wall']:.1f}  (v35: 7059 = 49% of 14478)")
    print(f"v36 GIL-off/GIL-on W=1 pc1+2 ratio: {pc12_1/pc12_gilon:.3f}x  (v35: 1.433x)")
    print(f"v36 W=16/W=1 pc-sibling-interference: {pc12_16/pc12_1:.3f}x  (v35: 1.391x)")
    par16 = sv16['wall'] - pc12_16
    par1 = sv1['wall'] - pc12_1
    print(f"v36 parallelizable-section speedup (W=16): {par1/par16:.2f}x  (v35: 2.33x)")

print()
print("="*100)
print("ACCEPTANCE SUMMARY")
print("="*100)
worst_delta_v35 = max(r[6] for r in rows)
print(f"Max same-host vs-§30 regression: {worst_delta_v35:+.2f}% (gate: <=+5%)")
for W in [8,16,32]:
    r=[x for x in rows if x[0]==W][0]
    print(f"W={W}: speedup-self={r[3]:.3f}x vs Java {r[7]}x -> {'PASS' if r[8] else 'FAIL'}")
