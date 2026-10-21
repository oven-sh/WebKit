// usage: bun app-matrix.mjs <script> <reps> "<script args>" name=<exe>[,ENV=V...] ...   (script prints one JSON line with nsPerOp)
const [script, repsArg, sargs, ...specs] = process.argv.slice(2);
const reps = Number(repsArg);
const arms = specs.map(spec => { const eq = spec.indexOf("="); const [exe, ...envs] = spec.slice(eq + 1).split(","); const env = {}; for (const e of envs) { const i = e.indexOf("="); env[e.slice(0, i)] = e.slice(i + 1); } return { name: spec.slice(0, eq), exe, env, runs: [] }; });
for (let r = 0; r < reps; r++) for (const arm of r % 2 ? [...arms].reverse() : arms) {
  const p = Bun.spawn({ cmd: [arm.exe, script, ...sargs.split(" ").filter(Boolean)], env: { PATH: process.env.PATH, ...arm.env }, stdout: "pipe", stderr: "pipe" });
  const out = await p.stdout.text(); await p.exited;
  const j = JSON.parse(out.trim().split("\n").pop()); const ru = p.resourceUsage();
  arm.runs.push({ ns: j.nsPerOp, sys: Number(ru.cpuTime.system) / 1e6, user: Number(ru.cpuTime.user) / 1e6, vcs: Number(ru.contextSwitches.voluntary), rss: Number(ru.maxRSS) / 1048576 });
}
const med = xs => { const s = [...xs].sort((a, b) => a - b); const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };
console.log(`${script} ${sargs}, ${reps} runs each`);
console.log("| arm | ns/op | system CPU s | user CPU s | voluntary ctx switches | peak RSS MB |");
console.log("|---|---|---|---|---|---|");
for (const arm of arms) { const c = k => arm.runs.map(x => x[k]); console.log(`| ${arm.name} | ${med(c("ns")).toFixed(1)} (${Math.min(...c("ns")).toFixed(1)}-${Math.max(...c("ns")).toFixed(1)}) | ${med(c("sys")).toFixed(2)} | ${med(c("user")).toFixed(2)} | ${med(c("vcs")).toFixed(0)} | ${med(c("rss")).toFixed(0)} (${Math.min(...c("rss")).toFixed(0)}-${Math.max(...c("rss")).toFixed(0)}) |`); }
