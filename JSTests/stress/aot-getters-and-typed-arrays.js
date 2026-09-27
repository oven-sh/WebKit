//@ requireOptions("--useAOT=1")
// Getters and typed arrays, which code from the static compiler deals with in its stubs. What is expected is what the interpreter says.

function check(actual, expected) {
    if (actual !== expected)
        throw new Error("bad result:\n" + actual + "\nexpected:\n" + expected);
}

function read(o) { return o.v; }
function readLoop(o, n) { let s = 0; for (let i = 0; i < n; i++) s += o.v; return s; }
function size(s) { return s.size; }
let out = [];
class A { constructor(x) { this.x = x; } get v() { return this.x * 2; } }
class B extends A { }
const own = { x: 5, get v() { return this.x + 1; } };
for (let i = 0; i < 6; i++) out.push(read(new A(i)), read(new B(i)), read(own), readLoop(new A(i), 4), readLoop(own, 3));
out.push(size(new Set([1, 2, 3])), size(new Set()), size(new Map([[1, 2]])), size(new Set([1])), size(new Set([1, 2])));
// A getter that takes parameters it is not given.
const params = { get v() { return arguments.length; } };
Object.defineProperty(params, "w", { get: function (a, b, c) { return [a, b, c, arguments.length].join(); }, configurable: true });
function readW(o) { return o.w; }
for (let i = 0; i < 5; i++) out.push(read(params), readW(params));
// Throwing, and caught in the caller.
const thrower = { get v() { throw new Error("boom " + this.tag); }, tag: "t" };
function tryRead(o) { try { return o.v; } catch (e) { return e.message; } }
for (let i = 0; i < 5; i++) out.push(tryRead(thrower));
// Replaced by another getter, by a value, by no getter.
const changing = { get v() { return "first"; } };
for (let i = 0; i < 4; i++) out.push(read(changing));
Object.defineProperty(changing, "v", { get() { return "second"; }, configurable: true });
for (let i = 0; i < 4; i++) out.push(read(changing));
Object.defineProperty(changing, "v", { get: undefined, set(x) { }, configurable: true });
for (let i = 0; i < 4; i++) out.push(read(changing));
Object.defineProperty(changing, "v", { value: "plain", configurable: true });
for (let i = 0; i < 4; i++) out.push(read(changing));
// On the prototype, then shadowed and changed.
class P { get v() { return "proto"; } }
const p = new P();
for (let i = 0; i < 4; i++) out.push(read(p));
Object.defineProperty(P.prototype, "v", { get() { return "proto2"; }, configurable: true });
for (let i = 0; i < 4; i++) out.push(read(p));
Object.defineProperty(p, "v", { value: "own now" });
for (let i = 0; i < 4; i++) out.push(read(p));
// this is a primitive, strict and sloppy.
Object.defineProperty(String.prototype, "v", { get() { "use strict"; return typeof this; }, configurable: true });
Object.defineProperty(Number.prototype, "v", { get() { return typeof this; }, configurable: true });
for (let i = 0; i < 4; i++) out.push(read("s"), read(1));
// Bound and proxied getters, a getter that recurses, one that allocates a lot.
Object.defineProperty(own, "b", { get: function () { return this; }.bind("bound") });
function readB(o) { return o.b; }
for (let i = 0; i < 4; i++) out.push(String(readB(own)));
const deep = { n: 0, get v() { return this.n++ < 50 ? this.v + 1 : 0; } };
for (let i = 0; i < 3; i++) { deep.n = 0; out.push(read(deep)); }
const alloc = { get v() { let a = []; for (let i = 0; i < 2000; i++) a.push({ i }); return a.length; } };
for (let i = 0; i < 30; i++) out.push(read(alloc));
// length through a getter.
const len = { get length() { return 77; } };
function length(o) { return o.length; }
for (let i = 0; i < 4; i++) out.push(length(len), length([1, 2]), length("abc"));
check(out.join(" "), "0 0 6 0 18 2 2 6 8 18 4 4 6 16 18 6 6 6 24 18 8 8 6 32 18 10 10 6 40 18 3 0 1 1 2 0 ,,,0 0 ,,,0 0 ,,,0 0 ,,,0 0 ,,,0 boom t boom t boom t boom t boom t first first first first second second second second     plain plain plain plain proto proto proto proto proto2 proto2 proto2 proto2 own now own now own now own now string object string object string object string object bound bound bound bound 50 50 50 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 2000 77 2 3 77 2 3 77 2 3 77 2 3");

// Typed arrays.
function get(a, i) { return a[i]; }
function put(a, i, v) { a[i] = v; }
function sum(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function fill(a, v) { for (let i = 0; i < a.length; i++) a[i] = v + i; }
let t = [];
for (const T of [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array, Float32Array, Float64Array, Float16Array, BigInt64Array]) {
    const big = T === BigInt64Array;
    const a = new T(6);
    for (const v of [1, -1, 300, 70000, 4294967295, 2147483648, 1.5, -0, NaN, Infinity, 1e20]) {
        try { put(a, 1, big ? BigInt(Math.trunc(Number.isFinite(v) ? v : 0)) : v); } catch (e) { t.push("E"); }
        t.push(String(get(a, 1)), Object.is(get(a, 1), -0));
    }
    t.push(get(a, 6), get(a, -1), get(a, 1.5), get(a, "1") === get(a, 1));
    put(a, 6, big ? 1n : 1); put(a, -1, big ? 1n : 1);
    if (!big) { fill(a, 250.5); t.push(sum(a)); put(a, 2, "7"); put(a, 3, true); put(a, 4, undefined); put(a, 5, null); t.push(Array.from(a).join()); }
}
const buffer = new ArrayBuffer(16, { maxByteLength: 64 });
const tracking = new Int32Array(buffer);
for (let i = 0; i < 4; i++) put(tracking, i, i + 1);
t.push(sum(tracking), get(tracking, 3), get(tracking, 4));
buffer.resize(32); t.push(tracking.length, get(tracking, 4), get(tracking, 7), get(tracking, 8));
buffer.resize(8); t.push(tracking.length, get(tracking, 1), get(tracking, 2), sum(tracking));
const gone = new Uint8Array(8); put(gone, 0, 9); t.push(get(gone, 0));
transferArrayBuffer(gone.buffer); t.push(get(gone, 0), gone.length); put(gone, 0, 1); t.push(get(gone, 0));
const doubles = [1.5, 2.5, , 4.5]; t.push(get(doubles, 0), get(doubles, 2), get(doubles, 3), get(doubles, 4), sum([0.5, 1.5, 2]));
const view = new Int32Array(new ArrayBuffer(32), 8, 3); fill(view, 10); t.push(sum(view), get(view, 2), get(view, 3));
check(t.join(" "), "1 false -1 false 44 false 112 false -1 false 0 false 1 false 0 false 0 false 0 false 0 false    true -21 -6,-5,7,1,0,0 1 false 255 false 44 false 112 false 255 false 0 false 1 false 0 false 0 false 0 false 0 false    true 1515 250,251,7,1,0,0 1 false 0 false 255 false 255 false 255 false 255 false 2 false 0 false 0 false 255 false 255 false    true 1517 250,252,7,1,0,0 1 false -1 false 300 false 4464 false -1 false 0 false 1 false 0 false 0 false 0 false 0 false    true 1515 250,251,7,1,0,0 1 false 65535 false 300 false 4464 false 65535 false 0 false 1 false 0 false 0 false 0 false 0 false    true 1515 250,251,7,1,0,0 1 false -1 false 300 false 70000 false -1 false -2147483648 false 1 false 0 false 0 false 0 false 1661992960 false    true 1515 250,251,7,1,0,0 1 false 4294967295 false 300 false 70000 false 4294967295 false 2147483648 false 1 false 0 false 0 false 0 false 1661992960 false    true 1515 250,251,7,1,0,0 1 false -1 false 300 false 70000 false 4294967296 false 2147483648 false 1.5 false 0 true NaN false Infinity false 100000002004087730000 false    true 1518 250.5,251.5,7,1,NaN,0 1 false -1 false 300 false 70000 false 4294967295 false 2147483648 false 1.5 false 0 true NaN false Infinity false 100000000000000000000 false    true 1518 250.5,251.5,7,1,NaN,0 1 false -1 false 300 false Infinity false Infinity false Infinity false 1.5 false 0 true NaN false Infinity false Infinity false    true 1518 250.5,251.5,7,1,NaN,0 1 false -1 false 300 false 70000 false 4294967295 false 2147483648 false 1 false 0 false 0 false 0 false 7766279631452241920 false    true 10 4  8 0 0  2 2  3 9  0  1.5  4.5  4 33 12 ");

// Getters that replace themselves with what they computed, while they are being called.
(function () {
    function defineLazy(object, key, getter) {
        Object.defineProperty(object, key, {
            get() { const value = getter(); object[key] = value; return value; },
            set(v) { Object.defineProperty(object, key, { value: v }); },
            configurable: true,
        });
    }
    function read(o) { return o.optin; }
    noInline(read);
    let out = [];
    for (let i = 0; i < 8; i++) {
        const o = {};
        defineLazy(o, "optin", () => true);
        out.push(read(o));
        out.push(read(o), read(o));
    }
    check(out.join(), "true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true,true");
    // The same on a prototype: the getter replaces itself there.
    function P() { }
    function makeProto() {
        const proto = {};
        Object.defineProperty(proto, "lazy", { get() { Object.defineProperty(proto, "lazy", { value: "now a value", configurable: true }); return "from the getter"; }, configurable: true });
        return proto;
    }
    function readLazy(o) { return o.lazy; }
    noInline(readLazy);
    let out2 = [];
    for (let i = 0; i < 6; i++) {
        const o = Object.create(makeProto());
        for (let j = 0; j < 4; j++) out2.push(readLazy(o));
    }
    // One prototype, many reads, then it replaces itself.
    const shared = {}; let reads = 0;
    Object.defineProperty(shared, "lazy", { get() { if (++reads == 10) Object.defineProperty(shared, "lazy", { value: "settled" }); return reads; }, configurable: true });
    const child = Object.create(shared);
    for (let j = 0; j < 16; j++) out2.push(readLazy(child));
    check(out2.join(), "from the getter,now a value,now a value,now a value,from the getter,now a value,now a value,now a value,from the getter,now a value,now a value,now a value,from the getter,now a value,now a value,now a value,from the getter,now a value,now a value,now a value,from the getter,now a value,now a value,now a value,1,2,3,4,5,6,7,8,9,10,settled,settled,settled,settled,settled,settled");
})();
