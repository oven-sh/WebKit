// Single-threaded LRU churn: a Map from integer key to a small node object,
// nodes linked in a doubly linked list, a fresh cache every round. (From oven-sh/bun#44893.)
// usage: lru-gc.mjs [rounds] [capacity]   KEYS and OPS scale with the capacity, so the miss ratio stays that of the issue's script.
const ROUNDS = Number(process.argv[2] ?? 300), CAPACITY = Number(process.argv[3] ?? 4096);
const SCALE = CAPACITY / 4096, KEYS = 30000 * SCALE, OPS = 65536 * SCALE;

let seed = 12345;
const rand = () => (seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296;
const stream = new Uint32Array(OPS);
for (let i = 0; i < OPS; i++) stream[i] = Math.floor(KEYS * rand() ** 3); // skewed toward low keys

function round() {
  const map = new Map();
  const head = { prev: null, next: null }, tail = { prev: head, next: null };
  head.next = tail;
  let hits = 0;
  for (let i = 0; i < OPS; i++) {
    const key = stream[i];
    let node = map.get(key);
    if (node) {
      hits++;
      node.prev.next = node.next; node.next.prev = node.prev;
    } else {
      if (map.size === CAPACITY) {
        const old = tail.prev;
        old.prev.next = tail; tail.prev = old.prev;
        map.delete(old.key);
      }
      node = { key, value: key, prev: null, next: null };
      map.set(key, node);
    }
    node.next = head.next; node.prev = head;
    head.next.prev = node; head.next = node;
  }
  return hits;
}

round();
const cpu0 = process.cpuUsage(), t0 = performance.now();
let hits = 0;
for (let r = 0; r < ROUNDS; r++) hits = round();
const ms = performance.now() - t0, cpu = process.cpuUsage(cpu0);
const rt = typeof Bun !== 'undefined' ? `bun ${Bun.version}` : `node ${process.version}`;
console.log(JSON.stringify({ runtime: rt, gcMarkers: process.env.BUN_JSC_numberOfGCMarkers ?? 'default',
  hits, nsPerOp: +(ms * 1e6 / (ROUNDS * OPS)).toFixed(1), wallS: +(ms / 1000).toFixed(2),
  userS: +(cpu.user / 1e6).toFixed(2), systemS: +(cpu.system / 1e6).toFixed(2) }));
