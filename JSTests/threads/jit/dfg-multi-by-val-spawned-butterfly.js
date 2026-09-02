//@ requireOptions("--useJSThreads=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=50")
//@ runDefault()
//@ runDefault("--useFTLJIT=0")
// A get or put whose profile mixes a JSArray with a typed array compiles to
// MultiGetByVal / MultiPutByVal. A butterfly installed by a spawned thread
// carries that thread's ID in its high bits, so the DFG must decode the word
// before it indexes through it. Both the spawned thread (which owns the
// butterfly) and the main thread (which does not) run the optimized code.

load("../harness.js", "caller relative");

function get(a, i) { return a[i]; }
noInline(get);
function put(a, i, v) { a[i] = v; }
noInline(put);

function drive(ints, doubles, objs, typed) {
    let sum = 0;
    for (let n = 0; n < 20000; ++n) {
        const i = n & 3;
        put(ints, i, i + 1);
        put(doubles, i, i + 2);
        put(typed, i, i + 3);
        sum += get(ints, i) + get(doubles, i) + get(typed, i);
        if (get(objs, i) !== objs[i])
            throw new Error("bad object element at " + i);
    }
    return sum;
}

// Each iteration adds (i+1) + (i+2) + (i+3) = 3i + 6 for i = n & 3, so each
// group of four adds 42.
const expected = 20000 / 4 * 42;

function makeArrays() {
    const o = {};
    return {
        ints: [0, 0, 0, 0],
        doubles: [0.5, 0.5, 0.5, 0.5],
        objs: [o, o, o, o],
        typed: new Float64Array(4),
    };
}

const spawned = new Thread(() => {
    const arrays = makeArrays();
    const sum = drive(arrays.ints, arrays.doubles, arrays.objs, arrays.typed);
    return { sum, arrays };
}).join();
shouldBe(spawned.sum, expected);

// The spawned thread's arrays are foreign to the main thread.
const a = spawned.arrays;
shouldBe(drive(a.ints, a.doubles, a.objs, a.typed), expected);
shouldBe(a.ints[3], 4);
shouldBe(a.doubles[3], 5);
