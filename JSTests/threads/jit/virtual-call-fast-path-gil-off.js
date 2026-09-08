//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// SPEC-jit §5.8 r15: GIL off, a virtual (unlinkable) call site must reach a
// JS callee through the virtual-call thunk's fast path - executable ->
// CodeBlock -> its published JITCode -> that code's arity-check entry - and not through
// operationVirtualCall. Before the seventh landing round the executable's
// arity-check mirror was kept null GIL off, so EVERY virtual call to a script
// function took the C++ path (31 M times in one JetStream test). The call
// site below sees 64 distinct functions, more than a polymorphic call stub
// holds, so it settles as a virtual call. Checks the slow-path count through
// the diagnostic counters and the loop's cost against a monomorphic twin, on
// the main thread and on a spawned thread.
load("../harness.js", "caller relative");

// 64 DISTINCT executables: closures of one function would link as a closure
// call stub; more callees than the polymorphic call stub holds
// (maxPolymorphicCallVariantListSize) make the site virtual.
function makeAdder(k) { return new Function("x", "return x + " + k + ";"); }
noInline(makeAdder);
function callIt(f, x) { return f(x); } // the virtual call site
noInline(callIt);
function mono(x) { return x + 1; }
function callMono(x) { return mono(x); }
noInline(callMono);

function run(n) {
    const fs = [];
    for (let i = 0; i < 64; ++i) fs.push(makeAdder(i));
    let s = 0;
    const t0 = preciseTime();
    for (let i = 0; i < n; ++i) s += callIt(fs[i & 63], i);
    const tVirtual = preciseTime() - t0;
    let m = 0;
    const t1 = preciseTime();
    for (let i = 0; i < n; ++i) m += callMono(i);
    const tMono = preciseTime() - t1;
    return { s, m, tVirtual, tMono };
}

function check(where) {
    run(200000); // tier up, settle the call site as virtual
    const slowBefore = $vm.jsThreadsCounter("callVirtualSlow");
    const N = 2000000;
    const r = run(N);
    const slowCalls = $vm.jsThreadsCounter("callVirtualSlow") - slowBefore;
    if (r.s !== 2000062000000 || r.m !== 2000001000000) throw new Error(where + ": wrong sums " + r.s + " " + r.m);
    if (typeof AMPLIFY_VERBOSE !== "undefined")
        print(where + ": virtual " + (r.tVirtual * 1e3).toFixed(1) + " ms, mono " + (r.tMono * 1e3).toFixed(1) + " ms, operationVirtualCall calls " + slowCalls);
    // Sixth-round binary GIL off: slowCalls == N (every call) and the virtual
    // loop 16x the monomorphic one. Through the thunk: a few hundred slow
    // calls at most (the site settling, tier-up relinks) and about 3x, the
    // same as GIL on.
    if (slowCalls > N / 100) throw new Error(where + ": " + slowCalls + " of " + N + " virtual calls took operationVirtualCall");
    // The timing ratio is a Release-build check; Debug/sanitizer builds skew
    // the two loops differently (the counter above is the invariant there).
    if (!$vm.assertEnabled() && r.tVirtual > r.tMono * 8) throw new Error(where + ": virtual calls " + (r.tVirtual / r.tMono).toFixed(1) + "x the monomorphic loop");
}

check("main thread");
new Thread(() => { check("spawned thread"); return 1; }).join();
print("PASS");
