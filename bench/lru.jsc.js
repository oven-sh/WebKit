const CAPACITY = 4096, KEYS = 30000, OPS = 65536, ROUNDS = 40;
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
const t0 = Date.now();
for (let r = 0; r < ROUNDS; r++) hits = round();
print("done", hits, Date.now() - t0, "ms");
