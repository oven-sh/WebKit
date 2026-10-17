//@ runDefault
//@ runDefault("--useLazyLLIntCallLinkInfos=false")
//@ runDefault("--useJIT=false")
//@ runDefault("--useConcurrentJIT=false", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20", "--thresholdForOptimizeSoon=20", "--thresholdForFTLOptimizeAfterWarmUp=50")
//@ runDefault("--collectContinuously=true", "--useGenerationalGC=false")
//@ runDefault("--useEagerCodeBlockJettisonTiming=true")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}`);
}

function shouldThrow(func, errorType) {
    let error = null;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error(`bad error: ${String(error)}`);
}

// A site is entered again, with the same and with another callee, before its first execution returns.
function recurse(n) { return n ? recurse(n - 1) + 1 : 0; }
function through(f, n) { return f(n); }
function ping(n) { return n ? through(pong, n - 1) + 1 : 0; }
function pong(n) { return n ? through(ping, n - 1) + 2 : 0; }
function constructThrough(C, n) { return new C(n); }
function Chain(n) { this.depth = n ? constructThrough(Chain, n - 1).depth + 1 : 0; }

// The first execution of a site does not return normally.
let throwCount = 0;
function throwsFirst(x) { if (!throwCount++) throw new RangeError("first"); return x + 1; }
function callsThrowsFirst(x) { return throwsFirst(x); }
function notCallableFirst(f, x) { return f(x); }
function tailNotCallableFirst(f, x) { "use strict"; return f(x); }
function deep(n) { return deep(n + 1) + 1; }
function catchesInside(f) { try { return f(1); } catch { return f; } }

// A callee inlined into optimized code exits at a call site that has never run.
function rareCallee(x) { return x * 3; }
function inlined(x, rare) { if (rare) return rareCallee(x) + 1; return x + 1; }
function hot(n, rare) { let s = 0; for (let i = 0; i < n; ++i) s += inlined(i, rare); return s; }

// Callers with sites in every state while their callees get better code, lose it, and die.
function makeCallee(k) { return new Function("x", `let s = 0; for (let i = 0; i < 4; ++i) s += x + ${k}; return s;`); }
function mixed(mode, f, g, x) {
    let result = 0;
    if (mode & 1)
        result += f(x);
    if (mode & 2)
        result += g(x);
    if (mode & 4)
        result += f(x) + g(x);
    if (mode & 8)
        result += new f(x) instanceof f ? 1 : 0;
    return result + f(x);
}
noInline(mixed);

function phase(round) {
    shouldBe(recurse(3), 3);
    shouldBe(recurse(200), 200);
    shouldBe(through(ping, 6), 9);
    shouldBe(through(pong, 7), 11);
    shouldBe(new Chain(5).depth, 5);
    shouldBe(constructThrough(Chain, 30).depth, 30);

    throwCount = 0;
    shouldThrow(() => callsThrowsFirst(1), RangeError);
    shouldBe(callsThrowsFirst(1), 2);
    shouldBe(callsThrowsFirst(2), 3);
    shouldThrow(() => notCallableFirst(undefined, 1), TypeError);
    shouldBe(notCallableFirst(rareCallee, 1), 3);
    shouldThrow(() => notCallableFirst(null, 1), TypeError);
    shouldBe(notCallableFirst(rareCallee, 2), 6);
    shouldThrow(() => tailNotCallableFirst(3, 1), TypeError);
    shouldBe(tailNotCallableFirst(rareCallee, 2), 6);
    shouldBe(tailNotCallableFirst(rareCallee, 3), 9);
    shouldThrow(() => deep(0), RangeError);
    shouldBe(catchesInside(3), 3);
    shouldBe(catchesInside(rareCallee), 3);

    shouldBe(hot(2000, false), 2001000);
    shouldBe(hot(10, true), 145);
    shouldBe(hot(2000, true), 5999000);

    let f = makeCallee(round);
    let g = makeCallee(round + 100);
    shouldBe(mixed(0, f, g, 1), 4 + 4 * round);
    shouldBe(mixed(1, f, g, 1), 2 * (4 + 4 * round));
    for (let i = 0; i < 3000; ++i)
        mixed(i & 3, f, g, i);
    shouldBe(mixed(2, f, g, 1), 4 + 4 * round + 4 + 4 * (round + 100));
    f = null;
    g = null;
    fullGC();
    let h = makeCallee(round + 200);
    shouldBe(mixed(15, h, h, 0), 5 * 4 * (round + 200) + 1);
    shouldBe(mixed(15, h, h, 0), 5 * 4 * (round + 200) + 1);
}

// Everything above runs again on CodeBlocks that were thrown away and linked anew.
let round = 0;
function next() {
    phase(round);
    if (++round === 6)
        return;
    if (round & 1)
        $vm.deleteAllCodeWhenIdle();
    else
        fullGC();
    setTimeout(next, 0);
}
next();
