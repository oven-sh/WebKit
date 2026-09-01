// Serial-perf gate: reads of inline-cell properties.
//
// Inline properties never touch the butterfly, so per THREAD.md they get
// concurrency for free — this path must be bit-for-bit today's code under
// the threads object model. Any regression here means the change leaked
// into the cell access path itself.

(function() {
    function Point(x, y, z) {
        this.x = x;
        this.y = y;
        this.z = z;
    }

    var p = new Point(3, 5, 7);

    function run() {
        var sum = 0;
        for (var i = 0; i < 2000000; ++i)
            sum += p.x + p.y + p.z;
        return sum;
    }
    noInline(run);

    reportBench("inline-property-read", run, 2000000 * 15);
})();
