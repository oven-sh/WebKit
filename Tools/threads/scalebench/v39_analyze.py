#!/usr/bin/env python3
import json, statistics as st, sys

raw = "/root/WebKit/Tools/threads/scalebench/results-v39ab-raw.jsonl"
recs = []
for l in open(raw):
    l=l.strip()
    if not l.startswith("{"): continue
    try: recs.append(json.loads(l))
    except: pass

REF_CS_BI = "b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|c4bdd580f85ee058|af028188d7a56a96"
JAVA = {8:1.99, 16:1.99, 32:1.75}
# §33 v38 medians (BigInt arm; for vs-§33 column + >5% regression gate)
V38 = {1:19647.0, 2:20951.3, 4:16258.2, 8:14322.4, 16:13642.0, 32:14424.4}
V38_GILON = 13922.5

def med(xs): return st.median(xs) if xs else None
def cell(label): return [r for r in recs if r["label"]==label and r["rc"]==0]

def summarize(label, ref_csA=None):
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
    csA=set(r["json"]["checksumA"] for r in rs if isinstance(r.get("json"),dict))
    csC=set(r["json"]["checksumC"] for r in rs if isinstance(r.get("json"),dict))
    return dict(n=len(rs),wall=med(tot),wall_all=sorted(tot),rss=med(rss),user=med(user),sys=med(syss),
                pc1=med(pc1),pc2=med(pc2),pA=med(pa),pB=med(pb),pC=med(pc),
                cs=cs,csA=csA,csC=csC)

print("="*100)
print("Gate (0) W=32 stability 30 reps + bimodality (congc DEFAULT)")
print("="*100)
st32=[r for r in recs if r["label"]=="stab32-v39"]
rcs=[r["rc"] for r in st32]
nz=[r for r in st32 if r["rc"]!=0]
ok=[r for r in st32 if r["rc"]==0]
tots=sorted([r["total_ms"] for r in ok])
csset=set(r["cs"] for r in ok)
pas=sorted([r["json"]["phaseA_ms"] for r in ok if isinstance(r.get("json"),dict)])
syss=sorted([float(r["sys_s"]) for r in ok])
print(f"reps={len(st32)} nonzero_rc={len(nz)} SIGSEGV={sum(1 for r in st32 if r['rc'] in (139,11))}")
if nz:
    for r in nz: print(f"  FAIL rep={r['rep']} rc={r['rc']} err={r.get('err','')[:200]}")
print(f"cs_unique={len(csset)} cs_match_ref={csset=={REF_CS_BI}}")
if tots:
    spread=(tots[-1]/tots[0]-1)*100
    print(f"wall: min={tots[0]:.1f} med={st.median(tots):.1f} max={tots[-1]:.1f} spread={spread:.0f}% (gate: <=25%)")
if pas:
    pamed=st.median(pas)
    print(f"phaseA: min={pas[0]:.1f} med={pamed:.1f} max={pas[-1]:.1f}")
    slow=[p for p in pas if p > pamed*1.25]
    fast=[p for p in pas if p < pamed*0.75]
    print(f"bimodal slow-mode (phaseA>1.25*med): {len(slow)}/{len(pas)}  fast-mode (<0.75*med): {len(fast)}/{len(pas)}")
if syss:
    print(f"sys_s: min={syss[0]:.1f} med={st.median(syss):.1f} max={syss[-1]:.1f}")
loads_st=[float(r["load"]) for r in st32]
if loads_st: print(f"loadavg range: {min(loads_st):.1f}..{max(loads_st):.1f}")

def ladder(arm, prefix, ref_check):
    print()
    print("="*100)
    print(f"Gate (1{'b' if arm=='intcs' else ''}) {arm} arm ladder: 3-rep medians, base vs v39, speedup-vs-self, Java bar")
    print("="*100)
    v39_w1=None
    rows=[]
    for W in [1,2,4,8,16,32]:
        sb=summarize(f"{prefix}-w{W}-base")
        sv=summarize(f"{prefix}-w{W}-v39")
        if W==1: v39_w1=sv["wall"]
        rows.append((W,sb,sv))
    print(f"{'W':>3} {'base ms':>9} {'v39 ms':>9} {'vs base':>8} {'svself':>7} {'Java':>5} {'cpu':>5} {'RSS MB':>7} {'pc1+2':>7} {'phaseA':>8} {'phaseB':>7} {'phaseC':>6} {'csA':>18} {'csC ok':>6}")
    for W,sb,sv in rows:
        if sv["wall"] is None: print(f"{W:>3}  (no data)"); continue
        cpu=(sv["user"]+sv["sys"])/(sv["wall"]/1000)/W if sv["wall"] else 0
        pc12=(sv["pc1"] or 0)+(sv["pc2"] or 0)
        vs_base=(sv["wall"]/sb["wall"]-1)*100 if sb["wall"] else 0
        svself=v39_w1/sv["wall"] if v39_w1 else 0
        jb=JAVA.get(W,"")
        jb=f"{jb:.2f}" if jb else "—"
        csA="|".join(sorted(sv["csA"])) if sv["csA"] else "?"
        csCok = (sv["csC"]=={"af028188d7a56a96"})
        print(f"{W:>3} {sb['wall']:>9.1f} {sv['wall']:>9.1f} {vs_base:>+7.1f}% {svself:>6.3f}x {jb:>5} {cpu:>5.2f} {sv['rss']/1024:>7.0f} {pc12:>7.0f} {sv['pA']:>8.1f} {sv['pB']:>7.1f} {sv['pC']:>6.1f} {csA:>18} {'OK' if csCok else 'BAD':>6}")
        print(f"    v39 reps: {[f'{x:.0f}' for x in sv['wall_all']]}  base reps: {[f'{x:.0f}' for x in sb['wall_all']]}")
    # parsec/sibint
    sv1=summarize(f"{prefix}-w1-v39"); sv16=summarize(f"{prefix}-w16-v39")
    if sv1["wall"] and sv16["wall"]:
        pc12_1=(sv1["pc1"] or 0)+(sv1["pc2"] or 0)
        pc12_16=(sv16["pc1"] or 0)+(sv16["pc2"] or 0)
        parsec=(sv1["wall"]-pc12_1)/(sv16["wall"]-pc12_16) if (sv16["wall"]-pc12_16) else 0
        sibint=pc12_16/pc12_1 if pc12_1 else 0
        print(f"\nparallelizable-section speedup W=16: {parsec:.2f}x  pc-sibling-interference W=16/W=1: {sibint:.3f}x")
    # Java bar
    print(f"\nJava bar ({arm} arm):")
    for W in [8,16,32]:
        sv=summarize(f"{prefix}-w{W}-v39")
        if sv["wall"] and v39_w1:
            r=v39_w1/sv["wall"]
            need=v39_w1/JAVA[W]
            print(f"  W={W}: speedup-vs-self {r:.3f}x vs Java {JAVA[W]}x -> {'PASS' if r>JAVA[W] else 'FAIL'} (need wall<={need:.0f}ms, gap {sv['wall']-need:+.0f}ms)")
    return rows, v39_w1

rows_bi, w1_bi = ladder("BigInt", "giloff-bi", REF_CS_BI)
rows_ic, w1_ic = ladder("intcs", "giloff-ic", None)

# >5% regression check (BigInt arm vs §33)
print("\nNo same-host BigInt cell regressing >5% vs §33:")
maxreg=-100; maxW=0
for W,sb,sv in rows_bi:
    if sv["wall"] is None: continue
    d=(sv["wall"]/V38[W]-1)*100
    print(f"  W={W}: {sv['wall']:.1f} vs §33 {V38[W]:.1f} = {d:+.2f}%")
    if d>maxreg: maxreg=d; maxW=W
print(f"  max vs-§33 delta: W={maxW} {maxreg:+.1f}% -> {'PASS' if maxreg<=5 else 'FAIL'}")

print()
print("="*100)
print("Gate (2) GIL-on W=1 5-rep interleaved A/B")
print("="*100)
gb=summarize("gilon-w1-base"); gv=summarize("gilon-w1-v39")
if gv["wall"]:
    d=(gv["wall"]/gb["wall"]-1)*100
    print(f"base med {gb['wall']:.1f}  v39 med {gv['wall']:.1f}  delta {d:+.2f}% (gate <=2%: {'PASS' if d<=2 else 'FAIL'})")
    print(f"  base reps: {[f'{x:.0f}' for x in gb['wall_all']]}")
    print(f"  v39 reps:  {[f'{x:.0f}' for x in gv['wall_all']]}")
    print(f"  v39 vs §33 GIL-on {V38_GILON}: {(gv['wall']/V38_GILON-1)*100:+.1f}%")

# loadavg summary
all_loads=[float(r["load"]) for r in recs]
print(f"\nLoadavg across all reps: min={min(all_loads):.1f} max={max(all_loads):.1f}")
print(f"Total reps in raw: {len(recs)}  nonzero rc total: {sum(1 for r in recs if r['rc']!=0)}")

# intcs checksum reference check
ic_csA=set(); ic_csC=set(); ic_cs=set()
for r in recs:
    if r["label"].startswith("giloff-ic-") and r["rc"]==0 and isinstance(r.get("json"),dict):
        ic_csA.add(r["json"]["checksumA"]); ic_csC.add(r["json"]["checksumC"]); ic_cs.add(r["cs"])
print(f"\nintcs checksumA set: {sorted(ic_csA)} (ref: 8021f000)")
print(f"intcs checksumC set: {sorted(ic_csC)} (ref: af028188d7a56a96)")
print(f"intcs full cs tuples: {sorted(ic_cs)}")
