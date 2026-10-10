//@ runDefault("--compileMainScriptAheadOfTime=1")
function shouldBe(actual, expected) {
    if (actual !== expected && !(actual !== actual && expected !== expected))
        throw new Error("bad value: " + actual + ", expected " + expected);
}

function math(values) {
    let s = 0;
    for (let i = 0; i < values.length; i++) {
        const v = values[i];
        s += Math.sqrt(v) + Math.abs(v) + Math.floor(v) + Math.ceil(v) + Math.trunc(v) + Math.fround(v) + Math.min(v, 2) + Math.max(v, 3) + Math.imul(v, 3);
    }
    return s;
}
const numbers = [0, 1, 2.5, 4, 9.75, 100];
const expected = math(numbers);
for (let i = 0; i < 100; i++)
    shouldBe(math(numbers), expected);
shouldBe(math([-1]), NaN);
shouldBe(1 / [-0.5].map(v => { let r; for (let i = 0; i < 2; i++) r = Math.ceil(v); return r; })[0], -Infinity);
shouldBe(1 / [-0].map(v => { let r; for (let i = 0; i < 2; i++) r = Math.min(v, 0); return r; })[0], -Infinity);
shouldBe(1 / [-0].map(v => { let r; for (let i = 0; i < 2; i++) r = Math.max(v, 0); return r; })[0], Infinity);
shouldBe(math(["4"]), 2 + 4 + 4 + 4 + 4 + 4 + 2 + 4 + 12);
shouldBe(math([{ valueOf() { return 4; } }]), 2 + 4 + 4 + 4 + 4 + 4 + 2 + 4 + 12);

function walk(list) {
    let s = 0;
    for (let i = 0; i < list.length + 1; i++) {
        const o = list[i];
        if (!o)
            continue;
        s += o.a;
        o.b = s;
    }
    return s;
}
const list = [];
for (let i = 0; i < 20; i++)
    list.push({ a: i, b: 0 });
shouldBe(walk(list), 190);
shouldBe(walk(list), 190);
list[3] = { b: 0, a: 3 };
list[4] = { get a() { return 4; }, set b(v) { this.c = v; } };
list[5] = Object.create({ a: 5 });
list[6] = Object.freeze({ a: 6, b: 0 });
delete list[7];
shouldBe(walk(list), 183);
shouldBe(list[4].c, 10);
shouldBe(list[5].b, 15);
shouldBe(list[6].b, 0);

function fill(array, n, value) {
    for (let i = 0; i < n; i++)
        array[i] = value;
    let s = 0;
    for (let i = 0; i < n; i++)
        s += array[i];
    return s;
}
shouldBe(fill([], 100, 1), 100);
shouldBe(fill([], 100, 1.5), 150);
shouldBe(fill([1.5], 10, NaN), NaN);
shouldBe(fill([1.5], 10, "a"), "0aaaaaaaaaa");
shouldBe(fill(new Int8Array(10), 10, 300), 440);
shouldBe(fill(new Uint8Array(10), 10, -1), 2550);
shouldBe(fill(new Uint8ClampedArray(10), 10, 300), 2550);
shouldBe(fill(new Int16Array(10), 10, 70000), 44640);
shouldBe(fill(new Uint16Array(10), 10, -1), 655350);
shouldBe(fill(new Int32Array(10), 10, 2 ** 32 + 5), 50);
shouldBe(fill(new Uint32Array(10), 10, -1), 42949672950);
shouldBe(fill(new Float32Array(10), 10, 0.1), 10 * Math.fround(0.1));
shouldBe(fill(new Float64Array(10), 10, 0.1), 0.1 + 0.1 + 0.1 + 0.1 + 0.1 + 0.1 + 0.1 + 0.1 + 0.1 + 0.1);
shouldBe(fill(new Float64Array(5), 10, 1), NaN);
shouldBe(fill(new Int32Array(10), 10, "7"), 70);
shouldBe(fill({ length: 0 }, 10, 2), 20);

function codes(s, from, to) { let t = 0; for (let i = from; i < to; i++) t += s.charCodeAt(i); return t; }
shouldBe(codes("abc", 0, 3), 294);
shouldBe(codes("abc", 0, 3), 294);
shouldBe(codes("a\u1234c", 0, 3), 97 + 0x1234 + 99);
shouldBe(codes("abc", 0, 4), NaN);
shouldBe(codes("abc", -1, 2), NaN);
shouldBe(codes("ab" + String(Math.random()).slice(0, 0) + "cd".repeat(20), 0, 42), 97 + 98 + 20 * 199);
shouldBe(codes({ charCodeAt(i) { return i; } }, 0, 4), 6);
shouldBe(codes(new String("abc"), 0, 3), 294);
function fractional(s) { let t = 0; for (let i = 0; i < 2; i += 0.5) t += s.charCodeAt(i); return t; }
shouldBe(fractional("ab"), 97 + 97 + 98 + 98);

function pushes(a, n, v) { let last = 0; for (let i = 0; i < n; i++) last = a.push(v); return last + ":" + a.length + ":" + a[a.length - 1]; }
shouldBe(pushes([], 100, 1), "100:100:1");
shouldBe(pushes([], 100, 1.5), "100:100:1.5");
shouldBe(pushes([], 100, "s"), "100:100:s");
shouldBe(pushes([1], 10, 1.5), "11:11:1.5");
shouldBe(pushes([1.5], 10, NaN), "11:11:NaN");
shouldBe(pushes([1.5], 10, "s"), "11:11:s");
shouldBe(pushes([1, 2, 3], 3, {}), "6:6:[object Object]");
shouldBe(pushes({ length: 3, push: Array.prototype.push }, 2, 7), "5:5:7");
class Sub extends Array { }
shouldBe(pushes(new Sub, 5, 1), "5:5:1");
function ignores(a, n) { for (let i = 0; i < n; i++) a.push(i); return a.join(); }
shouldBe(ignores([], 5), "0,1,2,3,4");
shouldBe(ignores([9], 3), "9,0,1,2");
let threw = false;
try { pushes(Object.freeze([1]), 1, 2); } catch { threw = true; }
shouldBe(threw, true);
const sameEveryTime = [];
function pushesConstant(n) { for (let i = 0; i < n; i++) sameEveryTime.push(1); return sameEveryTime.length; }
shouldBe(pushesConstant(7), 7);
shouldBe(pushesConstant(7), 14);
function readsWhilePushing(a) { let s = 0; for (let i = 0; i < a.length && i < 20; i++) { s += a[i]; if (i < 5) a.push(i); } return s + ":" + a.length; }
shouldBe(readsWhilePushing([10]), "20:6");

function usesGlobal(n) {
    let s = 0;
    for (let i = 0; i < n; i++)
        s += Math.abs(-i);
    return s;
}
shouldBe(usesGlobal(5), 10);
shouldBe(usesGlobal(5), 10);

function repeats(list) {
    let s = 0;
    for (let i = 0; i < list.length; i++) {
        const o = list[i];
        s += o.m * o.a + o.m * o.b + o.m;
        o.a = o.a + 1;
        s += o.a;
    }
    return s;
}
const points = [];
for (let i = 0; i < 10; i++)
    points.push({ m: 2, a: i, b: 1 });
shouldBe(repeats(points), 10 * 2 + 2 * 45 + 2 * 10 + 55);
shouldBe(repeats(points), 10 * 2 + 2 * 55 + 2 * 10 + 65);
points[4] = Object.create({ m: 3 }, { a: { value: 0, writable: true }, b: { value: 1, writable: true } });
points[5] = { get m() { return this.count = (this.count || 0) + 1; }, a: 0, b: 0 };
let expectedSum = 0;
for (let i = 0; i < 10; i++) {
    if (i === 4) expectedSum += 3 * 0 + 3 * 1 + 3 + 1;
    else if (i === 5) expectedSum += 1 * 0 + 2 * 0 + 3 + 1;
    else expectedSum += 2 * (i + 2) + 2 * 1 + 2 + (i + 3);
}
shouldBe(repeats(points), expectedSum);
shouldBe(points[5].count, 3);
function invariantRepeats(o, n) { let s = 0; for (let i = 0; i < n; i++) s += o.k + o.k * i + o.k; return s; }
shouldBe(invariantRepeats({ k: 2 }, 4), 4 * 4 + 2 * 6);
shouldBe(invariantRepeats({ j: 0, k: 3 }, 4), 6 * 4 + 3 * 6);
let reads = 0;
shouldBe(invariantRepeats({ get k() { reads++; return 1; } }, 3), 2 * 3 + 3);
shouldBe(reads, 9);
