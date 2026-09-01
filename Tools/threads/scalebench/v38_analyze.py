#!/usr/bin/env python3
import json, statistics as st, sys

raw = "/root/WebKit/Tools/threads/scalebench/results-v38ab-raw.jsonl"
recs = []
for l in open(raw):
    l=l.strip()
    if not l.startswith("{"): continue
    try: recs.append(json.loads(l))
    except: pass

REF_CS = "b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|c4bdd580f85ee058|af028188d7a56a96"
JAVA = {8:1.99, 16:1.99, 32:1.75}
# §32 v37 medians (for vs-§32 column)
V37 = {1:20174.6, 2:21462.6, 4:16138.2, 8:14184.3, 16:13844.7, 32:14840.0}
V37_GILON = 13841.5
V37_PC12 = {1:4195, 16:5898}
V37_GILON_PC12 = 2670
V37_CPU = {1:1.04, 2:0.97, 4:0.82, 8:0.63, 16:0.51, 32:0.37}
V37_RSS = {1:420, 2:1672, 4:1377, 8:1308, 16:1339, 32:1318}
V37_PARSEC = 2.01  # parallelizable-section speedup W=16
V37_SIBINT = 1.406 # pc-sibling-interference W=16/W=1
V37_OFFON = 1.571  # GIL-off/on pc ratio
V37_CONGC = 16228.4

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
st32=[r for r in recs if r["label"]=="stab32-v38"]
rcs=[r["rc"] for r in st32]
nz=[r for r in st32 if r["rc"]!=0]
ok=[r for r in st32 if r["rc"]==0]
tots=sorted([r["total_ms"] for r in ok])
csset=set(r["cs"] for r in ok)
pas=sorted([r["json"]["phaseA_ms"] for r in ok if isinstance(r.get("json"),dict)])
syss=sorted([float(r["sys_s"]) for r in ok])
print(f"reps={len(st32)} nonzero_rc={len(nz)} SIGSEGV={sum(1 for r in st32 if r['rc'] in (139,11))}")
if nz:
    for r in nz: print(f"  FAIL rep={r['rep']} rc={r['rc']}")
print(f"cs_unique={len(csset)} cs_ok={csset=={REF_CS}}")
if tots:
    spread=(tots[-1]/tots[0]-1)*100
    print(f"wall: min={tots[0]:.1f} med={st.median(tots):.1f} max={tots[-1]:.1f} spread={spread:.0f}% (gate: <=25%)")
if pas:
    pamed=st.median(pas)
    print(f"phaseA: min={pas[0]:.1f} med={pamed:.1f} max={pas[-1]:.1f} spread={(pas[-1]/pas[0]-1)*100:.0f}%")
    slow=[p for p in pas if p > pamed*1.25]
    fast=[p for p in pas if p < pamed*0.75]
    print(f"bimodal slow-mode (phaseA>1.25*med): {len(slow)}/{len(pas)}  fast-mode (<0.75*med): {len(fast)}/{len(pas)}")
if syss:
    print(f"sys_s: min={syss[0]:.1f} med={st.median(syss):.1f} max={syss[-1]:.1f}")
loads_st=[float(r["load"]) for r in st32]
if loads_st: print(f"loadavg range: {min(loads_st):.1f}..{max(loads_st):.1f}")

print()
print("="*100)
print("Gate (1) ladder: 3-rep medians, base vs v38, speedup-vs-self, Java bar, parallelizable-speedup")
print("="*100)
v38_w1=None
rows=[]
for W in [1,2,4,8,16,32]:
    sb=summarize(f"giloff-w{W}-base")
    sv=summarize(f"giloff-w{W}-v38")
    if W==1: v38_w1=sv["wall"]
    rows.append((W,sb,sv))
print(f"{'W':>3} {'base ms':>9} {'v38 ms':>9} {'vs base':>8} {'vs §32':>8} {'svself':>7} {'Java':>5} {'cpu':>5} {'RSS MB':>7} {'pc1+2':>7} {'phaseA':>8} {'phaseB':>7} {'phaseC':>6} {'cs':>3}")
for W,sb,sv in rows:
    if sv["wall"] is None: continue
    cpu=(sv["user"]+sv["sys"])/(sv["wall"]/1000)/W if sv["wall"] else 0
    pc12=(sv["pc1"] or 0)+(sv["pc2"] or 0)
    vs_base=(sv["wall"]/sb["wall"]-1)*100 if sb["wall"] else 0
    vs_32=(sv["wall"]/V37[W]-1)*100
    svself=v38_w1/sv["wall"] if v38_w1 else 0
    jb=JAVA.get(W,"")
    jb=f"{jb:.2f}" if jb else "—"
    print(f"{W:>3} {sb['wall']:>9.1f} {sv['wall']:>9.1f} {vs_base:>+7.1f}% {vs_32:>+7.1f}% {svself:>6.3f}x {jb:>5} {cpu:>5.2f} {sv['rss']/1024:>7.0f} {pc12:>7.0f} {sv['pA']:>8.1f} {sv['pB']:>7.1f} {sv['pC']:>6.1f} {'OK' if sv['cs_ok'] else 'BAD':>3}")
    print(f"    v38 reps: {[f'{x:.0f}' for x in sv['wall_all']]}  base reps: {[f'{x:.0f}' for x in sb['wall_all']]}")

# parallelizable-section speedup + sibling-interference
sv1=summarize("giloff-w1-v38"); sv16=summarize("giloff-w16-v38")
if sv1["wall"] and sv16["wall"]:
    pc12_1=(sv1["pc1"] or 0)+(sv1["pc2"] or 0)
    pc12_16=(sv16["pc1"] or 0)+(sv16["pc2"] or 0)
    parsec=(sv1["wall"]-pc12_1)/(sv16["wall"]-pc12_16)
    sibint=pc12_16/pc12_1
    print()
    print(f"parallelizable-section speedup W=16: (W1_wall-pc12)/(W16_wall-pc12) = ({sv1['wall']:.0f}-{pc12_1:.0f})/({sv16['wall']:.0f}-{pc12_16:.0f}) = {parsec:.2f}x  (§32: {V37_PARSEC}x)")
    print(f"pc-sibling-interference W=16/W=1: {pc12_16:.0f}/{pc12_1:.0f} = {sibint:.3f}x  (§32: {V37_SIBINT}x)")
    print(f"W=1 GIL-off pc1+2: {pc12_1:.0f}ms  (§32: {V37_PC12[1]})")
    print(f"W=16 GIL-off pc1+2: {pc12_16:.0f}ms  (§32: {V37_PC12[16]})")

# W=1 neutrality
if v38_w1:
    neut=(v38_w1/V37[1]-1)*100
    print(f"\nW=1-neutrality vs §32 20174.6ms: {v38_w1:.1f}ms = {neut:+.2f}% (gate ±3%: {'PASS' if abs(neut)<=3 else 'FAIL'})")

# Java bar
print("\nJava bar:")
for W in [8,16,32]:
    sv=summarize(f"giloff-w{W}-v38")
    if sv["wall"] and v38_w1:
        r=v38_w1/sv["wall"]
        need=v38_w1/JAVA[W]
        print(f"  W={W}: speedup-vs-self {r:.3f}x vs Java {JAVA[W]}x -> {'PASS' if r>=JAVA[W] else 'FAIL'} (need wall<={need:.0f}ms, gap {sv['wall']-need:+.0f}ms)")

# >5% regression check
print("\nNo same-host cell regressing >5% vs §32:")
maxreg=-100; maxW=0
for W,sb,sv in rows:
    if sv["wall"] is None: continue
    d=(sv["wall"]/V37[W]-1)*100
    if d>maxreg: maxreg=d; maxW=W
print(f"  max vs-§32 delta: W={maxW} {maxreg:+.1f}% -> {'PASS' if maxreg<=5 else 'FAIL'}")

print()
print("="*100)
print("Gate (2) GIL-on W=1 5-rep interleaved A/B")
print("="*100)
gb=summarize("gilon-w1-base"); gv=summarize("gilon-w1-v38")
if gv["wall"]:
    d=(gv["wall"]/gb["wall"]-1)*100
    pc12_on=(gv["pc1"] or 0)+(gv["pc2"] or 0)
    print(f"base med {gb['wall']:.1f}  v38 med {gv['wall']:.1f}  delta {d:+.2f}% (gate <=2%: {'PASS' if d<=2 else 'FAIL'})")
    print(f"  base reps: {[f'{x:.0f}' for x in gb['wall_all']]}")
    print(f"  v38 reps:  {[f'{x:.0f}' for x in gv['wall_all']]}")
    print(f"  v38 vs §32 GIL-on {V37_GILON}: {(gv['wall']/V37_GILON-1)*100:+.1f}%")
    if sv1["wall"]:
        offon=((sv1['pc1'] or 0)+(sv1['pc2'] or 0))/pc12_on
        print(f"  GIL-off/on W=1 pc1+2 ratio: {offon:.3f}x (§32: {V37_OFFON}x)")

print()
print("="*100)
print("Gate (5) congc W=4 5x")
print("="*100)
cg=[r for r in recs if r["label"]=="congc-w4-v38"]
cgok=[r for r in cg if r["rc"]==0]
cgcs=set(r["cs"] for r in cgok)
print(f"reps={len(cg)} ok={len(cgok)} cs_ok={cgcs=={REF_CS}}")
if cgok:
    cgt=[r["total_ms"] for r in cgok]
    print(f"median {st.median(cgt):.1f}ms (§32: {V37_CONGC}ms, {(st.median(cgt)/V37_CONGC-1)*100:+.1f}%)  reps={[f'{x:.0f}' for x in sorted(cgt)]}")

# loadavg summary
all_loads=[float(r["load"]) for r in recs]
print(f"\nLoadavg across all reps: min={min(all_loads):.1f} max={max(all_loads):.1f}")
print(f"Total reps in raw: {len(recs)}")
