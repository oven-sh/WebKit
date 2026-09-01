#!/usr/bin/env python3
import json, statistics as st, sys

raw = "/root/WebKit/Tools/threads/scalebench/results-v35ab-raw.jsonl"
recs = []
for l in open(raw):
    l=l.strip()
    if not l.startswith("{"): continue
    try: recs.append(json.loads(l))
    except: pass

REF_CS = "b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|c4bdd580f85ee058|af028188d7a56a96"

def med(xs): return st.median(xs) if xs else None

def cell(label):
    rs=[r for r in recs if r["label"]==label and r["rc"]==0]
    return rs

def summarize(label):
    rs=cell(label)
    tot=[r["total_ms"] for r in rs]
    rss=[r["rss_kb"] for r in rs]
    user=[r["user_s"] for r in rs]
    syss=[r["sys_s"] for r in rs]
    pc1=[r["json"]["postingsChecksum1_ms"] for r in rs if isinstance(r.get("json"),dict)]
    pc2=[r["json"]["postingsChecksum2_ms"] for r in rs if isinstance(r.get("json"),dict)]
    pa=[r["json"]["phaseA_ms"] for r in rs if isinstance(r.get("json"),dict)]
    pb=[r["json"]["phaseB_ms"] for r in rs if isinstance(r.get("json"),dict)]
    pc=[r["json"]["phaseC_ms"] for r in rs if isinstance(r.get("json"),dict)]
    cs=set(r["cs"] for r in rs)
    loads=[r["load"] for r in rs]
    return dict(n=len(rs),wall=med(tot),wall_all=tot,rss=med(rss),user=med(user),sys=med(syss),
                pc1=med(pc1),pc2=med(pc2),pA=med(pa),pB=med(pb),pC=med(pc),cs_ok=(cs=={REF_CS}),loads=loads)

print("="*90)
print("Gate (1) ladder: medians (3 reps each)")
print("="*90)
v35_w1=None
for W in [1,2,4,8,16,32]:
    sb=summarize(f"giloff-w{W}-base")
    sv=summarize(f"giloff-w{W}-v35")
    if W==1: v35_w1=sv["wall"]
    speedup_self = v35_w1/sv["wall"] if v35_w1 else 0
    cpu = (sv["user"]+sv["sys"])/(sv["wall"]/1000)/W
    delta_base = (sv["wall"]/sb["wall"]-1)*100
    print(f"W={W:2d}  base={sb['wall']:8.1f}  v35={sv['wall']:8.1f}  Δbase={delta_base:+6.2f}%  "
          f"rss={sv['rss']/1024:6.1f}M  cpu={cpu:.2f}  speedup-self={speedup_self:.3f}x  "
          f"pc1+2={sv['pc1']+sv['pc2']:7.1f}  pA={sv['pA']:7.1f}  pB={sv['pB']:6.1f}  cs_ok={sv['cs_ok']}  loads={sv['loads']}")

print()
print("§28 v34 reference: W=1 24558.4 / W=8 16211.0 / W=16 15024.2 / W=32 15933.9")
print("Java bar: W=8>1.99x W=16>1.99x W=32>1.75x")

print()
print("="*90)
print("Gate (2) GIL-on W=1 5-rep interleaved A/B")
print("="*90)
gb=summarize("gilon-w1-base"); gv=summarize("gilon-w1-v35")
print(f"base reps: {gb['wall_all']}")
print(f"v35  reps: {gv['wall_all']}")
print(f"base med={gb['wall']:.1f}  v35 med={gv['wall']:.1f}  Δ={gv['wall']/gb['wall']*100-100:+.2f}%  cs_ok={gv['cs_ok']}")
print(f"§28 v34 GIL-on W=1: 16157.6")

print()
print("="*90)
print("Gate (5) congc W=4 5x")
print("="*90)
cg=[r for r in recs if r["label"]=="congc-w4-v35"]
rcs=[r["rc"] for r in cg]; tots=[r["total_ms"] for r in cg if r["rc"]==0]
cs=set(r["cs"] for r in cg if r["rc"]==0)
print(f"reps={len(cg)} rcs={rcs} med={med(tots):.1f}  cs_ok={cs=={REF_CS}}")
print(f"§28 v34 congc med: 19186.5")

print()
print("="*90)
print("Per-section delta v35 vs §28 v34 (W=16 attribution)")
print("="*90)
sv16=summarize("giloff-w16-v35")
print(f"v35 W=16: pc1={sv16['pc1']:.1f} pc2={sv16['pc2']:.1f} sum={sv16['pc1']+sv16['pc2']:.1f}  pA={sv16['pA']:.1f} pB={sv16['pB']:.1f} pC={sv16['pC']:.1f}")
print(f"§28 v34 W=16: pc1+2=7708 (51% of 15012)")
sv1=summarize("giloff-w1-v35")
gv1=summarize("gilon-w1-v35")
print(f"v35 GIL-off/GIL-on W=1 pc1+2 ratio: {(sv1['pc1']+sv1['pc2'])/(gv1['pc1']+gv1['pc2']):.3f}x  (§28: 1.51-1.54x)")
print(f"  GIL-off pc1+2={sv1['pc1']+sv1['pc2']:.1f}  GIL-on pc1+2={gv1['pc1']+gv1['pc2']:.1f}")
