#!/usr/bin/env node
// Differential driver: runs regex-fuzz.js under several jsc configurations (and node) per seed,
// compares outputs line-by-line, records mismatches / crashes / timeouts.
// Usage: node drive.mjs --seeds 1-200 --count 400 --profile mixed --par 32 --configs jit,interp,gatesoff,node[,asan,asan-interp,base]
import { spawn } from "node:child_process";
import { mkdirSync, writeFileSync, appendFileSync } from "node:fs";
import { cpus } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const arg = (name, def) => { const i = process.argv.indexOf("--" + name); return i >= 0 ? process.argv[i + 1] : def; };
const [seedLo, seedHi] = (arg("seeds", "1-50")).split("-").map(Number);
const COUNT = Number(arg("count", "400"));
const PROFILE = arg("profile", "mixed");
const PAR = Number(arg("par", String(Math.max(4, cpus().length - 8))));
const CONFIGS = arg("configs", "jit,interp,gatesoff,node").split(",");
const OUTDIR = arg("out", join(here, "out"));
const TIMEOUT = Number(arg("timeout", "90")) * 1000;
const REL = arg("jsc", join(here, "..", "..", "WebKitBuild", "Release", "bin", "jsc"));
const ASAN = arg("asan", join(here, "..", "..", "WebKitBuild", "DebugASAN", "bin", "jsc"));
const BASE = arg("base", process.env.YARR_FUZZ_BASE_JSC || join(here, "..", "..", "WebKitBuild", "Baseline", "bin", "jsc")); // a jsc built from the merge base
mkdirSync(OUTDIR, { recursive: true });

const FUZZ = join(here, "regex-fuzz.js");
const common = ["--useDollarVM=1", "--validateOptions=1"];
const CFG = {
    jit:          { bin: REL,  args: [...common] },
    interp:       { bin: REL,  args: [...common, "--useRegExpJIT=0"] },
    gatesoff:     { bin: REL,  args: [...common, "--useRegExpLookbehindJIT=0", "--useRegExpAlternationFactoring=0", "--useRegExpAlternationDispatch=0"] },
    nolb:         { bin: REL,  args: [...common, "--useRegExpLookbehindJIT=0"] },
    nofactor:     { bin: REL,  args: [...common, "--useRegExpAlternationFactoring=0"] },
    nodispatch:   { bin: REL,  args: [...common, "--useRegExpAlternationDispatch=0"] },
    nodfg:        { bin: REL,  args: [...common, "--useDFGJIT=0"] },
    eager:        { bin: REL,  args: [...common, "--thresholdForJITAfterWarmUp=5", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=40", "--useConcurrentJIT=0"] },
    // Every load the RegExp JIT makes from the subject is bounds-checked (crashes on an out-of-bounds read that
    // ASAN cannot see because JIT code is not instrumented); slow, so pair it with a smaller --count.
    verifyreads:  { bin: REL,  args: [...common, "--verifyRegExpJITReads=1"] },
    // GC torture: collect continuously while matching, to shake out subject/RegExp lifetime bugs.
    gcstress:     { bin: REL,  args: [...common, "--collectContinuously=1", "--useGenerationalGC=0", "--useConcurrentGC=1"] },
    asan:         { bin: ASAN, args: [...common], env: { ASAN_OPTIONS: "detect_leaks=0:abort_on_error=1:allocator_may_return_null=1" } },
    "asan-interp":{ bin: ASAN, args: [...common, "--useRegExpJIT=0"], env: { ASAN_OPTIONS: "detect_leaks=0:abort_on_error=1:allocator_may_return_null=1" } },
    base:         { bin: BASE, args: [...common], env: { ASAN_OPTIONS: "detect_leaks=0:allocator_may_return_null=1" } },
    "base-interp":{ bin: BASE, args: [...common, "--useRegExpJIT=0"], env: { ASAN_OPTIONS: "detect_leaks=0:allocator_may_return_null=1" } },
    node:         { bin: process.execPath, args: [], node: true },
};

function runOne(cfgName, seed) {
    const c = CFG[cfgName];
    const args = c.node ? [FUZZ, String(seed), String(COUNT), PROFILE] : [...c.args, FUZZ, "--", String(seed), String(COUNT), PROFILE];
    return new Promise((resolve) => {
        const t0 = Date.now();
        const p = spawn(c.bin, args, { env: { ...process.env, ...(c.env || {}) }, stdio: ["ignore", "pipe", "pipe"] });
        let out = "", err = "", timedOut = false;
        p.stdout.on("data", (d) => { out += d; });
        p.stderr.on("data", (d) => { err += d; if (err.length > 200000) err = err.slice(-100000); });
        const timer = setTimeout(() => { timedOut = true; p.kill("SIGKILL"); }, TIMEOUT);
        p.on("close", (code, signal) => { clearTimeout(timer); resolve({ cfgName, seed, code, signal, timedOut, out, err, ms: Date.now() - t0 }); });
    });
}

function parse(out) {
    const map = new Map(); const times = new Map(); let done = false;
    for (const line of out.split("\n")) {
        if (line.startsWith("DONE ")) { done = true; continue; }
        const parts = line.split("\t");
        if (parts.length < 2) continue;
        map.set(parts[0], parts[1]);
        if (parts[2] !== undefined) times.set(parts[0], Number(parts[2]));
    }
    return { map, done, times };
}

// Normalize a record for comparison against node (error text differs; node lacks 8/16 distinction but that is in input, fine).
function normForNode(json) {
    return json.replace(/"err":"[^"]*"/, '"err":"E"').replace(/"x":"[^"]*"/, '"x":"E"');
}

const summary = { seeds: 0, cases: 0, mismatches: {}, crashes: [], timeouts: [], incomplete: [] };
const PROFILE_TAG = PROFILE.startsWith("file:") ? "file-" + PROFILE.slice(5).replace(/^.*\//, "").replace(/[^A-Za-z0-9._-]/g, "_") : PROFILE; // safe in filenames
const mismatchFile = join(OUTDIR, `mismatch-${PROFILE_TAG}.jsonl`);
const crashFile = join(OUTDIR, `crash-${PROFILE_TAG}.jsonl`);

async function doSeed(seed) {
    const results = await Promise.all(CONFIGS.map((c) => runOne(c, seed)));
    const by = Object.fromEntries(results.map((r) => [r.cfgName, r]));
    const ref = by[CONFIGS[0]];
    const refP = parse(ref.out);
    summary.seeds++;
    summary.cases += refP.map.size;
    for (const r of results) {
        const crashed = !r.timedOut && (r.signal || (r.code !== 0 && r.code !== null)) ;
        const p = parse(r.out);
        if (r.timedOut) {
            const lastKey = [...p.map.keys()].pop();
            summary.timeouts.push({ cfg: r.cfgName, seed, lastKey, n: p.map.size });
            appendFileSync(crashFile, JSON.stringify({ kind: "timeout", cfg: r.cfgName, seed, profile: PROFILE, count: COUNT, lastKey, n: p.map.size, nextOfRef: nextKeyAfter(refP.map, lastKey) }) + "\n");
        } else if (crashed || !p.done) {
            const lastKey = [...p.map.keys()].pop();
            const rec = { kind: crashed ? "crash" : "incomplete", cfg: r.cfgName, seed, profile: PROFILE, count: COUNT, code: r.code, signal: r.signal, lastKey, nextOfRef: nextKeyAfter(refP.map, lastKey), stderr: r.err.slice(-6000) };
            summary.crashes.push({ cfg: r.cfgName, seed, code: r.code, signal: r.signal, lastKey });
            appendFileSync(crashFile, JSON.stringify(rec) + "\n");
            console.error(`!!! ${rec.kind} cfg=${r.cfgName} seed=${seed} code=${r.code} sig=${r.signal} last=${lastKey} next=${JSON.stringify(rec.nextOfRef).slice(0, 300)}`);
            console.error(r.err.slice(-1500));
        }
    }
    // Metamorphic self-check failures (any config): semantically neutral rewrites disagreed.
    for (const r of results) {
        const p = parse(r.out);
        for (const [k, v] of p.map) {
            if (v.includes('"META"')) {
                summary.meta = (summary.meta || 0) + 1;
                appendFileSync(join(OUTDIR, `meta-${PROFILE_TAG}.jsonl`), JSON.stringify({ cfg: r.cfgName, seed, k, profile: PROFILE, rec: safeParse(v) }) + "\n");
            }
        }
    }
    // Compare each config against ref.
    for (const r of results.slice(1)) {
        const p = parse(r.out);
        const isNode = CFG[r.cfgName].node;
        let n = 0;
        for (const [k, v] of refP.map) {
            if (!p.map.has(k)) continue;
            let a = v, b = p.map.get(k);
            if (isNode) { a = normForNode(a); b = normForNode(b); }
            if (a !== b) {
                n++;
                const key = `${CONFIGS[0]}~${r.cfgName}`;
                summary.mismatches[key] = (summary.mismatches[key] || 0) + 1;
                if (n <= 50)
                    appendFileSync(mismatchFile, JSON.stringify({ pair: key, seed, k, profile: PROFILE, ta: refP.times.get(k), tb: p.times.get(k), a: JSON.parse(v), b: safeParse(p.map.get(k)) }) + "\n");
            }
        }
        if (n && !isNode) console.error(`### ${n} mismatches ${CONFIGS[0]} vs ${r.cfgName} seed=${seed}`);
    }
}
function safeParse(s) { try { return JSON.parse(s); } catch { return s; } }
function nextKeyAfter(map, lastKey) {
    // The case after lastKey in the reference run is the likely crasher.
    let seen = lastKey === undefined;
    for (const [k, v] of map) { if (seen) return { k, v: safeParse(v) }; if (k === lastKey) seen = true; }
    return null;
}

const seeds = []; for (let s = seedLo; s <= seedHi; ++s) seeds.push(s);
let next = 0; const t0 = Date.now();
const perSeed = CONFIGS.length;
const lanes = Math.max(1, Math.floor(PAR / perSeed));
console.error(`profile=${PROFILE} count=${COUNT} seeds=${seedLo}-${seedHi} configs=${CONFIGS} lanes=${lanes}`);
await Promise.all(Array.from({ length: lanes }, async () => {
    while (next < seeds.length) {
        const s = seeds[next++];
        await doSeed(s);
        if (summary.seeds % 10 === 0) console.error(`.. ${summary.seeds}/${seeds.length} seeds, ${summary.cases} cases, meta=${summary.meta||0} mism=${JSON.stringify(summary.mismatches)} crashes=${summary.crashes.length} timeouts=${summary.timeouts.length} ${((Date.now() - t0) / 1000) | 0}s`);
    }
}));
console.error(`== DONE profile=${PROFILE} seeds=${summary.seeds} cases=${summary.cases} meta=${summary.meta||0} mism=${JSON.stringify(summary.mismatches)} crashes=${summary.crashes.length} timeouts=${summary.timeouts.length} ${((Date.now() - t0) / 1000) | 0}s`);
writeFileSync(join(OUTDIR, `summary-${PROFILE_TAG}-${seedLo}-${seedHi}.json`), JSON.stringify(summary, null, 1));
