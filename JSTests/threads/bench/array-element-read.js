// Serial-perf gate: contiguous array element reads.
//
// Array elements live on the right side of the butterfly, so GetByVal
// fast paths load the butterfly pointer raw on every access. The threads
// design must keep TTL arrays at today's speed (no extra indirection,
// no unmasking arithmetic when watchpoints hold).

(function() {
    var array = new Array(1024);
    for (var i = 0; i < array.length; ++i)
        array[i] = i & 7;

    function run() {
        var sum = 0;
        for (var i = 0; i < 2000000; ++i)
            sum += array[i & 1023];
        return sum;
    }
    noInline(run);

    // i & 1023 sweeps 0..1023 uniformly: 2000000/1024 = 1953.125 sweeps.
    var expected = 0;
    for (var i = 0; i < 2000000; ++i)
        expected += i & 7;

    reportBench("array-element-read", run, expected);
})();
