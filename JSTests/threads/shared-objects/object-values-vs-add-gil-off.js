//@ requireOptions("--useJSThreads=1")
//@ threadsRequireGILOff
// Object.values' fast path counts an object's enumerable properties, allocates
// the result array for that count, then walks the object a second time and
// stores each value at the next index. GIL off another thread can add named
// properties or push elements between the two walks; the second walk re-read
// the object's structure and its indexed storage, so it stored more values than
// the result array holds - past the end of its butterfly (Release: a write
// outside the array; Debug: "ASSERTION FAILED: index == result->length()" or
// the contiguous storage's index assertion; AUDIT R10-5). The fast path now
// walks one structure snapshot both times, bounds the second walk by the count
// it allocated for, and re-validates afterwards (SPEC-objectmodel I42); on a
// mismatch it takes the generic path.
// Every value returned is one that some property of the object held.
load("../harness.js", "caller relative");

const WRITERS = 2;
const ROUNDS = 6000;
const gate = new Int32Array(new SharedArrayBuffer(8));
// Several objects, so that the readers meet objects in different phases: one
// with named properties only, one with elements too.
const named = { anchor: "anchor!" };
const mixed = { anchor: "anchor!" };
mixed[0] = "e0!";

const writers = spawnN(WRITERS, (id) => {
    let i = 0;
    Atomics.add(gate, 1, 1);
    while (!Atomics.load(gate, 0)) {
        const k = "w" + id + "_" + (i & 127);
        named[k] = k + "!";
        mixed[k] = k + "!";
        if (id === 0) {
            // Elements come and go at the end of the indexed storage.
            const n = 1 + (i & 31);
            mixed[n] = "e" + n + "!";
        }
        if (i > 16) {
            const old = "w" + id + "_" + ((i - 16) & 127);
            delete named[old];
            delete mixed[old];
        }
        ++i;
    }
    return i;
});

while (Atomics.load(gate, 1) < WRITERS) { }

function check(values, how) {
    if (!Array.isArray(values))
        throw new Error(how + ": not an array");
    for (let i = 0; i < values.length; ++i) {
        const v = values[i];
        // A value is the string its key was written with ("<key>!"), or
        // undefined for a slot deleted while it was read.
        if (v !== undefined && !(typeof v === "string" && v.endsWith("!")))
            throw new Error(how + ": values[" + i + "] is " + String(v));
    }
}

for (let r = 0; r < ROUNDS; ++r) {
    check(Object.values(named), "named");
    check(Object.values(mixed), "mixed");
}

Atomics.store(gate, 0, 1);
joinAll(writers);
