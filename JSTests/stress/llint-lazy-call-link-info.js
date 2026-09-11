//@ runDefault
//@ runDefault("--useLazyLLIntCallLinkInfos=false")
//@ runDefault("--useLLIntICs=false")
//@ runDefault("--useJIT=false")
//@ runDefault("--useConcurrentJIT=false", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20", "--thresholdForOptimizeSoon=20", "--thresholdForFTLOptimizeAfterWarmUp=50")
//@ runDefault("--useConcurrentJIT=true", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20", "--thresholdForOptimizeSoon=20", "--thresholdForFTLOptimizeAfterWarmUp=50")
//@ runDefault("--collectContinuously=true", "--useGenerationalGC=false")
//@ runDefault("--useEagerCodeBlockJettisonTiming=true")
//@ runDefault("--forceEagerCompilation=true")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}`);
}

function shouldThrow(func, check) {
    let error = null;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!error)
        throw new Error("not thrown");
    if (!check(error))
        throw new Error(`bad error: ${String(error)}`);
}

// Sites that run zero, one, two and many times inside one function.
function add1(x) { return x + 1; }
function add2(x) { return x + 2; }
function add3(x) { return x + 3; }
function never(x) { throw new Error("unreachable"); }
function counts(n) {
    let result = 0;
    if (n < 0)
        result += never(n);
    if (n === 0)
        result += add1(n);
    if (n < 2)
        result += add2(n);
    result += add3(n);
    return result;
}
noInline(counts);
shouldBe(counts(0), 1 + 2 + 3);
shouldBe(counts(1), 3 + 4);
for (let i = 2; i < 200; ++i)
    shouldBe(counts(i), i + 3);
shouldBe(counts(0), 6);

// The same site sees one callee, then a second, then many.
function callIt(f, x) { return f(x); }
noInline(callIt);
shouldBe(callIt(add1, 1), 2);
shouldBe(callIt(add1, 1), 2);
shouldBe(callIt(add2, 1), 3);
{
    let functions = [];
    for (let i = 0; i < 40; ++i)
        functions.push(new Function("x", `return x + ${i};`));
    for (let round = 0; round < 5; ++round) {
        for (let i = 0; i < functions.length; ++i)
            shouldBe(callIt(functions[i], 1), 1 + i);
    }
}

// Closures of one executable.
function makeAdder(n) { return function (x) { return x + n; }; }
function callClosure(f, x) { return f(x); }
noInline(callClosure);
for (let i = 0; i < 100; ++i)
    shouldBe(callClosure(makeAdder(i), 1), i + 1);

// Host functions, InternalFunctions, bound functions, proxies and class constructors as the first callee.
function callHost(x) { return Math.abs(x); }
function constructInternal() { return new Map(); }
function callInternal(x) { return String(x); }
function callBound(f) { return f(); }
function callProxy(p) { return p(20); }
function constructProxy(p) { return new p(21); }
function constructClass(C) { return new C(22); }
class Point { constructor(x) { this.x = x; } }
let bound = add1.bind(null, 41);
let proxy = new Proxy(function (x) { return this === undefined ? x : x + 1; }, { });
let constructProxyTarget = new Proxy(Point, { });
for (let i = 0; i < 3; ++i) {
    shouldBe(callHost(-4), 4);
    shouldBe(constructInternal() instanceof Map, true);
    shouldBe(callInternal(12), "12");
    shouldBe(callBound(bound), 42);
    shouldBe(callProxy(proxy), 21);
    shouldBe(constructProxy(constructProxyTarget).x, 21);
    shouldBe(constructClass(Point).x, 22);
}

// Errors thrown by the call itself, on the first and on later executions of the site.
function callNonFunction(o) { return o.notThere(1); }
function constructNonConstructor(f) { return new f(); }
function callClass(C) { return C(); }
let arrow = () => 1;
for (let i = 0; i < 3; ++i) {
    shouldThrow(() => callNonFunction({ }), (e) => e instanceof TypeError && String(e).includes("o.notThere"));
    shouldThrow(() => constructNonConstructor(arrow), (e) => e instanceof TypeError && String(e).includes("is not a constructor"));
    shouldThrow(() => constructNonConstructor(3), (e) => e instanceof TypeError && String(e).includes("is not a constructor"));
    shouldThrow(() => callClass(Point), (e) => e instanceof TypeError);
}

// The callee throws on the first execution of the site; the stack has the caller.
function thrower() { throw new Error("thrown"); }
function callsThrower() { return thrower(); }
for (let i = 0; i < 3; ++i)
    shouldThrow(callsThrower, (e) => e.message === "thrown" && e.stack.includes("callsThrower"));

// Tail calls: once, twice, many, and to a non-function.
function tailTarget(x) { "use strict"; return x * 2; }
function tailOnce(x) { "use strict"; return tailTarget(x); }
function tailTwice(x) { "use strict"; return tailTarget(x); }
function tailMany(x) { "use strict"; return tailTarget(x); }
function tailNonFunction(o) { "use strict"; return o.notThere(1); }
function tailLoop(n, acc) { "use strict"; if (!n) return acc; return tailLoop(n - 1, acc + 1); }
shouldBe(tailOnce(2), 4);
shouldBe(tailTwice(2), 4);
shouldBe(tailTwice(3), 6);
for (let i = 0; i < 200; ++i)
    shouldBe(tailMany(i), i * 2);
for (let i = 0; i < 3; ++i)
    shouldThrow(() => tailNonFunction({ }), (e) => e instanceof TypeError && String(e).includes("o.notThere"));
shouldBe(tailLoop(100000, 0), 100000);

// Construct: once, twice, many, derived classes.
function Box(v) { this.v = v; }
function makeBoxOnce(v) { return new Box(v); }
function makeBoxMany(v) { return new Box(v); }
class Base { constructor(v) { this.v = v; } }
class Derived extends Base { constructor(v) { super(v + 1); } }
class DerivedSpread extends Base { constructor(...args) { super(...args); } }
shouldBe(makeBoxOnce(1).v, 1);
for (let i = 0; i < 200; ++i)
    shouldBe(makeBoxMany(i).v, i);
shouldBe(new Derived(1).v, 2);
shouldBe(new Derived(2).v, 3);
shouldBe(new DerivedSpread(5).v, 5);
for (let i = 0; i < 100; ++i) {
    shouldBe(new Derived(i).v, i + 1);
    shouldBe(new DerivedSpread(i).v, i);
}

// Varargs.
function sum() { let s = 0; for (let i = 0; i < arguments.length; ++i) s += arguments[i]; return s; }
function spreadCall(args) { return sum(...args); }
function applyCall(args) { return sum.apply(null, args); }
function spreadConstruct(args) { return new Box(...args); }
function spreadTailCall(args) { "use strict"; return sum(...args); }
function forwardArguments() { return sum.apply(this, arguments); }
shouldBe(spreadCall([1, 2, 3]), 6);
shouldBe(applyCall([1, 2, 3, 4]), 10);
shouldBe(spreadConstruct([7]).v, 7);
shouldBe(spreadTailCall([1, 2]), 3);
shouldBe(forwardArguments(1, 2, 3, 4, 5), 15);
for (let i = 0; i < 100; ++i) {
    shouldBe(spreadCall([i, 1]), i + 1);
    shouldBe(applyCall([i, 2]), i + 2);
    shouldBe(spreadConstruct([i]).v, i);
    shouldBe(spreadTailCall([i, 3]), i + 3);
    shouldBe(forwardArguments(i, 4), i + 4);
}

// A collection between the first and the second execution of a site, and one after it linked.
function callee1(x) { return x + 10; }
function callAcrossGC(x) { return callee1(x); }
noInline(callAcrossGC);
shouldBe(callAcrossGC(1), 11);
fullGC();
shouldBe(callAcrossGC(2), 12);
shouldBe(callAcrossGC(3), 13);
fullGC();
edenGC();
for (let i = 0; i < 100; ++i)
    shouldBe(callAcrossGC(i), i + 10);

// The callee's code goes away between executions of the site.
function makeCallee() { return new Function("x", "return x + 100;"); }
function callDisposable(f, x) { return f(x); }
noInline(callDisposable);
for (let i = 0; i < 20; ++i) {
    let f = makeCallee();
    shouldBe(callDisposable(f, i), i + 100);
    if (i % 3 === 0) {
        f = null;
        fullGC();
    }
}

// A function gets hot while most of its call sites never ran, then they all run.
function sparse(mode, x) {
    if (mode === 1)
        return add1(x);
    if (mode === 2)
        return add2(x);
    if (mode === 3)
        return add3(x) + add1(x);
    if (mode === 4)
        return new Box(x).v;
    if (mode === 5)
        return sum(...[x, x]);
    return x;
}
noInline(sparse);
for (let i = 0; i < 20000; ++i)
    shouldBe(sparse(0, i), i);
shouldBe(sparse(1, 1), 2);
shouldBe(sparse(2, 1), 3);
shouldBe(sparse(3, 1), 6);
shouldBe(sparse(4, 1), 1);
shouldBe(sparse(5, 1), 2);
for (let i = 0; i < 20000; ++i)
    shouldBe(sparse(1 + (i % 5), 1) > 0, true);

// A loop tiers up in the middle of the first execution of the function.
function hotLoop(n) {
    let result = 0;
    for (let i = 0; i < n; ++i)
        result = add1(result);
    result += add2(0);
    return result;
}
shouldBe(hotLoop(100000), 100002);

// Iteration protocols call through the same kind of site.
function iterate(iterable) { let s = 0; for (let v of iterable) s += v; return s; }
function* generator() { yield 1; yield 2; yield 3; }
let customIterable = { [Symbol.iterator]() { let i = 0; return { next() { return { done: i >= 3, value: i++ }; } }; } };
shouldBe(iterate(generator()), 6);
shouldBe(iterate(customIterable), 3);
shouldBe(iterate([1, 2, 3, 4]), 10);
shouldBe(iterate(new Set([5, 6])), 11);
for (let i = 0; i < 100; ++i) {
    shouldBe(iterate(generator()), 6);
    shouldBe(iterate(customIterable), 3);
}
shouldThrow(() => iterate({ [Symbol.iterator]: 3 }), (e) => e instanceof TypeError);
shouldThrow(() => iterate({ [Symbol.iterator]() { return { next: 3 }; } }), (e) => e instanceof TypeError);

// eval and functions named eval.
function callsEval(x) { return eval("x + 1"); }
function callsFakeEval(eval, x) { return eval(x); }
for (let i = 0; i < 3; ++i) {
    shouldBe(callsEval(1), 2);
    shouldBe(callsFakeEval(add1, 1), 2);
}

// Deep recursion through a fresh site overflows the stack cleanly.
function recurse(n) { return recurse(n + 1) + 1; }
shouldThrow(() => recurse(0), (e) => e instanceof RangeError);
shouldThrow(() => recurse(0), (e) => e instanceof RangeError);
