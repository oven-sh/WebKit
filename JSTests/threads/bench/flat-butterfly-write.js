// Serial-perf gate: writes to out-of-line (flat butterfly) properties.
//
// Replace-style PutById to existing out-of-line slots. Under the threads
// design, writes from the owning thread with valid writeThreadLocal
// watchpoints must compile to exactly today's store (no SW-bit check,
// no DCAS). This bench regresses if the store fast path grows.

(function() {
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
        for (var i = 0; i < 1000000; ++i) {
            o.p08 = i;
            o.p09 = i + 1;
            o.p10 = i + 2;
            o.p11 = i + 3;
            o.p12 = i + 4;
            o.p13 = i + 5;
            o.p14 = i + 6;
            o.p15 = i + 7;
        }
        return o.p08 + o.p09 + o.p10 + o.p11 + o.p12 + o.p13 + o.p14 + o.p15;
    }
    noInline(run);

    // After the last iteration (i = 999999): sum = 8*999999 + (0+1+...+7).
    reportBench("flat-butterfly-write", run, 8 * 999999 + 28);
})();
