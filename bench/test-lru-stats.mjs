// usage: bun test-lru-stats.mjs <reps> <capacity> <rounds> name=<exe> ...
const [reps, capacity, rounds, ...specs] = process.argv.slice(2);
for (const spec of specs) {
  const [name, exe] = spec.split("=");
  const rows = [];
  for (let r = 0; r < Number(reps); r++) {
    const t0 = performance.now();
    const p = Bun.spawn({ cmd: [exe, "/tmp/repro/test-lru.mjs", capacity, rounds], env: { PATH: process.env.PATH, BUN_JSC_logGC: "1", BUN_JSC_numberOfGCMarkers: "8", BUN_GC_TIMER_DISABLE: "1" }, stdout: "pipe", stderr: "pipe" });
    const [out, err] = await Promise.all([p.stdout.text(), p.stderr.text()]); await p.exited;
    const log = out + err;
    const cols = [...log.matchAll(/START [A-Z] \d+kb => (\w+)Collection[\s\S]*?END\]/g)].map(([block]) => { const v = [...block.matchAll(/v=(\d+)kb/g)]; return { markedKB: Number(v[v.length - 1][1]), mw: Number(block.match(/ mw=(\d+) /)?.[1] ?? NaN) }; });
    const vcs = Number(p.resourceUsage().contextSwitches.voluntary);
    rows.push({ ms: Math.round(performance.now() - t0), n: cols.length, over2MB: cols.filter(c => c.markedKB >= 2048).length, perCollection: +(vcs / cols.length).toFixed(1), mw: cols.reduce((s, c) => s + c.mw, 0), markedMB: +(cols.reduce((s, c) => s + c.markedKB, 0) / 1024).toFixed(0), maxMwMinusMB: +Math.max(...cols.map(c => c.mw - c.markedKB / 1024)).toFixed(1) });
  }
  const r = k => { const v = rows.map(x => x[k]).sort((a, b) => a - b); return v[0] + ".." + v[v.length - 1]; };
  console.log(name, capacity, rounds, "ms", r("ms"), "collections", r("n"), "over2MB", r("over2MB"), "switches/collection", r("perCollection"), "mw", r("mw"), "markedMB", r("markedMB"), "max(mw-MB)", r("maxMwMinusMB"));
}
