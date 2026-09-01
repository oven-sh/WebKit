//@ requireOptions("--useJSThreads=1")
// Under useJSThreads tryCachePutBy creates no Transition access case, so the
// shared put-transition handlers and operationReallocateButterflyAndTransition
// (which fail-stops flag-on) must never run. Drive out-of-line reallocating
// put transitions through put_by_id and put_by_val (string and symbol keys)
// hot enough for the JIT tiers to try caching them, on plain objects and on
// objects whose structure has an indexing header (the ReallocatingOutOfLine
// handler shape), on the main thread and on a spawned thread.

function check(cond, msg) { if (!cond) throw new Error(msg); }

// Twelve named properties leave the inline capacity of a literal behind and
// grow the out-of-line butterfly more than once.
function ById(i) {
    this.p0 = i; this.p1 = i + 1; this.p2 = i + 2; this.p3 = i + 3;
    this.p4 = i + 4; this.p5 = i + 5; this.p6 = i + 6; this.p7 = i + 7;
    this.p8 = i + 8; this.p9 = i + 9; this.p10 = i + 10; this.p11 = i + 11;
}
noInline(ById);

const k0 = "q0", k1 = "q1", k2 = "q2", k3 = "q3", k4 = "q4", k5 = "q5",
    k6 = "q6", k7 = "q7", k8 = "q8", k9 = "q9", k10 = "q10", k11 = "q11";
function byValString(o, i) {
    o[k0] = i; o[k1] = i + 1; o[k2] = i + 2; o[k3] = i + 3;
    o[k4] = i + 4; o[k5] = i + 5; o[k6] = i + 6; o[k7] = i + 7;
    o[k8] = i + 8; o[k9] = i + 9; o[k10] = i + 10; o[k11] = i + 11;
    return o;
}
noInline(byValString);

const s0 = Symbol("s0"), s1 = Symbol("s1"), s2 = Symbol("s2"), s3 = Symbol("s3"),
    s4 = Symbol("s4"), s5 = Symbol("s5"), s6 = Symbol("s6"), s7 = Symbol("s7"),
    s8 = Symbol("s8"), s9 = Symbol("s9"), s10 = Symbol("s10"), s11 = Symbol("s11");
const symbols = [s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11];
function byValSymbol(o, i) {
    o[s0] = i; o[s1] = i + 1; o[s2] = i + 2; o[s3] = i + 3;
    o[s4] = i + 4; o[s5] = i + 5; o[s6] = i + 6; o[s7] = i + 7;
    o[s8] = i + 8; o[s9] = i + 9; o[s10] = i + 10; o[s11] = i + 11;
    return o;
}
noInline(byValSymbol);

function withIndexingHeader() {
    var o = {};
    o[0] = 0;
    return o;
}
noInline(withIndexingHeader);

function verify(o, prefix, i, label) {
    for (var k = 0; k < 12; ++k) {
        var key = prefix === null ? symbols[k] : prefix + k;
        check(o[key] === i + k, label + ": " + String(key) + " = " + o[key] + " at " + i);
    }
}

function run(iterations, label) {
    for (var i = 0; i < iterations; ++i) {
        var a = new ById(i);
        var b = byValString({}, i);
        var c = byValSymbol({}, i);
        var d = byValString(withIndexingHeader(), i);
        var e = byValSymbol(withIndexingHeader(), i);
        var f = byValString([], i);
        if ((i & 255) === 0) {
            verify(a, "p", i, label + " by-id");
            verify(b, "q", i, label + " by-val string");
            verify(c, null, i, label + " by-val symbol");
            verify(d, "q", i, label + " by-val string, indexing header");
            verify(e, null, i, label + " by-val symbol, indexing header");
            verify(f, "q", i, label + " by-val string, array");
            check(Object.keys(a).length === 12, label + ": by-id key count " + Object.keys(a).length);
            check(Object.keys(d).length === 13, label + ": indexed key count " + Object.keys(d).length);
        }
    }
    return iterations;
}
noInline(run);

const ITERATIONS = 3000;
check(run(ITERATIONS, "main") === ITERATIONS, "main run did not complete");

var thread = new Thread(() => run(ITERATIONS, "spawned"));
check(thread.join() === ITERATIONS, "spawned run did not complete");

check(run(ITERATIONS, "main again") === ITERATIONS, "second main run did not complete");
