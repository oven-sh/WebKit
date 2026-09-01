// Serial-perf gate: transition-heavy object construction.
//
// Builds objects whose properties spill past inline capacity, exercising
// the structure-transition chain plus butterfly (re)allocation. Under the
// threads design, owner-thread transitions with valid
// transitionThreadLocal/writeThreadLocal watchpoints must proceed with no
// locking or CAS — this is exactly the path the watchpoint elision is
// supposed to keep at today's speed. The blog post's worst case here is
// ~7x; the gate holds it to <=1%.

(function() {
    function make(seed) {
        var o = {};
        o.a = seed;
        o.b = seed + 1;
        o.c = seed + 2;
        o.d = seed + 3;
        o.e = seed + 4;
        o.f = seed + 5;
        o.g = seed + 6;
        o.h = seed + 7;
        o.i = seed + 8;
        o.j = seed + 9;
        o.k = seed + 10;
        o.l = seed + 11;
        return o;
    }
    noInline(make);

    function run() {
        var sum = 0;
        for (var i = 0; i < 100000; ++i) {
            var o = make(i & 0xff);
            sum += o.a + o.l;
        }
        return sum;
    }
    noInline(run);

    // Per iteration: seed + (seed + 11) where seed = i & 0xff.
    var expected = 0;
    for (var i = 0; i < 100000; ++i)
        expected += 2 * (i & 0xff) + 11;

    // Protocol must match Tools/threads/baseline.json: the 54.918ms baseline
    // median was recorded (2026-06-05T08:59:40Z) with the harness default of
    // 50 measured iterations. Changing measuredIterations without a --record
    // on the same pre-threads reference build is a gate-protocol mismatch,
    // not a regression signal (see docs/threads/BENCH.md, "Compare like with
    // like" and "Writing a new benchmark" item 5: the gated count must stay
    // in lockstep with the protocol baseline.json was recorded under).
    // If a longer region is needed for perf attribution, run a local copy
    // with a larger count instead of editing the gated bench. The count is
    // kept explicit here to pin the gated protocol against future
    // harness-default drift.
    reportBench("transition-heavy-constructor", run, expected, 20, 50);
})();
