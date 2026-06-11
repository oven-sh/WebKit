// Serial-perf gate: reads of out-of-line (flat butterfly) properties.
//
// The threads object model tags the high bits of the butterfly pointer
// (TID + shared-write bit) and is supposed to elide the residual check
// entirely via the per-structure transitionThreadLocal/writeThreadLocal
// watchpoints. This bench regresses if that elision fails and butterfly
// loads pick up extra masking/branching.

(function() {
    // Force properties out of line: more named properties than inline
    // capacity (object literals get 6 inline slots by default; transitions
    // beyond capacity spill to the butterfly).
    function make(seed) {
        var o = {};
        o.p00 = seed + 0;
        o.p01 = seed + 1;
        o.p02 = seed + 2;
        o.p03 = seed + 3;
        o.p04 = seed + 4;
        o.p05 = seed + 5;
        o.p06 = seed + 6;
        o.p07 = seed + 7;
        o.p08 = seed + 8;
        o.p09 = seed + 9;
        o.p10 = seed + 10;
        o.p11 = seed + 11;
        o.p12 = seed + 12;
        o.p13 = seed + 13;
        o.p14 = seed + 14;
        o.p15 = seed + 15;
        return o;
    }

    var o = make(1);
    noInline(make);

    function run() {
        var sum = 0;
        for (var i = 0; i < 1000000; ++i)
            sum += o.p08 + o.p09 + o.p10 + o.p11 + o.p12 + o.p13 + o.p14 + o.p15;
        return sum;
    }
    noInline(run);

    // p08..p15 hold seed+8 .. seed+15 with seed=1 => 9..16, sum per iteration = 100.
    reportBench("flat-butterfly-read", run, 100000000);
})();
