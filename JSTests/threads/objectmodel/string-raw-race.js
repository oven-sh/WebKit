//@ requireOptions("--useJSThreads=1", "--verifyConcurrentButterfly=1")
//@ threadsRequireGILOff
// String.raw reads template.raw one index at a time through a fast path.
// Another thread changes that array while String.raw runs.
// Dense part: the writer pushes, pops, shrinks, and changes the indexing type.
// A foreign push past the vector also makes the word segmented, and the fast
// path must not read that word as flat storage.
// Sparse part: the writer adds and removes entries in the sparse map of an
// ArrayStorage array, which rehashes the map. The reader must not walk a
// freed table.
load("../harness.js", "caller relative");

const MAX_LENGTH = 40;
// More substitutions than either array can have literals, so every segment is
// followed by a separator.
const SEPARATORS = "|".repeat(256);
const shared = { dense: null, sparse: null, round: 0, stop: 0 };

function isDenseSegment(segment) {
    return segment === "undefined" || segment === "m" || /^s?\d+(\.5)?$/.test(segment);
}

const writer = new Thread(() => {
    let seen = 0;
    let step = 0;
    while (Atomics.load(shared, "stop") === 0) {
        const round = Atomics.load(shared, "round");
        if (round === seen)
            continue;
        seen = round;
        const dense = shared.dense;
        const sparse = shared.sparse;
        for (let i = 0; i < 48; ++i) {
            ++step;
            if (dense.length < MAX_LENGTH - 4)
                dense.push("s" + step, "s" + (step + 1));
            if (step % 5 === 0)
                dense.pop();
            if (step % 9 === 0)
                dense.length = step % 4;
            if (step % 23 === 0 && dense.length)
                dense[0] = step + 0.5;
            if (step % 31 === 0 && dense.length)
                dense[dense.length - 1] = "m";
            const key = 100 + (step % 64);
            if (step & 64)
                delete sparse[key];
            else
                sparse[key] = "w";
        }
    }
});

// An ArrayStorage array in sparse mode: entries below SPARSE_LENGTH live in
// the sparse map and the writer never touches them.
const SPARSE_LENGTH = 8;
function makeSparse() {
    const array = [];
    Object.defineProperty(array, "0", { value: "e0", writable: true, enumerable: true, configurable: false });
    for (let i = 1; i < SPARSE_LENGTH; ++i)
        array[i] = "e" + i;
    return array;
}

const deadline = Date.now() + 700;
let calls = 0;
for (let round = 1; Date.now() < deadline; ++round) {
    // A constant literal is copy-on-write, and the writer's first push gives it a
    // flat butterfly that the writer owns, so it stays flat. The other literal
    // segments on the writer's first push past the vector.
    const dense = (round & 1) ? ["s0", "s1", "s2"] : ["s" + round, "s1", "s2"];
    const sparse = makeSparse();
    shared.dense = dense;
    shared.sparse = sparse;
    Atomics.store(shared, "round", round);
    for (let i = 0; i < 10; ++i) {
        // A length of 0 gives "", which has no segments.
        const denseResult = String.raw({ raw: dense }, ...SEPARATORS);
        const denseSegments = denseResult === "" ? [] : denseResult.split("|");
        if (denseSegments.length > MAX_LENGTH)
            throw new Error("String.raw read too many segments: " + denseSegments.length);
        for (const segment of denseSegments) {
            if (!isDenseSegment(segment))
                throw new Error("String.raw produced a segment nobody wrote: " + segment);
        }

        const sparseSegments = String.raw({ raw: sparse }, ...SEPARATORS).split("|");
        if (sparseSegments.length < SPARSE_LENGTH)
            throw new Error("String.raw lost sparse segments: " + sparseSegments.length);
        for (let j = 0; j < SPARSE_LENGTH; ++j) {
            if (sparseSegments[j] !== "e" + j)
                throw new Error("sparse segment " + j + " is " + sparseSegments[j]);
        }
        for (let j = SPARSE_LENGTH; j < sparseSegments.length; ++j) {
            if (sparseSegments[j] !== "w" && sparseSegments[j] !== "undefined")
                throw new Error("sparse segment " + j + " is " + sparseSegments[j]);
        }
        calls += 2;
    }
}
Atomics.store(shared, "stop", 1);
writer.join();
shouldBeTrue(calls > 0, "reader made progress");
