//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// Array.prototype.join's strings-and-Int32s fast joiner measures every lane,
// allocates a buffer of exactly that size, then RE-READS the lanes to fill it.
// GIL off another thread may store a longer string (or a longer integer) into
// a lane between the two passes, and the fill then writes past the measured
// buffer. GIL off the two-pass joiner is skipped (the one-pass joiner converts
// each lane as it reads it); GIL on nothing can run between the passes. The
// test hammers join on an array a second thread keeps rewriting with values
// of different printed lengths and checks every result is made only of values
// that were stored.
load("../harness.js", "caller relative");

const N = 2048;
const shortS = "a", longS = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
const strs = new Array(N).fill(shortS); strs[0] = shortS; // Contiguous
const ints = []; for (let i = 0; i < N; ++i) ints.push(1); // Int32

const box = { stop: 0 };
const writer = new Thread(() => {
    let n = 0;
    while (!Atomics.load(box, "stop")) {
        const toLong = !!(n & 1);
        for (let i = 0; i < N; ++i) { strs[i] = toLong ? longS : shortS; ints[i] = toLong ? 2147483647 : 1; }
        ++n;
    }
    return n;
});

const okStr = new RegExp("^(?:" + shortS + "|" + longS + ")+$");
const okStrComma = new RegExp("^(?:(?:" + shortS + "|" + longS + "),)*(?:" + shortS + "|" + longS + ")$");
const okInt = /^(?:1|2147483647)+$/;
const okIntComma = /^(?:(?:1|2147483647),)*(?:1|2147483647)$/;
const deadline = Date.now() + 4000; // the race needs volume, not wall time: bound it for slow (Debug, TSAN) builds
for (let i = 0; i < 3000 && (i < 200 || Date.now() < deadline); ++i) {
    const s1 = strs.join(""), s2 = strs.join(","), s3 = ints.join(""), s4 = ints.join(",");
    if (!okStr.test(s1)) throw new Error("join('') of strings: " + s1.slice(0, 80));
    if (!okStrComma.test(s2)) throw new Error("join(',') of strings: " + s2.slice(0, 80));
    if (!okInt.test(s3)) throw new Error("join('') of ints: " + s3.slice(0, 80));
    if (!okIntComma.test(s4)) throw new Error("join(',') of ints: " + s4.slice(0, 80));
}
Atomics.store(box, "stop", 1);
writer.join();
print("PASS");
