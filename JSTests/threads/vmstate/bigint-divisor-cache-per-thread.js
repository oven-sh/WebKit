//@ requireOptions("--useJSThreads=1")
// BigInt % keeps a cache on the VM for a divisor that is used many times in a
// row: the divisor, and a fold factor or an inverse computed from it. Two
// threads each reduce by their own divisor, of a different length, at the
// same time. With one shared cache they take turns arming it, and each re-arm
// rewrites the inverse the other thread may be reading.
//
// The cache arms after 100 uses of one divisor in a row, so two threads that
// start together keep resetting each other's count and never arm it. Thread 0
// therefore starts first, and thread 1 joins it a little later. The waits are
// on the clock, not on the other thread, so GIL-on the threads just run one
// after the other.
load("../harness.js", "caller relative");

const THREADS = 2;
const MIN_ITERATIONS = 500;
const STAGGER_MS = 30;
const RUN_MS = 400;

// Neither divisor is close to a power of two, so both take the inverse path.
const divisors = [
    (0x1234567n << 128n) + 0xfedcba9876543210n,
    (0x7654321n << 256n) + (0x0123456789abcdefn << 64n) + 0x55n,
];

function dividends(t) {
    const d = divisors[t];
    const out = [];
    for (let i = 1n; i <= 16n; ++i)
        out.push(d * (d - i * 977n) + i * 0x9e3779b97f4a7c15n);
    return out;
}

// Computed before any thread starts. The divisor object here is a copy, so
// this does not arm the cache with the object the threads use.
const inputs = [];
const expected = [];
for (let t = 0; t < THREADS; ++t) {
    inputs.push(dividends(t));
    const d = BigInt(String(divisors[t]));
    expected.push(inputs[t].map(x => x % d));
}

let startAt = 0;
let endAt = 0;

function work(t) {
    // One BigInt object, reused, so the cache can match it by identity.
    const d = divisors[t];
    const xs = inputs[t];
    const want = expected[t];
    while (Date.now() < startAt + t * STAGGER_MS) { }
    for (let i = 0; i < MIN_ITERATIONS || Date.now() < endAt; ++i) {
        const k = i % xs.length;
        const r = xs[k] % d;
        if (r !== want[k])
            return "thread " + t + " iteration " + i + ": " + r.toString(16);
    }
    return null;
}

startAt = Date.now() + 150;
endAt = startAt + RUN_MS;
for (const r of joinAll(spawnN(THREADS, work)))
    shouldBe(r, null);
shouldBe(work(0), null);
