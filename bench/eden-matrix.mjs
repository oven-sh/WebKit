// usage: bun eden-matrix.mjs <reps> <S list> name=<exe>[,ENV=V...] ...
const [repsArg, sList, ...specs] = process.argv.slice(2);
const reps = Number(repsArg);
const arms = specs.map(spec => { const eq = spec.indexOf("="); const [exe, ...envs] = spec.slice(eq + 1).split(","); const env = {}; for (const e of envs) { const i = e.indexOf("="); env[e.slice(0, i)] = e.slice(i + 1); } return { name: spec.slice(0, eq), exe, env }; });
const med = xs => { const s = [...xs].sort((a, b) => a - b); return s[s.length >> 1]; };
const here = import.meta.dir;
for (const S of sList.split(",")) {
  const rows = new Map(arms.map(a => [a.name, []]));
  for (let r = 0; r < reps; r++) {
    for (const arm of r % 2 ? [...arms].reverse() : arms) {
      const proc = Bun.spawn({ cmd: [arm.exe, here + "/bench-eden.mjs", S, "60"], env: { PATH: process.env.PATH, BUN_JSC_logGC: "1", ...arm.env }, stdout: "pipe", stderr: "pipe" });
      const [out, err, code] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);
      if (code !== 0) { console.log(`${arm.name} exited ${code}`); continue; }
      const log = out + err;
      const j = JSON.parse(log.match(/^\{"S".*$/m)[0]);
      const ru = proc.resourceUsage();
      const cols = [...log.matchAll(/START ([A-Z]) \d+kb => (\w+)Collection[\s\S]*?END\]/g)].map(([block, , scope]) => {
        const v = [...block.matchAll(/v=(\d+)kb \(C:(\d+) M:(\d+)((?: P\d+:\d+)*)\)/g)]; const pauses = [...block.matchAll(/ p=([\d.]+)ms/g)].map(m => Number(m[1]));
        const last = v[v.length - 1]; const helpers = last ? [...last[4].matchAll(/:(\d+)/g)].map(([, kb]) => Number(kb)) : [];
        const mw = Number(block.match(/ mw=(\d+) /)?.[1] ?? 0);
        return { scope, markedKB: last ? Number(last[1]) : 0, pause: pauses.reduce((a, b) => a + b, 0), maxPause: Math.max(...pauses), helpersUsed: helpers.filter(kb => kb > 0).length, helpersKB: helpers.reduce((a, b) => a + b, 0), mw };
      });
      const big = cols.filter(c => c.markedKB >= Number(S) * 512);
      rows.get(arm.name).push({ ms: j.ms, totalPause: cols.reduce((s, c) => s + c.pause, 0), maxPause: Math.max(...cols.map(c => c.maxPause)), markedMB: cols.reduce((s, c) => s + c.markedKB, 0) / 1024, n: cols.length, sys: Number(ru.cpuTime.system) / 1e6, user: Number(ru.cpuTime.user) / 1e6, vcs: Number(ru.contextSwitches.voluntary), helpersUsed: big.length ? med(big.map(c => c.helpersUsed)) : 0, helperShare: cols.reduce((s, c) => s + c.helpersKB, 0) / Math.max(1, cols.reduce((s, c) => s + c.markedKB, 0)), mw: cols.reduce((s, c) => s + c.mw, 0), pausePerMB: cols.reduce((s, c) => s + c.pause, 0) / Math.max(1e-9, cols.reduce((s, c) => s + c.markedKB, 0) / 1024) });
    }
  }
  console.log(`S=${S} MB per eden batch, ${reps} runs each (medians)`);
  console.log("| arm | wall ms | total pause ms | max pause ms | collections | MB marked | pause ms/MB | helpers used | helper share | mw | user s | sys s | vol cs |");
  console.log("|---|---|---|---|---|---|---|---|---|---|---|---|---|");
  for (const arm of arms) { const xs = rows.get(arm.name); if (!xs.length) continue; const m = k => med(xs.map(x => x[k])); console.log(`| ${arm.name} | ${m("ms").toFixed(0)} | ${m("totalPause").toFixed(0)} | ${m("maxPause").toFixed(1)} | ${m("n")} | ${m("markedMB").toFixed(0)} | ${m("pausePerMB").toFixed(2)} | ${m("helpersUsed")} | ${m("helperShare").toFixed(2)} | ${m("mw")} | ${m("user").toFixed(2)} | ${m("sys").toFixed(2)} | ${m("vcs")} |`); }
}
