// Hot loops over shared structs and SharedArrays, so that the baseline, DFG and FTL tiers compile the accesses.
// Field accesses are cached by the inline caches of every tier (the fields are properties of the object's
// structure with a native accessor); SharedArray elements go through the generic indexed path. The results are
// checked against what plain JavaScript computes. It runs with the default options, with low JIT thresholds and
// with --useJIT=false (see run.mjs and the workflow).
function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }
function throws(fn, type, m) {
    try { fn(); } catch (e) { assert(e instanceof type, m + " threw " + e); return; }
    throw new Error("did not throw: " + m);
}
function report(name, fn) { print("# " + name + ": numberOfDFGCompiles=" + numberOfDFGCompiles(fn)); }

shared struct Counter { a; b; c; label; next; }
shared struct Other { z; a; }
shared struct Wide extends Counter { d; e; }
shared struct Link { v; next; }

const N = 2000000;
const CHUNK = 100000;

// 2 million iterations of two field reads.
function hotRead(s, n) {
    let sum = 0;
    for (let i = 0; i < n; i++)
        sum += s.a + s.b;
    return sum;
}
const s = new Counter();
s.a = 1;
s.b = 2;
s.c = 3;
let total = 0;
for (let i = 0; i < N / CHUNK; i++)
    total += hotRead(s, CHUNK);
assert(total === N * 3, "hot reads " + total);
report("hotRead", hotRead);

// 2 million iterations of three field writes and a read back.
function hotWrite(s, n) {
    for (let i = 0; i < n; i++) {
        s.a = i;
        s.b = s.a + 1;
        s.c = i * 0.5;
    }
    return s.b;
}
let last = 0;
for (let i = 0; i < N / CHUNK; i++)
    last = hotWrite(s, CHUNK);
assert(last === CHUNK && s.a === CHUNK - 1 && s.b === CHUNK && s.c === (CHUNK - 1) * 0.5, "hot writes");
report("hotWrite", hotWrite);

// Reads through three different structures (a polymorphic cache), and a miss.
const shapes = [new Counter(), new Other(), new Wide()];
shapes[0].a = 10;
shapes[1].a = 20;
shapes[2].a = 30;
function readA(o) { return o.a; }
function readMissing(o) { return o.nothing; }
let poly = 0;
for (let i = 0; i < 300000; i++) {
    poly += readA(shapes[i % 3]);
    if (readMissing(shapes[i % 3]) !== undefined)
        throw new Error("a missing field read as " + readMissing(shapes[i % 3]));
}
assert(poly === 100000 * 60, "polymorphic reads " + poly);
assert("a" in shapes[0] && !("nothing" in shapes[0]), "in");
report("readA", readA);

// Every kind of value, written and read back in a hot loop.
const nested = new Link();
nested.v = 99;
function hotMixed(s, nested, n) {
    let ok = 0;
    for (let i = 0; i < n; i++) {
        s.label = (i & 1) ? "odd" : "even";
        if (s.label.length === ((i & 1) ? 3 : 4))
            ok++;
        s.next = nested;
        if (s.next === nested && s.next.v === 99)
            ok++;
        const kind = i % 4;
        s.c = kind === 0 ? null : kind === 1 ? true : kind === 2 ? 12345678901234567890n : 1.5;
        const c = s.c;
        if (kind === 0 ? c === null : kind === 1 ? c === true : kind === 2 ? c === 12345678901234567890n : c === 1.5)
            ok++;
    }
    return ok;
}
let mixed = 0;
for (let i = 0; i < 4; i++)
    mixed += hotMixed(s, nested, 50000);
assert(mixed === 4 * 50000 * 3, "mixed values " + mixed);
report("hotMixed", hotMixed);

// A write that is refused from compiled code stays refused and changes nothing.
function setA(s, v) { s.a = v; }
for (let i = 0; i < 200000; i++)
    setA(s, i);
throws(() => setA(s, {}), TypeError, "an ordinary object");
throws(() => setA(s, () => 1), TypeError, "a function");
assert(s.a === 199999, "refused writes change nothing");
setA(s, "text");
assert(s.a === "text", "a string after the numbers");
function addKey(s) { "use strict"; s.zzz = 1; }
function removeKey(s) { "use strict"; delete s.a; }
for (let i = 0; i < 3000; i++) {
    throws(() => addKey(s), TypeError, "adding a property");
    throws(() => removeKey(s), TypeError, "deleting a field");
}
assert(s.zzz === undefined && "a" in s && Object.keys(s).join() === "a,b,c,label,next", "the shape is fixed");
report("setA", setA);

// Atomics on fields from a hot loop: a compareExchange counter and store/load.
function hotAtomics(s, n) {
    for (let i = 0; i < n; i++) {
        let current;
        do {
            current = Atomics.load(s, "c");
        } while (Atomics.compareExchange(s, "c", current, current + 1) !== current);
        Atomics.store(s, "b", i);
    }
}
s.c = 0;
hotAtomics(s, 100000);
assert(s.c === 100000 && s.b === 99999, "atomics " + s.c + " " + s.b);
report("hotAtomics", hotAtomics);

// Walking a linked list of shared structs: every next is read through the wrapper cache.
let head = undefined;
for (let i = 999; i >= 0; i--) {
    const link = new Link();
    link.v = i;
    link.next = head;
    head = link;
}
function walk(head) {
    let sum = 0;
    for (let link = head; link; link = link.next)
        sum += link.v;
    return sum;
}
let walked = 0;
for (let i = 0; i < 500; i++)
    walked += walk(head);
assert(walked === 500 * (999 * 1000 / 2), "walked " + walked);
report("walk", walk);

// SharedArray elements: the same body on a SharedArray and on an Array must agree.
function hotArray(a, n) {
    let sum = 0;
    for (let i = 0; i < n; i++) {
        a[i & 15] = i;
        sum += a[(i + 1) & 15] ?? 0;
    }
    return sum;
}
function hotArrayPlain(a, n) {
    let sum = 0;
    for (let i = 0; i < n; i++) {
        a[i & 15] = i;
        sum += a[(i + 1) & 15] ?? 0;
    }
    return sum;
}
const shared = new SharedArray(16);
const plain = new Array(16).fill(undefined);
let sharedSum = 0;
let plainSum = 0;
for (let i = 0; i < N / CHUNK; i++) {
    sharedSum += hotArray(shared, CHUNK);
    plainSum += hotArrayPlain(plain, CHUNK);
}
assert(sharedSum === plainSum, "SharedArray sum " + sharedSum + " vs " + plainSum);
for (let i = 0; i < 16; i++)
    assert(shared[i] === plain[i], "element " + i);
assert(shared.length === 16, "length");
report("hotArray", hotArray);
