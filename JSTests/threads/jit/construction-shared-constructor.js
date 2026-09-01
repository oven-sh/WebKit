//@ skip (bench-only: needs reportBench from ../bench/harness.js; driven by jit/bench-gates.sh, not the corpus runner)
// construction-shared-constructor.js — SPEC-jit Task 13: shared-constructor
// construction microbench vs the GIL stub (RECORDED; feeds the OM 8h Task-14
// promotion decision — relative per-op cost of E4 transitions vs the
// post-F2 locked N2/§4.3+L6 path with the TTL sets force-fired).
//
// Run via the bench harness:
//   jsc [--useJSThreads=0|1] --useDollarVM=1 ../bench/harness.js construction-shared-constructor.js
//
// Two BENCH lines:
//   construct-pre-share  — constructing while the constructor's structure
//                          chain TTL sets are still valid (flag-on: the
//                          lock-free E4/owner-transition regime once OM
//                          lands; today: the plain transition path).
//   construct-post-share — same constructor AFTER an instance was written by
//                          a foreign thread (flag-on: writeThreadLocal fired
//                          on the chain => transitions take the locked/SW
//                          regime). Flag-off (or no Thread global): the
//                          foreign write is simulated on-thread; the line is
//                          still recorded so the GIL-stub comparison in
//                          bench-gates.sh has both sides in both configs.
//
// bench-gates.sh records both configs; the {flag-on post-share} /
// {flag-on pre-share} ratio is the §4.3/L6 revival trigger input.

const PER_ITERATION = 20000;

function Point(i) {
    // 8 transitions per construction: enough to leave inline capacity and
    // exercise out-of-line (butterfly) transitions on the tail of the chain.
    this.a = i;
    this.b = i + 1;
    this.c = i + 2;
    this.d = i + 3;
    this.e = i + 4;
    this.f = i + 5;
    this.g = i + 6;
    this.h = i + 7;
}

function constructMany(n) {
    let checksum = 0;
    for (let i = 0; i < n; ++i) {
        const p = new Point(i & 0xff);
        checksum += p.h;
    }
    return checksum;
}
noInline(constructMany);

let expected = 0;
for (let i = 0; i < PER_ITERATION; ++i)
    expected += (i & 0xff) + 7;

reportBench("construct-pre-share", function () {
    return constructMany(PER_ITERATION);
}, expected, 10, 30);

// Force-share: have a foreign thread WRITE to an instance of the chain's
// final structure. Flag-on with OM landed this fires/invalidates the chain's
// writeThreadLocal sets; under the GIL stub it is the semantic oracle for
// the same program.
const sharedInstance = new Point(1);
if (typeof Thread === "function") {
    const t = new Thread(function () {
        sharedInstance.a = 1000; // foreign write => SW path
        sharedInstance.h = 1007;
    });
    t.join();
    if (sharedInstance.h !== 1007)
        throw new Error("foreign write lost on shared instance");
} else {
    // No Thread global (flag-off build/config): keep the structure usage
    // identical so the two BENCH lines stay comparable across configs.
    sharedInstance.a = 1000;
    sharedInstance.h = 1007;
}

reportBench("construct-post-share", function () {
    return constructMany(PER_ITERATION);
}, expected, 10, 30);
