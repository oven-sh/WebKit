// Where a collection marks, from a BUN_JSC_logGC=1 log. usage: bun churn-phases.mjs <reps> "<script and args>" name=<exe>[,ENV=V...] ...
// Per run, over the collections that mark >= MIN_MB (default 20) MB:
//   pauseShareOfTime   = sum of pauses / sum of cycle times
//   markedInPauses     = share of the marked bytes that are marked while the mutator is stopped
//                        (v at the end of a stop minus v at its start; the end of a stop is the start of the next resume, so the last
//                        drain of a stop counts as outside: the numbers are a lower bound for "in pauses")
//   headroomUsedUp     = share of the collections whose last stop starts with hf >= 0.98
//   markedPerMsOutside = MB marked outside pauses / ms outside pauses; allocatedPerMsOutside = a at the last stop / ms outside pauses
const [repsArg, cmdline, ...specs] = process.argv.slice(2);
const reps = Number(repsArg);
const arms = specs.map(spec => { const eq = spec.indexOf("="); const [exe, ...envs] = spec.slice(eq + 1).split(","); const env = {}; for (const e of envs) { const i = e.indexOf("="); env[e.slice(0, i)] = e.slice(i + 1); } return { name: spec.slice(0, eq), exe, env, runs: [] }; });
export function analyze(log) {
  const cols = [];
  for (const [block] of log.matchAll(/START [A-Z] \d+kb => \w+Collection[\s\S]*?END\]/g)) {
    const stops = block.split("\n").filter(l => l.includes(" p="));
    let inPauses = 0, lastV = 0, pauseMs = 0, maxPause = 0, lastA = 0, lastHf = 0;
    for (const line of stops) {
      const vs = [...line.matchAll(/ v=(\d+)kb/g)].map(m => Number(m[1]));
      if (!vs.length) continue;
      inPauses += vs[vs.length - 1] - vs[0]; lastV = vs[vs.length - 1];
      const p = Number(line.match(/ p=([\d.]+)ms/)[1]); pauseMs += p; maxPause = Math.max(maxPause, p);
      const a = line.match(/ a=(\d+)kb hf=([\d.]+)/); if (a) { lastA = Number(a[1]); lastHf = Number(a[2]); }
    }
    const cycleMs = Number(block.match(/cycle ([\d.]+)ms END\]/)[1]);
    const mw = Number(block.match(/ mw=(\d+) /)?.[1] ?? 0);
    cols.push({ markedKB: lastV, inPausesKB: inPauses, pauseMs, maxPause, cycleMs, stops: stops.length, lastA, lastHf, mw });
  }
  return cols;
}
const sum = (xs, f) => xs.reduce((a, x) => a + f(x), 0);
for (let r = 0; r < reps; r++) for (const arm of r % 2 ? [...arms].reverse() : arms) {
  const p = Bun.spawn({ cmd: [arm.exe, ...cmdline.split(" ").filter(Boolean)], env: { PATH: process.env.PATH, BUN_JSC_logGC: "1", ...arm.env }, stdout: "pipe", stderr: "pipe" });
  const [out, err] = await Promise.all([p.stdout.text(), p.stderr.text()]); await p.exited;
  const j = JSON.parse((out + "\n" + err).split("\n").filter(l => l.startsWith('{"') && l.includes('"nsPerOp"')).pop());
  const ru = p.resourceUsage();
  const big = analyze(out + "\n" + err).filter(c => c.markedKB >= Number(process.env.MIN_MB ?? 20) * 1024);
  const outsideMs = sum(big, c => c.cycleMs - c.pauseMs), outsideKB = sum(big, c => c.markedKB - c.inPausesKB);
  arm.runs.push({ ns: j.nsPerOp, rss: Number(ru.maxRSS) / 1048576, vcs: Number(ru.contextSwitches.voluntary), n: big.length,
    pauseShareOfTime: 100 * sum(big, c => c.pauseMs) / sum(big, c => c.cycleMs), markedInPauses: 100 * sum(big, c => c.inPausesKB) / sum(big, c => c.markedKB),
    headroomUsedUp: 100 * big.filter(c => c.lastHf >= 0.98).length / big.length, pauseMs: sum(big, c => c.pauseMs), cycleMs: sum(big, c => c.cycleMs),
    maxPause: Math.max(0, ...big.map(c => c.maxPause)), markedMB: sum(big, c => c.markedKB) / 1024, markedPerMsOutside: outsideKB / 1024 / outsideMs, allocatedPerMsOutside: sum(big, c => c.lastA) / 1024 / outsideMs, mw: sum(big, c => c.mw) });
}
const med = xs => { const s = [...xs].sort((a, b) => a - b); const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };
const keys = ["ns", "rss", "vcs", "n", "pauseShareOfTime", "markedInPauses", "headroomUsedUp", "markedMB", "pauseMs", "cycleMs", "maxPause", "markedPerMsOutside", "allocatedPerMsOutside", "mw"];
console.log(`${cmdline}, ${reps} runs each, medians (min-max)`);
console.log("| arm | " + keys.join(" | ") + " |"); console.log("|" + "---|".repeat(keys.length + 1));
for (const arm of arms) console.log(`| ${arm.name} | ` + keys.map(k => { const c = arm.runs.map(x => x[k]); const d = k.endsWith("Outside") ? 2 : k === "ns" || k.endsWith("Share") || k === "markedInPauses" ? 1 : 0; return `${med(c).toFixed(d)} (${Math.min(...c).toFixed(d)}-${Math.max(...c).toFixed(d)})`; }).join(" | ") + " |");
