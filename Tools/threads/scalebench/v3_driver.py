#!/usr/bin/env python3
"""results-v3 batch driver: run one cell (lang/arm/W) N reps synchronously,
parse program JSON + /usr/bin/time -v, append raw records to a JSONL file,
print a per-rep summary line and the cell median."""
import json, os, re, resource, subprocess, sys, tempfile, time

def _no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

SB = os.path.dirname(os.path.abspath(__file__))
JSC = "/root/WebKit/WebKitBuild/Release/bin/jsc"
PINNED = {
    "JSC_useJSThreads": "1", "JSC_useThreadGIL": "0", "JSC_useVMLite": "1",
    "JSC_useSharedAtomStringTable": "1", "JSC_useSharedGCHeap": "1",
    "JSC_useThreadGILOffUnsafe": "1",
}
RAW = os.path.join(SB, "results-v3-raw.jsonl")

def parse_time_v(text):
    rss = user = syss = elapsed = None
    for ln in text.splitlines():
        ln = ln.strip()
        if ln.startswith("Maximum resident set size"):
            rss = int(ln.split(":")[-1])
        elif ln.startswith("User time"):
            user = float(ln.split(":")[-1])
        elif ln.startswith("System time"):
            syss = float(ln.split(":")[-1])
        elif ln.startswith("Elapsed (wall clock)"):
            t = ln.split("):")[-1].strip()
            parts = [float(p) for p in t.split(":")]
            elapsed = sum(p * 60**i for i, p in enumerate(reversed(parts)))
    return rss, user, syss, elapsed

def run_cell(label, lang, arm, W, reps, cmd, env_extra, smoke=False):
    env = dict(os.environ); env.update(env_extra)
    if smoke: env["SCALEBENCH_SMOKE"] = "1"
    recs = []
    for rep in range(1, reps + 1):
        with tempfile.NamedTemporaryFile(mode="r+", suffix=".time") as tf:
            t0 = time.time()
            p = subprocess.run(["/usr/bin/time", "-v", "-o", tf.name] + cmd,
                               capture_output=True, text=True, env=env, cwd=SB,
                               timeout=1200, preexec_fn=_no_core)
            wall_outer = time.time() - t0
            tf.seek(0); tv = tf.read()
        rss, user, syss, elapsed = parse_time_v(tv)
        rec = {"label": label, "lang": lang, "arm": arm, "threads": W,
               "rep": rep, "exit": p.returncode, "rss_kb": rss,
               "user_s": user, "sys_s": syss, "elapsed_s": elapsed,
               "wall_outer_s": round(wall_outer, 3)}
        j = None
        for ln in p.stdout.splitlines():
            ln = ln.strip()
            if ln.startswith("{") and '"impl"' in ln:
                try: j = json.loads(ln)
                except Exception: pass
        if p.returncode != 0 or j is None:
            rec["failed"] = True
            rec["stderr_tail"] = p.stderr[-1500:]
            rec["stdout_tail"] = p.stdout[-500:]
        else:
            rec.update(j)
            if user is not None and elapsed and W:
                rec["cpu_util"] = round((user + syss) / (elapsed * W), 3)
        recs.append(rec)
        with open(RAW, "a") as f:
            f.write(json.dumps(rec) + "\n")
        cs = "|".join(str(rec.get(k)) for k in
                      ("checksumA", "postings", "checksumA2", "checksumB", "checksumC"))
        print(f"  rep{rep}: exit={p.returncode} total_ms={rec.get('total_ms')} "
              f"rss_kb={rss} cpu={rec.get('cpu_util')} cs={cs}", flush=True)
    ok = [r for r in recs if not r.get("failed")]
    if ok:
        med = sorted(ok, key=lambda r: r["total_ms"])[len(ok)//2]
        print(f"CELL {label} lang={lang} arm={arm} W={W}: median total_ms={med['total_ms']} "
              f"(A={med['phaseA_ms']} B={med['phaseB_ms']} C={med['phaseC_ms']}) "
              f"rss_kb_max={max(r['rss_kb'] for r in ok)} n_ok={len(ok)}/{reps}", flush=True)
    else:
        print(f"CELL {label} lang={lang} arm={arm} W={W}: ALL {reps} REPS FAILED", flush=True)
    return recs

def js_cmd(W, extra_args=(), flags=True):
    c = [JSC]
    c += ["bench.js", "--", str(W)] + list(extra_args)
    return c

def main():
    spec = json.loads(sys.argv[1])
    for cell in spec:
        kind = cell["kind"]; W = cell["W"]; reps = cell.get("reps", 3)
        smoke = cell.get("smoke", False)
        label = cell["label"]
        print(f"=== {label} W={W} ===", flush=True)
        if kind == "js":
            if cell.get("env_full") is not None:
                env = dict(cell["env_full"])
            else:
                env = dict(PINNED); env.update(cell.get("env", {}))
            args = ["ws"] if cell.get("ws") else []
            if smoke: args = ["smoke"] + args
            run_cell(label, "js", cell.get("arm", "naive"), W, reps,
                     [JSC, "bench.js", "--", str(W)] + args, env, smoke=smoke)
        elif kind == "js-plain":
            args = ["smoke"] if smoke else []
            run_cell(label, "js", "naive", W, reps,
                     [JSC, "bench.js", "--", str(W)] + args, {}, smoke=smoke)
        elif kind == "go":
            env = {"SCALEBENCH_WS": "1"} if cell.get("ws") else {}
            run_cell(label, "go", cell.get("arm", "naive"), W, reps,
                     [os.path.join(SB, "out", "bench-go"), str(W)], env, smoke=smoke)
        elif kind == "java":
            env = {"SCALEBENCH_WS": "1"} if cell.get("ws") else {}
            run_cell(label, "java", cell.get("arm", "naive"), W, reps,
                     ["java", "-cp", os.path.join(SB, "out"), "Bench", str(W)],
                     env, smoke=smoke)
        else:
            raise SystemExit(f"unknown kind {kind}")

if __name__ == "__main__":
    main()
