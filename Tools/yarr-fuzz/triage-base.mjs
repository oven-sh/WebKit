#!/usr/bin/env node
// For jit~base mismatches: re-run each case on branch jit, branch interp, gates-off, base, base-interp, node and classify.
import { readFileSync, writeFileSync, readdirSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";
const here = dirname(fileURLToPath(import.meta.url));
const dir = process.argv[2] || join(here, "out");
const pairRe = new RegExp(process.argv[3] || "base");
const REL = join(here, "..", "..", "WebKitBuild", "Release", "bin", "jsc");
const BASE = process.env.YARR_FUZZ_BASE_JSC || join(here, "..", "..", "WebKitBuild", "Baseline", "bin", "jsc");
const unesc = (s) => s.replace(/\\u([0-9a-f]{4})/g, (m, h) => String.fromCharCode(parseInt(h, 16)));
const seen = new Set(); const skipped = {};
const rows = [];
for (const f of readdirSync(dir).filter((f) => f.startsWith("mismatch-"))) {
    for (const line of readFileSync(join(dir, f), "utf8").split("\n")) {
        if (!line) continue; let o; try { o = JSON.parse(line); } catch { continue; }
        if (!pairRe.test(o.pair)) continue;
        const key = o.a.p + " " + o.a.f + " " + o.a.s + " " + o.a.w;
        if (seen.has(key)) continue; seen.add(key);
        // Known, intended divergence from main: /u|/v matches never start/report mid-surrogate-pair.
        if (/[uv]/.test(o.a.f) && /\\ud[89a-f][0-9a-f]{2}/i.test(o.a.s) && !process.env.ALL) { skipped.surrogate = (skipped.surrogate||0) + 1; continue; }
        if ((o.ta > 300 || o.tb > 300) && !process.env.ALL) { skipped.slow = (skipped.slow||0) + 1; continue; }
        rows.push(o);
    }
}
console.log("unique mismatches:", rows.length, "skipped-known:", JSON.stringify(skipped));
const script = join(tmpdir(), "yarr-triage-base-" + process.pid + ".js");
let out = [];
for (const o of rows) {
    const p = unesc(o.a.p), f = o.a.f, s = unesc(o.a.s), w = o.a.w;
    const src = `const P=typeof print!=="undefined"?print:console.log; const to16=typeof $vm!=="undefined"?$vm.make16BitStringIfPossible:(x)=>x;
let re; try { re=new RegExp(${JSON.stringify(p)}, ${JSON.stringify(f)}); } catch(e) { P("ERR"); }
if (re) { const s=${w === 16 ? "to16" : ""}(${JSON.stringify(s)}); const r={}; re.lastIndex=0; const m=re.exec(s); r.e=m?[m.index,...m].map(x=>x===undefined?"~U":x):null; re.lastIndex=0; r.t=re.test(s);
 if (s.length<=60){const sw=[]; for(let k=0;k<=s.length;k++){re.lastIndex=k; const m=re.exec(s); sw.push(m?m.index+":"+m[0].length:"-");} r.sw=sw.join(",");}
 re.lastIndex=0; try{ const all=s.match(re); r.m=all?all.length+":"+all.slice(0,8).join("|"):null; }catch(e){r.m="ERR"} try{r.sp=s.split(re,12).map(x=>x===undefined?"~U":x).join("|")}catch(e){r.sp="ERR"}
 P(JSON.stringify(r)); }`;
    writeFileSync(script, src);
    const run = (bin, args) => { try { return execFileSync(bin, [...args, script], { encoding: "utf8", timeout: 60000, env: { ...process.env, ASAN_OPTIONS: "detect_leaks=0" } }).trim().split("\n").pop(); } catch (e) { return "FAIL:" + (e.signal || e.status); } };
    const jit = run(REL, ["--useDollarVM=1"]);
    const interp = run(REL, ["--useDollarVM=1", "--useRegExpJIT=0"]);
    const gates = run(REL, ["--useDollarVM=1", "--useRegExpLookbehindJIT=0", "--useRegExpAlternationFactoring=0", "--useRegExpAlternationDispatch=0"]);
    const base = run(BASE, ["--useDollarVM=1"]);
    const baseI = run(BASE, ["--useDollarVM=1", "--useRegExpJIT=0"]);
    const nd = run(process.execPath, []);
    let cls;
    if (jit === base && interp === baseI) cls = "no-diff-on-rerun";
    else if (jit === nd && interp === nd) cls = "FIXED (branch==node!=base)";
    else if (base === nd && jit !== nd) cls = "REGRESSION? (base==node!=branch)";
    else if (jit !== interp) cls = "JIT!=INTERP";
    else cls = "3-way-differ";
    out.push({ cls, p, f, s, w, jit, interp, gates, base, baseI, node: nd });
    console.log(cls, "/" + p + "/" + f, JSON.stringify(s).slice(0, 100), "w=" + w);
    if (cls !== "FIXED (branch==node!=base)" && cls !== "no-diff-on-rerun") {
        console.log("   jit   ", jit); console.log("   interp", interp); console.log("   gates ", gates); console.log("   base  ", base); console.log("   baseI ", baseI); console.log("   node  ", nd);
    }
}
writeFileSync(join(dir, "triage-base.json"), JSON.stringify(out, null, 1));
const counts = {}; for (const r of out) counts[r.cls] = (counts[r.cls] || 0) + 1; console.log(counts);
