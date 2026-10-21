// Interleaved arms with BUN_JSC_logGC=1; per-arm medians of GC-log counts.
// usage: bun run.mjs <label> <reps> <minMB> "<script args>" name=<exe>[,ENV=V...] ...   (env NOLOG=1: no GC log; MAXMB: upper bound)
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { parseLog, summarize } from "./gcl.mjs";
const [label, repsArg, minMB, cmdline, ...specs] = process.argv.slice(2);
const reps = Number(repsArg), noLog = !!process.env.NOLOG, maxMB = process.env.MAXMB ? Number(process.env.MAXMB) : Infinity;
const arms = specs.map(spec => { const eq = spec.indexOf("="); const [exe, ...envs] = spec.slice(eq + 1).split(","); const env = {}; for (const e of envs) { const i = e.indexOf("="); env[e.slice(0, i)] = e.slice(i + 1); } return { name: spec.slice(0, eq), exe, env, runs: [] }; });
const load = () => readFileSync("/proc/loadavg", "utf8").split(" ").slice(0, 2).join("/");
const load0 = load(), t0 = Date.now();
mkdirSync("/tmp/lcvm-logs", { recursive: true });
for (let r = 0; r < reps; r++) for (const arm of r % 2 ? [...arms].reverse() : arms) {
  const p = Bun.spawn({ cmd: [arm.exe, ...cmdline.split(" ").filter(Boolean)], env: { PATH: process.env.PATH, HOME: process.env.HOME, ...(noLog ? {} : { BUN_JSC_logGC: "1" }), ...arm.env }, stdout: "pipe", stderr: "pipe" });
  const [out, err, code] = await Promise.all([p.stdout.text(), p.stderr.text(), p.exited]);
  if (code !== 0) { console.log(`${arm.name} FAILED ${code}: ${err.slice(-300)}`); continue; }
  const jsonLine = out.trim().split("\n").filter(l => l.startsWith("{")).pop();
  const j = jsonLine ? JSON.parse(jsonLine) : {};
  const ru = p.resourceUsage();
  const run = { metric: j.nsPerOp ?? j.ms ?? j.medMs ?? NaN, sys: Number(ru.cpuTime.system) / 1e6, user: Number(ru.cpuTime.user) / 1e6, vcs: Number(ru.contextSwitches.voluntary), rss: Number(ru.maxRSS) / 1048576 };
  if (!noLog) { const log = out + err; writeFileSync(`/tmp/lcvm-logs/${label}-${arm.name}-${r}.log`, log); Object.assign(run, summarize(parseLog(log), Number(minMB) * 1024, maxMB * 1024)); }
  arm.runs.push(run);
}
const med = xs => { const s = xs.filter(x => !Number.isNaN(x)).sort((a, b) => a - b); if (!s.length) return NaN; const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };
const f = (arm, k, d = 1, scale = 1) => { const xs = arm.runs.map(x => x[k] * scale).filter(x => !Number.isNaN(x)); if (!xs.length) return "-"; return `${med(xs).toFixed(d)} (${Math.min(...xs).toFixed(d)}-${Math.max(...xs).toFixed(d)})`; };
console.log(`### ${label}: ${cmdline}; ${reps} fresh processes per arm, interleaved; collections >= ${minMB} MB${maxMB < Infinity ? " and < " + maxMB + " MB" : ""}; load ${load0} -> ${load()}; ${((Date.now() - t0) / 1000).toFixed(0)} s; median (min-max)`);
const colsDef = noLog
  ? [["script metric", "metric", 1], ["peak RSS MB", "rss", 0], ["user s", "user", 2], ["sys s", "sys", 2], ["vol cs", "vcs", 0]]
  : [["script metric", "metric", 1], ["peak RSS MB", "rss", 0], ["n", "n", 0], ["marked MB", "markedMB", 0], ["in-pause %", "inPauseShare", 1, 100], ["hf>=.98 %", "hfFullShare", 0, 100], ["helpers %", "helpersShare", 1, 100], ["helpers used", "helpersUsedMean", 1], ["max pause ms", "maxPause", 1], ["pause ms (these)", "pauseBig", 0], ["pause ms (all)", "pauseAll", 0], ["pause ms/MB", "pausePerMB", 3], ["cycle ms (these)", "cycleBig", 0], ["marked outside pauses per MB allocated", "concPerAlloc", 2], ["max heap after MB", "maxHeapAfterMB", 0], ["mw", "mwAll", 0], ["user s", "user", 2], ["sys s", "sys", 2], ["vol cs", "vcs", 0]];
console.log("| arm | " + colsDef.map(c => c[0]).join(" | ") + " |");
console.log("|---|" + colsDef.map(() => "---|").join(""));
for (const arm of arms) console.log(`| ${arm.name} | ` + colsDef.map(c => f(arm, c[1], c[2], c[3] ?? 1)).join(" | ") + " |");
