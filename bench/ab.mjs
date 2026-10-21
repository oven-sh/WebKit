// Interleaved A/B harness. usage: bun ab.mjs <rounds> <script> "<script args>" name=<exe>[,ENV=VALUE...] ...
import { readFileSync } from "node:fs";
const [roundsArg, script, scriptArg, ...armSpecs] = process.argv.slice(2);
const rounds = Number(roundsArg);
const arms = armSpecs.map(spec => {
  const eq = spec.indexOf("=");
  const [exe, ...envs] = spec.slice(eq + 1).split(",");
  const env = {};
  for (const e of envs) { const i = e.indexOf("="); env[e.slice(0, i)] = e.slice(i + 1); }
  return { name: spec.slice(0, eq), exe, env, runs: [] };
});
const baseEnv = { PATH: process.env.PATH, HOME: process.env.HOME };
const load = () => readFileSync("/proc/loadavg", "utf8").split(" ")[0];
const loadStart = load();
for (let r = 0; r < rounds; r++) {
  for (const arm of r % 2 === 0 ? arms : [...arms].reverse()) {
    const proc = Bun.spawn({ cmd: [arm.exe, script, ...scriptArg.split(" ").filter(Boolean)], env: { ...baseEnv, ...arm.env }, stdout: "pipe", stderr: "pipe" });
    const [out, err, code] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);
    if (code !== 0) { console.log(`${arm.name} exited ${code}: ${err.slice(0, 500)}`); arm.failed = (arm.failed ?? 0) + 1; continue; }
    const ru = proc.resourceUsage();
    const j = JSON.parse(out.trim().split("\n").pop());
    arm.runs.push({ ns: j.nsPerOp, hits: j.hits, user: Number(ru.cpuTime.user) / 1e6, sys: Number(ru.cpuTime.system) / 1e6, vcs: Number(ru.contextSwitches.voluntary), ics: Number(ru.contextSwitches.involuntary), rssMB: Number(ru.maxRSS) / 1048576 });
  }
}
const med = xs => { const s = [...xs].sort((a, b) => a - b); const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };
const fmt = (xs, d) => xs.length ? `${med(xs).toFixed(d)} (${Math.min(...xs).toFixed(d)}-${Math.max(...xs).toFixed(d)})` : "-";
console.log(`rounds=${rounds} script=${script} arg=${scriptArg} load ${loadStart} -> ${load()}`);
console.log("| arm | ns/op | system CPU s | user CPU s | voluntary ctx switches | involuntary | peak RSS MB | hits | failed |");
console.log("|---|---|---|---|---|---|---|---|---|");
for (const arm of arms) { const col = k => arm.runs.map(x => x[k]); console.log(`| ${arm.name} | ${fmt(col("ns"), 1)} | ${fmt(col("sys"), 2)} | ${fmt(col("user"), 2)} | ${fmt(col("vcs"), 0)} | ${fmt(col("ics"), 0)} | ${fmt(col("rssMB"), 0)} | ${[...new Set(col("hits"))].join(",")} | ${arm.failed ?? 0} |`); }
