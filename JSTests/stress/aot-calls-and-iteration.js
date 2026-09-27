//@ requireOptions("--useAOT=1")
// Calls that are not plain ones, for-of and for-in, in code from the static compiler.

function shouldBe(actual, expected) {
    if (JSON.stringify(actual) !== JSON.stringify(expected))
        throw new Error("bad value: " + JSON.stringify(actual) + ", expected " + JSON.stringify(expected));
}
function shouldThrow(f, type) {
    let error;
    try { f(); } catch (e) { error = e; }
    if (!(error instanceof type))
        throw new Error("did not throw " + type.name + ": " + error);
}

(function () {
    "use strict";
    function sum(...values) { let total = 0; for (const value of values) total += value; return total; }
    function spread(array) { return sum(...array); }
    function apply() { return sum.apply(null, arguments); }
    function spreadRest(a, ...rest) { return sum(a, ...rest, a); }
    function loop(n, acc) { if (!n) return acc; return loop(n - 1, acc + 1); }
    function loopVarargs(n, acc) { if (!n) return acc; return loopVarargs(...[n - 1, acc + 1]); }
    function even(n) { return n ? odd(n - 1) : true; }
    function odd(n) { return n ? even(n - 1, 1, 2, 3, 4, 5) : false; } // More arguments than the caller has room for.
    function tailToHost(x) { return Math.abs(x); }
    function tailToBound(x) { return boundSum(x); }
    function tailToNothing(f) { return f(); }
    const boundSum = sum.bind(null, 1, 2);
    class A { constructor(x, y) { this.x = x; this.y = y; } }
    class B extends A { constructor(...args) { super(...args); this.z = 3; } }
    class C extends A { constructor(a) { super(a, 2); } }
    class D extends Array { constructor(...args) { super(...args); } }

    for (let i = 0; i < 3; ++i) {
        shouldBe(spread([1, 2, 3]), 6);
        shouldBe(spread([]), 0);
        shouldBe(spread(new Set([4, 5])), 9);
        shouldBe(apply(4, 5, 6), 15);
        shouldBe(spreadRest(1, 2, 3), 7);
        shouldBe(sum.apply(null, { length: 2, 0: 1, get 1() { return 41; } }), 42);
        shouldBe(loop(300000, 0), 300000);
        shouldBe(loopVarargs(300000, 0), 300000);
        shouldBe(even(300001), false);
        shouldBe(tailToHost(-3), 3);
        shouldBe(tailToBound(4), 7);
        shouldThrow(() => tailToNothing(undefined), TypeError);
        shouldThrow(() => tailToNothing({}), TypeError);
        shouldThrow(() => sum.apply(null, 1), TypeError);
        shouldThrow(() => sum.apply(null, { length: 2, get 0() { throw new RangeError; } }), RangeError);
        shouldThrow(() => sum.apply(null, { length: 1e9 }), RangeError);
        shouldBe(new B(1, 2), { x: 1, y: 2, z: 3 });
        shouldBe(new C(1), { x: 1, y: 2 });
        shouldBe(new D(1, 2, 3).length, 3);
        shouldBe(Reflect.construct(A, [1, 2], B) instanceof B, true);
    }
})();

(function () {
    function direct(x) { var y = 2; return eval("x + y"); }
    function declares() { eval("var z = 5"); return z; }
    function notEval(eval) { return eval("1 + 1"); }
    function strictEval(x) { "use strict"; return eval("var x = 3; x") + x; }
    function noArguments() { return eval(); }
    function mapped(a, b) { arguments[0] = 10; b = 20; return [a, arguments[1], arguments.length]; }
    function captured(a) { const f = () => a; arguments[0] = 7; return f(); }
    function unmapped(a) { "use strict"; arguments[0] = 10; return [a, arguments[0]]; }
    for (let i = 0; i < 3; ++i) {
        shouldBe(direct(1), 3);
        shouldBe(declares(), 5);
        shouldBe(notEval(s => s + "!"), "1 + 1!");
        shouldBe(strictEval(1), 4);
        shouldBe(noArguments(), undefined);
        shouldThrow(() => direct.call(null, { valueOf() { throw new EvalError; } }), EvalError);
        shouldBe(mapped(1, 2), [10, 20, 2]);
        shouldBe(captured(1), 7);
        shouldBe(unmapped(1), [1, 10]);
    }
})();

(function () {
    function collect(iterable, stopAt = collect) { const result = []; for (const value of iterable) { result.push(value); if (value === stopAt) break; } return result; }
    function destructure([a, , b = 9, ...rest]) { return [a, b, rest]; }
    function keys(object) { const result = []; for (const key in object) result.push(key + ":" + object[key]); return result; }
    function ownKeys(object) { const result = []; for (const key in object) { if (object.hasOwnProperty(key) && key in object) result.push(key); } return result; }
    function double(object) { for (const key in object) object[key] *= 2; return object; }
    function* generator() { try { yield 1; yield 2; yield 3; } finally { closed++; } }
    let closed = 0;
    const custom = {
        [Symbol.iterator]() { let i = 0; return { next() { return { done: i >= 3 ? "yes" : 0, value: i++ }; }, return() { closed++; return {}; } }; },
    };

    for (let i = 0; i < 3; ++i) {
        shouldBe(collect([1, 2, 3]), [1, 2, 3]);
        shouldBe(collect([1, , 3]), [1, null, 3]);
        shouldBe(collect([1.5, 2.5]), [1.5, 2.5]);
        shouldBe(collect([1, 2, 3], 2), [1, 2]);
        shouldBe(collect([1, 2].keys()), [0, 1]);
        shouldBe(collect([1, 2].entries()), [[0, 1], [1, 2]]);
        shouldBe(collect(new Set([1, 2])), [1, 2]);
        shouldBe(collect(new Map([[1, 2]])), [[1, 2]]);
        shouldBe(collect(new Map([[1, 2]]).values()), [2]);
        shouldBe(collect("a\u{1F600}b"), ["a", "\u{1F600}", "b"]);
        closed = 0;
        shouldBe(collect(generator(), 2), [1, 2]);
        shouldBe(collect(custom, 1), [0, 1]);
        shouldBe(collect(custom), [0, 1, 2]);
        shouldBe(closed, 2);
        shouldBe(destructure([1, 2, undefined, 4, 5]), [1, 9, [4, 5]]);
        shouldBe(destructure("ab"), ["a", 9, []]);
        shouldThrow(() => collect(1), TypeError);
        shouldThrow(() => collect({ [Symbol.iterator]() { return 1; } }), TypeError);
        shouldThrow(() => collect({ [Symbol.iterator]() { return { next() { return 1; } }; } }), TypeError);
        const growing = [1, 2];
        const seen = [];
        for (const value of growing) { seen.push(value); if (growing.length < 4) growing.push(value + 2); }
        shouldBe(seen, [1, 2, 3, 4]);

        shouldBe(keys({ a: 1, b: 2 }), ["a:1", "b:2"]);
        shouldBe(keys([7, 8]), ["0:7", "1:8"]);
        shouldBe(keys(Object.create({ inherited: 1 }, { own: { value: 2, enumerable: true } })), ["own:2", "inherited:1"]);
        shouldBe(keys(null), []);
        shouldBe(keys("ab"), ["0:a", "1:b"]);
        shouldBe(ownKeys(Object.create({ inherited: 1 }, { own: { value: 2, enumerable: true } })), ["own"]);
        shouldBe(double({ a: 1, 0: 2 }), { 0: 4, a: 2 });
    }

    // Closing an array's iterator becomes observable half way through.
    function breaksOut(array) { for (const value of array) { if (value === 2) { patch(); break; } } }
    let returned = 0;
    let patch = () => { };
    breaksOut([1, 2, 3]);
    patch = () => { Object.getPrototypeOf([][Symbol.iterator]()).return = function () { returned++; return {}; }; };
    breaksOut([1, 2, 3]);
    shouldBe(returned, 1);
    breaksOut([1, 2, 3]);
    shouldBe(returned, 2);
})();

(function () {
    const log = [];
    async function* asyncGenerator() { yield 1; yield 2; }
    async function consume(iterable) { const result = []; for await (const value of iterable) result.push(value); return result; }
    async function* delegate(iterable) { return yield* iterable; }
    const custom = { [Symbol.asyncIterator]() { let i = 0; return { next() { return Promise.resolve({ done: i > 1, value: i++ }); } }; } };
    consume(asyncGenerator()).then(v => log.push(v));
    consume([Promise.resolve(3), 4]).then(v => log.push(v));
    consume(custom).then(v => log.push(v));
    consume(delegate(asyncGenerator())).then(v => log.push(v));
    consume({ [Symbol.asyncIterator]() { return 1; } }).catch(e => log.push(e instanceof TypeError));
    drainMicrotasks();
    log.sort();
    shouldBe(log, [[0, 1], [1, 2], [1, 2], [3, 4], true]);
})();
