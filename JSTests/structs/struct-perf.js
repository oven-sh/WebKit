// Hot loops over unshared structs: constructing instances, reading and writing fields, calling branded methods
// and accessors, and the error paths, so that the baseline, DFG and FTL tiers compile all of them. The results are
// checked against what plain JavaScript computes. It runs with the default options, with low JIT thresholds and
// with --useJIT=false (see run.mjs and the workflow).
function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }
function throws(fn, type, m) {
    try { fn(); } catch (e) { assert(e instanceof type, m + " threw " + e); return; }
    throw new Error("did not throw: " + m);
}
function report(name, fn) { print("# " + name + ": numberOfDFGCompiles=" + numberOfDFGCompiles(fn)); }

struct Vec {
    x;
    y;
    constructor(x, y) { this.x = x; this.y = y; }
    len2() { return this.x * this.x + this.y * this.y; }
    get sum() { return this.x + this.y; }
    static zero() { return new Vec(0, 0); }
}
struct Vec3 extends Vec {
    z;
    constructor(x, y, z) { super(x, y); this.z = z; }
    len2() { return super.len2() + this.z * this.z; }
}

// Construction: 1 million instances, read back immediately. The sum of 2i + 1 over i < n is n * n.
function build(n) {
    let total = 0;
    for (let i = 0; i < n; i++) {
        const v = new Vec(i, i + 1);
        total += v.x + v.y;
    }
    return total;
}
let built = 0;
for (let i = 0; i < 10; i++)
    built += build(100000);
assert(built === 10 * 100000 * 100000, "built " + built);
report("build", build);

function buildDerived(n) {
    let total = 0;
    for (let i = 0; i < n; i++) {
        const v = new Vec3(i, 1, 2);
        total += v.x + v.y + v.z;
    }
    return total;
}
let derivedTotal = 0;
for (let i = 0; i < 5; i++)
    derivedTotal += buildDerived(100000);
assert(derivedTotal === 5 * (100000 * 99999 / 2 + 100000 * 3), "derived " + derivedTotal);
report("buildDerived", buildDerived);

// Reads and writes of the fields of one instance: 2 million iterations.
const v = new Vec(1, 2);
function hotRead(v, n) {
    let sum = 0;
    for (let i = 0; i < n; i++)
        sum += v.x + v.y;
    return sum;
}
let read = 0;
for (let i = 0; i < 20; i++)
    read += hotRead(v, 100000);
assert(read === 20 * 100000 * 3, "reads " + read);
report("hotRead", hotRead);

function hotWrite(v, n) {
    for (let i = 0; i < n; i++) {
        v.x = v.x + 1;
        v.y = i;
    }
}
for (let i = 0; i < 20; i++)
    hotWrite(v, 100000);
assert(v.x === 1 + 20 * 100000 && v.y === 99999, "writes");
report("hotWrite", hotWrite);

// Branded methods and accessors: calls through the brand check, on a struct and a derived struct (super call).
function hotMethods(a, b, n) {
    let total = 0;
    for (let i = 0; i < n; i++)
        total += a.len2() + b.len2() + a.sum;
    return total;
}
const a = new Vec(3, 4);
const b = new Vec3(1, 2, 2);
let methods = 0;
for (let i = 0; i < 3; i++)
    methods += hotMethods(a, b, 100000);
assert(methods === 3 * 100000 * (25 + 9 + 7), "methods " + methods);
report("hotMethods", hotMethods);

// The brand check from compiled code: other receivers still throw.
const impostor = { x: 1, y: 2 };
const lookalike = Object.create(Vec.prototype);
function callLen2(receiver) { return Vec.prototype.len2.call(receiver); }
for (let i = 0; i < 20000; i++) {
    assert(callLen2(a) === 25, "a real receiver");
    if (i % 4 === 0) {
        throws(() => callLen2(impostor), TypeError, "a plain object");
        throws(() => callLen2(lookalike), TypeError, "an object that only inherits");
    }
}
report("callLen2", callLen2);

// A polymorphic read: struct, derived struct and a plain object share the property name.
function readX(o) { return o.x; }
let poly = 0;
const things = [new Vec(1, 0), new Vec3(2, 0, 0), { x: 3 }];
for (let i = 0; i < 300000; i++)
    poly += readX(things[i % 3]);
assert(poly === 100000 * 6, "polymorphic " + poly);
report("readX", readX);

// A struct is sealed: refused additions and deletions from compiled code change nothing.
function addField(o) { "use strict"; o.w = 1; }
function deleteField(o) { "use strict"; delete o.x; }
for (let i = 0; i < 3000; i++) {
    throws(() => addField(a), TypeError, "adding a field");
    throws(() => deleteField(a), TypeError, "deleting a field");
}
assert(a.w === undefined && a.x === 3 && Object.keys(a).join() === "x,y", "the instance is unchanged");
assert(Object.isSealed(a) && !Object.isExtensible(a), "still sealed");

// A static factory and the constructor in a loop that keeps the instances alive.
const kept = [];
for (let i = 0; i < 50000; i++)
    kept.push(i % 2 ? Vec.zero() : new Vec3(i, i, i));
let keptSum = 0;
for (const k of kept)
    keptSum += k.x;
assert(keptSum === 25000 * 24999, "kept " + keptSum);
assert(kept[1] instanceof Vec && !(kept[1] instanceof Vec3) && kept[2] instanceof Vec3, "instanceof after the loops");
