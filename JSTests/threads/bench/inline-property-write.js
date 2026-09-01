// Serial-perf gate: writes to inline-cell properties.
//
// Replace-style PutById to inline slots. The cell never resizes, so these
// stores are atomic by default and must not pick up any TID/SW checking
// under the threads object model.

(function() {
    function Point(x, y, z) {
        this.x = x;
        this.y = y;
        this.z = z;
    }

    var p = new Point(0, 0, 0);

    function run() {
        for (var i = 0; i < 2000000; ++i) {
            p.x = i;
            p.y = i + 1;
            p.z = i + 2;
        }
        return p.x + p.y + p.z;
    }
    noInline(run);

    // After the last iteration (i = 1999999): 3*1999999 + 3.
    reportBench("inline-property-write", run, 3 * 1999999 + 3);
})();
