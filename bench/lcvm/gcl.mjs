// Parse a BUN_JSC_logGC=1 log into collections and pause slices.
export function parseLog(log) {
  const cols = [];
  for (const m of log.matchAll(/START ([A-Z]) (\d+)kb => (\w+)Collection([\s\S]*?)END\]/g)) {
    const [, conn, , scope, body] = m;
    const slices = [];
    let pos = 0;
    const re = / p=([\d.]+)ms \(max [\d.]+\)(?:\.\.\.\]|, cycle ([\d.]+)ms )/g;
    let mm, cycle = NaN;
    while ((mm = re.exec(body))) {
      const text = body.slice(pos, mm.index);
      pos = re.lastIndex;
      const vs = [...text.matchAll(/v=(\d+)kb \(C:(\d+) M:(\d+)((?: P\d+:\d+)*)\)/g)].map(x => ({ v: +x[1], c: +x[2], m: +x[3], helpers: [...x[4].matchAll(/:(\d+)/g)].map(y => +y[1]) }));
      const stop = text.match(/: [A-Z] a=(\d+)kb hf=([\d.]+) mu=([\d.]+)/);
      slices.push({ p: +mm[1], vs, vFirst: vs.length ? vs[0].v : NaN, vLast: vs.length ? vs[vs.length - 1].v : NaN, stopA: stop ? +stop[1] : null, stopHf: stop ? +stop[2] : null });
      if (mm[2] !== undefined) cycle = +mm[2];
    }
    if (!slices.length) continue;
    const lastSlice = slices[slices.length - 1];
    const lastV = lastSlice.vs[lastSlice.vs.length - 1] ?? { v: 0, c: 0, m: 0, helpers: [] };
    const stops = slices.filter(s => s.stopHf !== null);
    const mw = body.match(/ mw=(\d+) /)?.[1];
    const after = body.match(/=> (\d+)kb, /)?.[1];
    cols.push({
      conn, scope, slices, cycle,
      markedKB: lastV.v, collectorKB: lastV.c, mutatorKB: lastV.m, helpersKB: lastV.helpers.reduce((a, b) => a + b, 0),
      helpersUsed: lastV.helpers.filter(k => k > 0).length,
      inPauseKB: slices.reduce((a, s) => a + (Number.isFinite(s.vLast - s.vFirst) ? s.vLast - s.vFirst : 0), 0),
      pauseTotal: slices.reduce((a, s) => a + s.p, 0), maxPause: Math.max(...slices.map(s => s.p)),
      resumes: slices.length - 1,
      lastStopHf: stops.length ? stops[stops.length - 1].stopHf : null, lastStopA: stops.length ? stops[stops.length - 1].stopA : null,
      mw: mw === undefined ? null : +mw, heapAfterKB: after ? +after : NaN,
    });
  }
  return cols;
}
export function summarize(cols, minKB = 20 * 1024, maxKB = Infinity) {
  const big = cols.filter(c => c.markedKB >= minKB && c.markedKB < maxKB);
  const sum = (xs, f) => xs.reduce((a, x) => a + f(x), 0);
  const marked = sum(big, c => c.markedKB);
  return {
    nAll: cols.length, n: big.length, markedMB: marked / 1024,
    inPauseShare: marked ? sum(big, c => c.inPauseKB) / marked : NaN,
    hfFull: big.filter(c => c.lastStopHf !== null && c.lastStopHf >= 0.98).length,
    hfFullShare: big.length ? big.filter(c => c.lastStopHf !== null && c.lastStopHf >= 0.98).length / big.length : NaN,
    helpersShare: marked ? sum(big, c => c.helpersKB) / marked : NaN,
    helpersUsedMean: big.length ? sum(big, c => c.helpersUsed) / big.length : NaN,
    maxPause: big.length ? Math.max(...big.map(c => c.maxPause)) : 0,
    pauseBig: sum(big, c => c.pauseTotal), pauseAll: sum(cols, c => c.pauseTotal), maxPauseAll: cols.length ? Math.max(...cols.map(c => c.maxPause)) : 0,
    cycleBig: sum(big, c => c.cycle),
    mw: sum(big, c => c.mw ?? 0), mwAll: sum(cols, c => c.mw ?? 0), allocDuringCycleMB: sum(big, c => c.lastStopA ?? 0) / 1024,
    maxHeapAfterMB: Math.max(0, ...cols.map(c => c.heapAfterKB).filter(Number.isFinite)) / 1024,
    pausePerMB: marked ? sum(big, c => c.pauseTotal) / (marked / 1024) : NaN,
    concPerAlloc: sum(big, c => c.lastStopA ?? 0) ? (marked - sum(big, c => c.inPauseKB)) / sum(big, c => c.lastStopA ?? 0) : NaN,
    resumes: sum(big, c => c.resumes),
  };
}
if (import.meta.main) {
  const { readFileSync } = await import("node:fs");
  const [file, min, max] = process.argv.slice(2);
  const cols = parseLog(readFileSync(file, "utf8"));
  const s = summarize(cols, min ? +min * 1024 : 20 * 1024, max ? +max * 1024 : Infinity);
  console.log(JSON.stringify(Object.fromEntries(Object.entries(s).map(([k, v]) => [k, typeof v === "number" ? +v.toFixed(3) : v]))));
  if (process.env.VERBOSE) for (const c of cols) console.log(`${c.scope} ${c.conn} marked=${(c.markedKB / 1024).toFixed(1)}MB inPause=${(c.inPauseKB / 1024).toFixed(1)}MB helpers=${(c.helpersKB / 1024).toFixed(1)}MB(${c.helpersUsed}) resumes=${c.resumes} lastHf=${c.lastStopHf} a=${c.lastStopA}kb pauses=[${c.slices.map(s => s.p.toFixed(1) + "ms:" + ((s.vLast - s.vFirst) / 1024).toFixed(1) + "MB").join(" ")}] cycle=${c.cycle.toFixed(1)} mw=${c.mw}`);
}
