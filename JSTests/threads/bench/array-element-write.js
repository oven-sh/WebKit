// Serial-perf gate: contiguous array element writes (in-bounds, no growth).
//
// In-bounds PutByVal to a contiguous array must stay a bare store under
// the threads object model. Growth/resize takes the CAS path per the
// design, but in-bounds stores to a TTL array must not.

(function() {
    var array = new Array(1024);
    for (var i = 0; i < array.length; ++i)
        array[i] = 0;

    function run() {
        for (var i = 0; i < 2000000; ++i)
            array[i & 1023] = i;
        var sum = 0;
        for (var i = 0; i < 1024; ++i)
            sum += array[i];
        return sum;
    }
    noInline(run);

    // Final value of slot k is the last i with (i & 1023) == k:
    // i = 1998848 + k for k < 1152-1024... compute directly instead.
    var final = new Array(1024);
    for (var i = 0; i < 2000000; ++i)
        final[i & 1023] = i;
    var expected = 0;
    for (var i = 0; i < 1024; ++i)
        expected += final[i];

    reportBench("array-element-write", run, expected);
})();
