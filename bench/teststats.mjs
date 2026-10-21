// What the bun test measures: collections, voluntary context switches per collection, wake-ups. usage: bun teststats.mjs <exe> <rounds> <capacity> [markers]
const [exe, rounds, capacity, markers] = process.argv.slice(2);
const proc = Bun.spawn({ cmd: [exe, import.meta.dir + "/lru-gc.mjs", rounds, capacity], env: { PATH: process.env.PATH, BUN_JSC_logGC: "1", BUN_JSC_numberOfGCMarkers: markers ?? "8", BUN_GC_TIMER_DISABLE: "1" }, stdout: "pipe", stderr: "pipe" });
const [out, err] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);
const log = out + err;
const collections = [...log.matchAll(/START [A-Z] \d+kb => (\w+)Collection[\s\S]*?END\]/g)].map(([block, scope]) => {
  const lines = [...block.matchAll(/v=(\d+)kb \(C:(\d+) M:\d+((?: P\d+:\d+)+)\)/g)];
  const [, markedKB] = lines[lines.length - 1];
  return { scope, markedKB: Number(markedKB), mw: Number(block.match(/ mw=(\d+) /)?.[1] ?? NaN) };
});
const vcs = Number(proc.resourceUsage().contextSwitches.voluntary);
const mb = collections.reduce((s, c) => s + c.markedKB, 0) / 1024;
console.log(JSON.stringify({ exe: exe.split("/").at(-2), rounds: +rounds, capacity: +capacity, collections: collections.length, over2MB: collections.filter(c => c.markedKB >= 2048).length, markedMB: +mb.toFixed(0), vcs, vcsPerCollection: +(vcs / collections.length).toFixed(1), wakeups: collections.reduce((s, c) => s + c.mw, 0), maxWakeupsOverMBPlus: Math.max(...collections.map(c => c.mw - c.markedKB / 1024)).toFixed(1) }));
