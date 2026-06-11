// SPEC-heap.md T10: option-off allocation bench for the I10 serial-perf gate.
//
// Runs with NO options (useSharedGCHeap defaults off): this is the
// configuration the I10 invariant says must be byte-for-byte today's code on
// the allocation paths — gated branches only. The benchmark hammers exactly
// the paths the heap workstream touched:
//   - inline/LocalAllocator fast paths and allocateSlowCase (MSPL is a no-op
//     locker option-off),
//   - the atomic accounting counters (§5.4; relaxed loads/stores),
//   - eden/full collection scheduling (activity gating is ISS-only),
//   - precise allocation (§5.6; lock branch not taken).
//
// Output format matches JSTests/threads/bench/harness.js (a single
// "BENCH <name> <ms>" line), so Tools/threads/bench-gate.sh can median it
// against baseline.json once this file is added to the gate's list (it globs
// bench/*.js; see INTEGRATE-heap.md T10 notes).
//
// NOTE: timing output is inherently nondeterministic — exclude this file
// from Tools/threads/amplify.sh divergence campaigns (AMPLIFIER.md corpus
// rule); it is a bench-gate input, not a race-amplifier target. The
// embedded checksum still makes correctness failures loud.
load("./bench/harness.js", "caller relative");

(function() {
    // Object + array churn across several size classes, with enough garbage
    // per iteration to drive real block handout and eden collections.
    function churn() {
        var checksum = 0;
        for (var i = 0; i < 20000; ++i) {
            var o = { a: i, b: i ^ 7, c: null };
            o.c = [i, i + 1, i + 2, i + 3];
            checksum = (checksum + o.a + o.b + o.c[3]) | 0;
        }
        // A few large (precise-path) allocations per iteration.
        for (var j = 0; j < 4; ++j) {
            var big = new Float64Array(16 * 1024);
            big[0] = j;
            big[big.length - 1] = j * 2;
            checksum = (checksum + big[0] + big[big.length - 1]) | 0;
        }
        return checksum;
    }

    // Expected checksum (deterministic; validated every iteration by the
    // harness): computed by the same arithmetic, kept literal so a behavior
    // change (not just a perf change) also fails the gate.
    var expected = (function() {
        var checksum = 0;
        for (var i = 0; i < 20000; ++i)
            checksum = (checksum + i + (i ^ 7) + (i + 3)) | 0;
        for (var j = 0; j < 4; ++j)
            checksum = (checksum + j + j * 2) | 0;
        return checksum;
    })();

    reportBench("heap-allocation-churn", churn, expected);
})();
