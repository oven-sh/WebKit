// Summarize a BUN_JSC_logGC=1 log. usage: bun gclog.mjs <logfile>
import { readFileSync } from "node:fs";
const log = readFileSync(process.argv[2], "utf8");
const rows = [...log.matchAll(/START ([A-Z]) \d+kb => (\w+)Collection[\s\S]*?END\]/g)].map(([block, conn, scope]) => {
  const v = [...block.matchAll(/v=(\d+)kb \(C:(\d+) M:(\d+)((?: P\d+:\d+)*)\)/g)];
  const last = v[v.length - 1];
  const helpersKB = last ? [...last[4].matchAll(/:(\d+)/g)].reduce((s, [, kb]) => s + Number(kb), 0) : 0;
  const pauses = [...block.matchAll(/ p=([\d.]+)ms/g)].map(m => Number(m[1]));
  const mw = block.match(/ mw=(\d+) /)?.[1];
  return { conn, scope, markedKB: last ? Number(last[1]) : 0, helpersKB, pauseTotal: pauses.reduce((a, b) => a + b, 0), maxPause: Math.max(0, ...pauses), resumes: pauses.length - 1, cycle: Number(block.match(/cycle ([\d.]+)ms END\]/)?.[1] ?? NaN), mw: mw === undefined ? 0 : Number(mw) };
});
const sum = (xs, f) => xs.reduce((a, x) => a + f(x), 0);
const q = (xs, p) => { const s = [...xs].sort((a, b) => a - b); return s[Math.min(s.length - 1, Math.floor(p * s.length))]; };
for (const scope of ["Eden", "Full"]) {
  const xs = rows.filter(r => r.scope === scope);
  if (!xs.length) continue;
  const n = xs.length;
  console.log(JSON.stringify({ scope, collections: n, markedMB_p10_p50_p90_max: [0.1, 0.5, 0.9, 1].map(p => +(q(xs.map(r => r.markedKB), p) / 1024).toFixed(1)), withHelpers: xs.filter(r => r.helpersKB > 0).length, helperShare: +(sum(xs, r => r.helpersKB) / Math.max(1, sum(xs, r => r.markedKB))).toFixed(2), meanPauseMs: +(sum(xs, r => r.pauseTotal) / n).toFixed(3), maxPauseMs: +Math.max(...xs.map(r => r.maxPause)).toFixed(2), totalPauseMs: +sum(xs, r => r.pauseTotal).toFixed(1), meanCycleMs: +(sum(xs, r => r.cycle) / n).toFixed(3), resumes: sum(xs, r => r.resumes), wakeups: sum(xs, r => r.mw), maxWakeups: Math.max(...xs.map(r => r.mw)), conn: Object.fromEntries(["M", "C"].map(c => [c, xs.filter(r => r.conn === c).length])) }));
}
