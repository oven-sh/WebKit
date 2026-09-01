//@ requireOptions("--useJSThreads=1")
// Holey (sparse) arrays shared across threads: reading holes, creating holes
// with delete, filling holes, and prototype fallthrough through holes.
load("../resources/assert.js", "caller relative");

// --- Foreign-thread reads of holes ---

const holey = [0, , 2, , 4]; // holes at 1 and 3
shouldBe(holey.length, 5);
shouldBe(new Thread(arr => arr[1], holey).join(), undefined);
shouldBe(new Thread(arr => arr[3], holey).join(), undefined);
shouldBe(new Thread(arr => arr[4], holey).join(), 4);
shouldBe(new Thread(arr => 1 in arr, holey).join(), false);
shouldBe(new Thread(arr => 0 in arr, holey).join(), true);
shouldBe(new Thread(arr => arr.hasOwnProperty(3), holey).join(), false);

// Holey double arrays.
const holeyDouble = [0.5, , 2.5];
shouldBe(new Thread(arr => arr[1], holeyDouble).join(), undefined);
shouldBe(new Thread(arr => arr[2], holeyDouble).join(), 2.5);

// --- Foreign thread creates a hole with delete ---

const toDelete = [10, 20, 30];
shouldBe(new Thread(arr => delete arr[1], toDelete).join(), true);
shouldBe(toDelete.length, 3);
shouldBe(toDelete[1], undefined);
shouldBeFalse(1 in toDelete);
shouldBe(toDelete[0], 10);
shouldBe(toDelete[2], 30);

// --- Foreign thread fills a hole ---

const toFill = [1, , 3];
shouldBeFalse(1 in toFill);
new Thread(arr => { arr[1] = 2; }, toFill).join();
shouldBeTrue(1 in toFill);
shouldBe(toFill[1], 2);
shouldBe(toFill.length, 3);

// --- Prototype fallthrough through a hole, mutated from a foreign thread ---

const fallthrough = [, , ,];
shouldBe(fallthrough.length, 3);
new Thread(() => { Array.prototype[1] = "from-proto"; }).join();
try {
    shouldBe(fallthrough[1], "from-proto");
    // An own element shadows the prototype value, even when stored by a
    // foreign thread.
    new Thread(arr => { arr[1] = "own"; }, fallthrough).join();
    shouldBe(fallthrough[1], "own");
    shouldBe(new Thread(arr => arr[1], fallthrough).join(), "own");
    // Deleting the own element re-exposes the prototype value.
    new Thread(arr => { delete arr[1]; }, fallthrough).join();
    shouldBe(fallthrough[1], "from-proto");
    shouldBe(new Thread(arr => arr[1], fallthrough).join(), "from-proto");
} finally {
    delete Array.prototype[1];
}
shouldBe(fallthrough[1], undefined);

// --- Far out-of-bounds store from a foreign thread creates a sparse array ---

const sparse = [0];
new Thread(arr => { arr[1000000] = "sparse"; }, sparse).join();
shouldBe(sparse.length, 1000001);
shouldBe(sparse[1000000], "sparse");
shouldBe(sparse[500000], undefined);
shouldBeFalse(500000 in sparse);
shouldBe(new Thread(arr => arr[1000000], sparse).join(), "sparse");

// --- Iteration semantics over shared holey arrays ---

const iterated = [1, , 3, , 5];
// forEach skips holes; map preserves them; for..of reads undefined.
shouldBe(new Thread(arr => {
    let visited = 0;
    arr.forEach(() => { ++visited; });
    return visited;
}, iterated).join(), 3);
shouldBe(new Thread(arr => {
    let count = 0;
    for (const x of arr) {
        if (x === undefined)
            ++count;
    }
    return count;
}, iterated).join(), 2);
shouldBe(new Thread(arr => Object.keys(arr).join(","), iterated).join(), "0,2,4");

// --- Concurrent hole punching on disjoint indices under a lock ---

const lock = new Lock();
const punched = new Array(64).fill(7);
joinAll(spawnN(4, index => {
    for (let i = index; i < 64; i += 4) {
        if (i % 2 === 0)
            lock.hold(() => { delete punched[i]; });
    }
}));
shouldBe(punched.length, 64);
for (let i = 0; i < 64; ++i) {
    if (i % 2 === 0) {
        shouldBeFalse(i in punched, "expected hole at " + i);
        shouldBe(punched[i], undefined);
    } else {
        shouldBeTrue(i in punched, "expected element at " + i);
        shouldBe(punched[i], 7);
    }
}

// Refill all holes from foreign threads; array becomes dense again.
joinAll(spawnN(4, index => {
    for (let i = index; i < 64; i += 4) {
        if (i % 2 === 0)
            punched[i] = i;
    }
}));
for (let i = 0; i < 64; ++i) {
    shouldBeTrue(i in punched);
    shouldBe(punched[i], i % 2 === 0 ? i : 7);
}

// --- delete then re-add must not expose a stale value to a third thread ---

const recycle = ["old"];
new Thread(arr => { delete arr[0]; }, recycle).join();
new Thread(arr => { arr[0] = "new"; }, recycle).join();
shouldBe(new Thread(arr => arr[0], recycle).join(), "new");
shouldBe(recycle[0], "new");
