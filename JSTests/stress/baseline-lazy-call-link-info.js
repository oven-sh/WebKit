//@ runDefault("--useDollarVM=true", "--useConcurrentJIT=false", "--thresholdForJITAfterWarmUp=5", "--thresholdForJITSoon=5")
//@ runDefault("--useDollarVM=true", "--useConcurrentJIT=false", "--thresholdForJITAfterWarmUp=5", "--thresholdForJITSoon=5", "--useDFGJIT=false")
//@ runDefault("--useDollarVM=true", "--useConcurrentJIT=false", "--thresholdForJITAfterWarmUp=5", "--thresholdForJITSoon=5", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20", "--thresholdForOptimizeSoon=20")
//@ runDefault("--useDollarVM=true", "--useLazyLLIntCallLinkInfos=false")
//@ runDefault("--useDollarVM=true", "--useJIT=false")
//@ runDefault("--useDollarVM=true")

// A function whose Baseline code is shared through its UnlinkedCodeBlock: every evaluation of the same source makes a new
// CodeBlock that starts out in that code with call sites that own no CallLinkInfo yet. They must behave as in the LLInt: the
// first call of a site goes through the CallLinkInfo all such sites share, the second gives it its own. A tail call gets its
// own when it runs for the first time, and so does a direct eval whose callee is not eval.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: bad value: ${String(actual)}, expected ${String(expected)}`);
}

const source = `(function (kit, n) {
    function finishWithTailCall(log, which) {
        "use strict";
        if (which === 1)
            return kit.never();
        if (which === 2)
            return kit.never(log);
        return kit.finish(log, n);
    }
    function notReallyEval(eval, x) {
        kit.evalCallerRanInBaselineCode = $vm.baselineJITTrue();
        if (x < 0)
            return eval("unreachable");
        return eval(4, x);
    }
    kit.inner = [finishWithTailCall, notReallyEval];
    let log = [];
    // Plain calls, run once, twice and often.
    log.push(kit.add(1, n));
    for (let i = 0; i < 2; ++i)
        log.push(kit.add(2, i));
    for (let i = 0; i < 12; ++i)
        log.push(kit.add(3, i));
    // A site that never runs.
    if (n < 0)
        log.push(kit.never());
    // A host function, a bound function, a constructor, a class, a getter's call.
    log.push(Math.max(n, 3));
    log.push(kit.bound(n));
    log.push(new kit.Point(n, 2).x);
    log.push(new kit.Derived(n).y);
    log.push(kit.objectWithGetter.value);
    // Too few and too many arguments.
    log.push(kit.three(n));
    log.push(kit.three(n, 1, 2, 3, 4));
    // A callee that throws the first time it is called from here, and things that cannot be called or constructed.
    try {
        kit.thrower(n);
        log.push("no throw");
    } catch (e) {
        log.push(e.message);
    }
    try {
        kit.notCallable(n);
        log.push("no throw");
    } catch (e) {
        log.push(e instanceof TypeError);
    }
    try {
        new kit.arrow();
        log.push("no throw");
    } catch (e) {
        log.push(e instanceof TypeError);
    }
    // Iteration protocol call sites.
    let sum = 0;
    for (let value of kit.iterable)
        sum += value;
    log.push(sum);
    for (let value of [n, 1])
        sum += value;
    log.push(sum);
    // Something called eval that is not, and the real one.
    log.push(notReallyEval(kit.add, n));
    log.push(kit.realEval("n => n + 5")(n));
    // Different callees at one site.
    for (let f of kit.many)
        log.push(f(n));
    return finishWithTailCall(log, 0);
})`;

class Base { constructor(y) { this.y = y + 10; } }
class Derived extends Base { constructor(y) { super(y); } }
const kit = {
    add(a, b) { return a + b; },
    never() { throw new Error("unreachable"); },
    bound: function (a, b) { return a * b; }.bind(null, 7),
    Point: function (x, y) { this.x = x; this.y = y; },
    Derived,
    objectWithGetter: { get value() { return kit.add(20, 1); } },
    three(a, b, c) { return `${a},${b},${c}`; },
    thrower(n) { throw new Error("thrown " + n); },
    notCallable: 42,
    arrow: () => { },
    iterable: { *[Symbol.iterator]() { yield 1; yield 2; yield 3; } },
    realEval: eval,
    many: [x => x + 1, x => x + 2, x => x + 3, x => x + 4, x => x + 5, x => x + 6, x => x + 7, x => x + 8, x => x + 9],
    finish(log, n) { return log.join("|") + "#" + n; },
};

function expected(n) {
    let log = [];
    log.push(1 + n);
    for (let i = 0; i < 2; ++i)
        log.push(2 + i);
    for (let i = 0; i < 12; ++i)
        log.push(3 + i);
    log.push(Math.max(n, 3), 7 * n, n, n + 10, 21, `${n},undefined,undefined`, `${n},1,2`, "thrown " + n, true, true);
    log.push(6, 6 + n + 1, 4 + n, n + 5);
    for (let i = 1; i <= 9; ++i)
        log.push(n + i);
    return log.join("|") + "#" + n;
}

// With --useLazyLLIntCallLinkInfos=false every site has its own from the start.
function probe() { return kit.add(1, 1); }
probe();
const lazyCallLinkInfos = !$vm.numberOfOwnCallLinkInfos(probe);

for (let n = 0; n < 40; ++n) {
    let f = (0, eval)(source);
    shouldBe(f(kit, n), expected(n), "evaluation " + n);
    let [finishWithTailCall, notReallyEval] = kit.inner;

    // Which sites own a CallLinkInfo now says who was taken for whom. finishWithTailCall() has three tail calls and ran once:
    // the one that ran has its own, in finishWithTailCall(), and no other site has been given one on its behalf.
    // notReallyEval() has two direct evals, one of which ran, once, with a callee that is not eval.
    if (lazyCallLinkInfos) {
        shouldBe($vm.numberOfOwnCallLinkInfos(finishWithTailCall), 1, "own CallLinkInfos in the tail caller, evaluation " + n);
        // (The LLInt does not use a CallLinkInfo for that call.)
        shouldBe($vm.numberOfOwnCallLinkInfos(notReallyEval), kit.evalCallerRanInBaselineCode ? 1 : 0, "own CallLinkInfos in the eval caller, evaluation " + n);
        // In f() itself the sites in loops ran more than once; the rest ran once and own none.
        shouldBe($vm.numberOfOwnCallLinkInfos(f) <= 12, true, "own CallLinkInfos in the outer function, evaluation " + n);
    }

    // Again on the same CodeBlock: the sites that ran once get their own CallLinkInfo now.
    if (n % 3 === 0) {
        shouldBe(f(kit, n + 1), expected(n + 1), "second call of evaluation " + n);
        if (lazyCallLinkInfos)
            shouldBe($vm.numberOfOwnCallLinkInfos(f) > 12, true, "own CallLinkInfos in the outer function after a second call, evaluation " + n);
    }
    if (n % 13 === 0)
        fullGC();
}
