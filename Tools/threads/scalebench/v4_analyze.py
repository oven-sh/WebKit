#!/usr/bin/env python3
import json, statistics as st

raw = "/root/WebKit/Tools/threads/scalebench/results-v4-raw.jsonl"
recs = []
for l in open(raw):
    l=l.strip()
    if not l.startswith("{"): continue
    try: recs.append(json.loads(l))
    except: pass

REF_BI = "b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|c4bdd580f85ee058|af028188d7a56a96"
REF_IC = "000000008021f000|4158957|000000001fc7d941|c4bdd580f85ee058|af028188d7a56a96"
WS = [1,2,4,8,16,32]

def med(xs): return st.median(xs) if xs else None
def cell(lang, W, arm):
    return [r for r in recs if r["lang"]==lang and r["W"]==W and r["arm"]==arm and r["rc"]==0]

def summ(lang, W, arm):
    rs = cell(lang, W, arm)
    tot=[r["total_ms"] for r in rs]
    rss=[r["rss_kb"] for r in rs]
    user=[float(r["user_s"]) for r in rs]
    syss=[float(r["sys_s"]) for r in rs]
    cpu=[r["cpu_pct"] for r in rs]
    cs=set(r["cs"] for r in rs)
    j=[r["json"] for r in rs if isinstance(r.get("json"),dict)]
    pa=[x["phaseA_ms"] for x in j]
    pb=[x["phaseB_ms"] for x in j]
    pc=[x["phaseC_ms"] for x in j]
    pc12=[x.get("postingsChecksum1_ms",0)+x.get("postingsChecksum2_ms",0) for x in j]
    return dict(n=len(rs), wall=med(tot), wall_all=sorted(tot), rss=med(rss),
                user=med(user), sys=med(syss), cpu=med(cpu), cs=cs,
                pA=med(pa), pB=med(pb), pC=med(pc), pc12=med(pc12))

print("="*100)
print("FULL MATRIX v4: 3-rep medians, jsc-v39 sha 9152bed9 (congc default-on)")
print("="*100)
print(f"{'lang':>10} {'arm':>6} {'W':>3} {'wall ms':>9} {'svself':>7} {'cpu%':>6} {'RSS MB':>7} {'phaseA':>8} {'phaseB':>7} {'phaseC':>7} {'pc1+2':>7} {'cs':>6}")

table={}
for lang,arm,ref in [("go","bigint",REF_BI),("java","bigint",REF_BI),("js","bigint",REF_BI),("js","intcs",REF_IC)]:
    w1=None
    for W in WS:
        s=summ(lang,W,arm)
        if s["wall"] is None: continue
        if W==1: w1=s["wall"]
        sv=w1/s["wall"] if w1 else 0
        csok = "OK" if s["cs"]=={ref} else "BAD"
        pa = f"{s['pA']:.1f}" if s['pA'] else "-"
        pb = f"{s['pB']:.1f}" if s['pB'] else "-"
        pcc = f"{s['pC']:.1f}" if s['pC'] else "-"
        pc12 = f"{s['pc12']:.0f}" if s['pc12'] else "-"
        table[(lang,arm,W)]=dict(wall=s["wall"],sv=sv,cpu=s["cpu"],rss=s["rss"]/1024,pA=s["pA"],pB=s["pB"],pC=s["pC"],pc12=s["pc12"],cs=csok,reps=s["wall_all"])
        print(f"{lang:>10} {arm:>6} {W:>3} {s['wall']:>9.1f} {sv:>6.3f}x {s['cpu']:>5}% {s['rss']/1024:>7.0f} {pa:>8} {pb:>7} {pcc:>7} {pc12:>7} {csok:>6}")
    print()

# JS-intcs vs Java
print("JS-intcs vs Java (FRESH Java numbers):")
for W in [8,16,32]:
    jv=table[("java","bigint",W)]; ic=table[("js","intcs",W)]
    print(f"  W={W}: JS-intcs sv={ic['sv']:.3f}x  Java sv={jv['sv']:.3f}x  -> {'PASS' if ic['sv']>=jv['sv'] else 'FAIL'}  (JS-intcs wall {ic['wall']:.0f}ms vs Java {jv['wall']:.0f}ms = {ic['wall']/jv['wall']:.2f}x slower abs)")

# parallelizable-section speedup (JS intcs)
ic1=table[("js","intcs",1)]; ic16=table[("js","intcs",16)]
parsec=(ic1["wall"]-ic1["pc12"])/(ic16["wall"]-ic16["pc12"])
print(f"\nJS-intcs parallelizable-section speedup W=16: {parsec:.2f}x (wall-pc12: {ic1['wall']-ic1['pc12']:.0f} / {ic16['wall']-ic16['pc12']:.0f})")
bi1=table[("js","bigint",1)]; bi16=table[("js","bigint",16)]
parsecb=(bi1["wall"]-bi1["pc12"])/(bi16["wall"]-bi16["pc12"])
print(f"JS-BigInt parallelizable-section speedup W=16: {parsecb:.2f}x")

# GIL-on
go=[r for r in recs if r["lang"]=="js-gilon" and r["rc"]==0]
gtot=sorted([r["total_ms"] for r in go])
print(f"\nGIL-on W=1 (5 reps): {[f'{x:.0f}' for x in gtot]} med={med(gtot):.1f}")
print(f"GIL-off W=1 BigInt med={bi1['wall']:.1f}  -> off/on ratio {bi1['wall']/med(gtot):.3f}x")

# checksum verification
print("\nChecksum verification:")
all_ok=True
for (lang,arm,W),d in table.items():
    if d["cs"]!="OK": all_ok=False; print(f"  BAD: {lang} {arm} W={W}")
print(f"  All checksums match references: {all_ok}")

# rc check
nz=[r for r in recs if r["rc"]!=0]
print(f"\nTotal recs={len(recs)}  nonzero rc={len(nz)}")
loads=[float(r["load"]) for r in recs]
print(f"Loadavg range: {min(loads):.1f}..{max(loads):.1f}")

# fast-mode characterization
print("\n"+"="*100)
print("(C) W=16 fast-mode characterization")
print("="*100)
fm=[r for r in recs if r["arm"]=="intcs-fm"]
ctrl=[r for r in recs if r["arm"]=="intcs-ctrl"]
def fmrow(rs,name):
    fast=[r for r in rs if r["phaseA_ms"]<4800]
    norm=[r for r in rs if r["phaseA_ms"]>=4800]
    print(f"{name}: {len(fast)}/{len(rs)} fast-mode")
    if fast:
        print(f"  FAST   med: phaseA {med([r['phaseA_ms'] for r in fast]):.0f} total {med([r['total_ms'] for r in fast]):.0f} sys {med([float(r['sys_s']) for r in fast]):.2f} cpu {med([r['cpu_pct'] for r in fast]):.0f}%")
    if norm:
        print(f"  NORMAL med: phaseA {med([r['phaseA_ms'] for r in norm]):.0f} total {med([r['total_ms'] for r in norm]):.0f} sys {med([float(r['sys_s']) for r in norm]):.2f} cpu {med([r['cpu_pct'] for r in norm]):.0f}%")
    if fm and "gc_eden" in rs[0]:
        ed=[r["gc_eden"] for r in rs]; fl=[r["gc_full"] for r in rs]
        print(f"  GC counts: eden {min(ed)}-{max(ed)} full {min(fl)}-{max(fl)}")
fmrow(fm,"JSC_logGC=1 (15 reps)")
fmrow(ctrl,"control no-logGC (15 reps)")
lhgf=[r for r in recs if r["arm"]=="intcs-lhgf2"]
if lhgf: fmrow(lhgf,"largeHeapGrowthFactor=2 (6 reps)")
