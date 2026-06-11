// Serial-perf gate: megamorphic property access.
//
// 1000 distinct structures at one access site blows out the IC and lands
// on the megamorphic/generic path. The threads design touches exactly this
// machinery (Handler IC dispatch, structure lookup), so this bench guards
// the slow-but-hot tail: generic GetById must not pick up locking or
// extra indirection for TTL objects.
//
// Modeled on JSTests/microbenchmarks/megamorphic-load.js.

(function() {
    var array = [];
    for (var i = 0; i < 1000; ++i) {
        var o = {};
        o["i" + i] = i;  // Unique leading property forces a unique structure.
        o.f = 42;
        o.g = i;
        array.push(o);
    }

    function run() {
        var sum = 0;
        for (var i = 0; i < 1000000; ++i) {
            var o = array[i % 1000];
            sum += o.f + o.g;
        }
        return sum;
    }
    noInline(run);

    // Each sweep of 1000 objects contributes 1000*42 + (0+1+...+999).
    var expected = 1000 * (1000 * 42 + (999 * 1000 / 2));

    reportBench("megamorphic-access", run, expected);
})();
