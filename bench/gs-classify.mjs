// Classify gdb samples of the JS thread. usage: bun gs-classify.mjs <file>...
import { readFileSync } from "node:fs";
for (const f of process.argv.slice(2)) {
  const rows = readFileSync(f, "utf8").split("\n").filter(Boolean);
  const c = {};
  const add = k => c[k] = (c[k] ?? 0) + 1;
  for (const r of rows) {
    const blocked = /futex|lockSlow|parkConditionally|sched_yield|__lll_lock/.test(r.split("|").slice(0, 14).join("|"));
    const phase = /runConcurrentPhase/.test(r) ? "concurrent phase poll (allocation slow path)" : /runFixpointPhase/.test(r) ? "fixpoint phase (pause)" : /runEndPhase/.test(r) ? "end phase (pause)" : /runBeginPhase/.test(r) ? "begin phase (pause)" : /Collector::|collectInMutatorThread|stopIfNecessary/.test(r) ? "collector, other" : "mutator";
    add(phase + (blocked ? ", in a lock or a futex" : ""));
  }
  console.log(f, rows.length, "samples");
  for (const [k, v] of Object.entries(c).sort((a, b) => b[1] - a[1])) console.log("  ", String(v).padStart(4), (100 * v / rows.length).toFixed(0).padStart(3) + "%", k);
}
