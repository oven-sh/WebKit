// N workers, each churns the LRU of the issue for R rounds at the same time. usage: bun workers-lru.mjs <workers> <rounds>
import { Worker, isMainThread, parentPort, workerData } from "node:worker_threads";
const [nWorkers, rounds] = [Number(process.argv[2] ?? 4), Number(process.argv[3] ?? 50)];
function churn(rounds) {
  const CAPACITY = 4096, KEYS = 30000, OPS = 65536;
  let seed = 12345;
  const rand = () => (seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296;
  const stream = new Uint32Array(OPS);
  for (let i = 0; i < OPS; i++) stream[i] = Math.floor(KEYS * rand() ** 3);
  function round() {
    const map = new Map();
    const head = { prev: null, next: null }, tail = { prev: head, next: null };
    head.next = tail;
    let hits = 0;
    for (let i = 0; i < OPS; i++) {
      const key = stream[i];
      let node = map.get(key);
      if (node) { hits++; node.prev.next = node.next; node.next.prev = node.prev; }
      else {
        if (map.size === CAPACITY) { const old = tail.prev; old.prev.next = tail; tail.prev = old.prev; map.delete(old.key); }
        node = { key, value: key, prev: null, next: null };
        map.set(key, node);
      }
      node.next = head.next; node.prev = head; head.next.prev = node; head.next = node;
    }
    return hits;
  }
  let hits = 0;
  for (let r = 0; r < rounds; r++) hits = round();
  return hits;
}
if (isMainThread) {
  const t0 = performance.now();
  const results = await Promise.all(Array.from({ length: nWorkers }, () => new Promise((resolve, reject) => { const w = new Worker(new URL(import.meta.url), { workerData: { rounds } }); w.on("message", resolve); w.on("error", reject); })));
  const ms = performance.now() - t0;
  console.log(JSON.stringify({ workers: nWorkers, rounds, hits: results[0], nsPerOp: +(ms * 1e6 / (rounds * 65536)).toFixed(1), wallS: +(ms / 1000).toFixed(2) }));
  process.exit(0);
} else {
  parentPort.postMessage(churn(workerData.rounds));
}
