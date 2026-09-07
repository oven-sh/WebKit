//@ runDefault("--useBytecodeOptimizer=1")
// Copy propagation / destination coalescing around calls: a call's callee frame (header, arguments, alignment
// padding, callee locals) overwrites the caller's high registers, generator bodies are re-entered with fresh
// arguments after every yield, and frame-building instructions read operands after writing part of the frame.

function shouldBe(actual, expected, what) {
    if (String(actual) !== String(expected))
        throw new Error(what + ": expected " + expected + " but got " + actual);
}

function foo(a, b) { return { a: a + 1, b }; }
function bar(array) { return foo.apply(undefined, array); }
function check(x, y) { return typeof x + " " + typeof y + " " + x.a + " " + y.a + " " + y.b; }
function valueLiveAcrossCall(array) {
    const expected = { a: array[0] + 1, b: array[1], c: array };
    const actual = bar(array); // even argument count: the frame has a padding slot above the arguments
    return check(actual, expected); // odd argument count
}
for (let i = 0; i < 200; i++)
    shouldBe(valueLiveAcrossCall([i, i * 2]), "object object " + (i + 1) + " " + (i + 1) + " " + i * 2, "value live across call");

function three(a, b, c) { return a + b + c; }
function nested(x) { return three(three(x, 1, 2), three(x, 3, 4), three(x, 5, 6)); }
shouldBe(nested(1) + "," + nested(10), "24,51", "nested calls as arguments");

(function () {
    "use strict";
    function wide(a, b, c, d, e, f, g) { return [a, b, c, d, e, f, g].join(","); }
    function narrow(arr) { return wide.apply(null, arr); } // tail_call_varargs with more arguments than the caller received
    function outer(v) { const keep = { v }; const r = narrow([v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6]); return keep.v + ":" + r; }
    for (let i = 0; i < 3; i++)
        shouldBe(outer(10 * i), 10 * i + ":" + [0, 1, 2, 3, 4, 5, 6].map(k => 10 * i + k), "tail_call_varargs growing the frame");
    function spreadCall(...args) { return wide(...args, "x", "y", "z"); }
    shouldBe(spreadCall(1, 2, 3, 4), "1,2,3,4,x,y,z", "spread tail call");
})();

function jsNull() { return null; }
function run(f) { try { return String(f()); } catch (e) { return e.constructor.name; } }
class A { constructor() { this.v = "a"; } }
function makeB() { const B = class extends A { }; return new B().v; }
function makeC() { class C extends jsNull() { constructor() { super(); } } return new C(); }
function makeD() { const o = {}; class D extends null { constructor() { return o; } } return new D() === o; }
for (let i = 0; i < 50; i++)
    shouldBe([run(makeB), run(makeC), run(makeD)], "a,TypeError,true", "construct callee not read from a clobbered register");

async function fib(n) { return n < 2 ? n : (await fib(n - 1)) + (await fib(n - 2)); }
function* gen(x) { const a = yield x; const b = yield a + x; return a + b + x; }
const g = gen(1);
shouldBe([g.next().value, g.next(10).value, g.next(100).value], "1,11,111", "generator re-entry values");
async function main() {
    shouldBe(await fib(12), 144, "await results not forwarded across yields");
    const log = [];
    if (false) { log.push(await fib(1)); } else { await null; log.push("else"); } // unreachable await
    shouldBe(log, "else", "unreachable yield removed");
    async function* agen() { yield 1; yield (await Promise.resolve(2)); yield 3; }
    const collected = [];
    for await (const v of agen()) collected.push(v);
    shouldBe(collected, "1,2,3", "async generator");
}
let asyncFailure;
main().catch(e => { asyncFailure = e; });
drainMicrotasks();
if (asyncFailure)
    throw asyncFailure;

const custom = { [Symbol.iterator]() { let i = 0; return { next: () => ({ done: i >= 3, value: i++ }), return() { custom.closed = true; return {}; } }; } };
for (const v of custom) { if (v === 1) break; }
shouldBe(custom.closed, true, "iterator return on break");
const [p, , q = 5, ...tail] = [1, 2, undefined, 4, 5].map(v => v);
shouldBe([p, q, tail], "1,5,4,5", "array destructuring through iterator ops");
