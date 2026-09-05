//@ requireOptions("--useJSThreads=1", "--jsThreadsStopWatchdogMs=10000")
// A spread of a Set ([...set]) copies the keys in C++
// (JSCellButterfly::createFromSet). With the GIL off, another thread can
// rehash or clear the table while the copy walks it. The walk has to go on
// from the table it reached, as an iterator does. It used to start again from
// the first table with the entry it had reached in the new one, so each step
// moved the entry back by the deleted entries of the obsolete table, and the
// walk never ended: it appended the same key until a stop-the-world request
// from another thread ran into the watchdog.
//
// The writers delete and re-add keys, so the table always has deleted entries
// and rehashes often. The readers spread it. The main thread transitions an
// array's indexing shape while it waits, which needs a stop, so a reader that
// never returns fails the test through the watchdog instead of hanging.
load("../harness.js", "caller relative");

const KEYS = 1500;
const READERS = 2;
const WRITERS = 2;
const SPREADS = 100;
const ROUNDS = 200;

const set = new Set();
for (let i = 0; i < KEYS; ++i)
    set.add(i);

const gate = { done: 0 };

// Each thread counts itself done even when it throws, so that the main thread
// stops waiting and join() reports the error.
const readers = spawnN(READERS, () => {
    try {
        let longest = 0;
        for (let n = 0; n < SPREADS; ++n) {
            const copy = [...set];
            // A key that a writer deletes and adds back during one spread moves
            // to the end of the table, so the spread can see it again, as a
            // single-threaded iteration does. The length is not exact; it is
            // only checked against a runaway walk.
            if (copy.length > 50 * KEYS)
                throw new Error("reader: spread of " + copy.length + " keys");
            for (const key of copy) {
                if (typeof key !== "number" || !(key >= 0 && key < KEYS))
                    throw new Error("reader: unexpected key " + String(key));
            }
            longest = Math.max(longest, copy.length);
        }
        return longest;
    } finally {
        Atomics.add(gate, "done", 1);
    }
});

const writers = spawnN(WRITERS, (w) => {
    try {
        const base = w * 300;
        for (let round = 0; round < ROUNDS; ++round) {
            for (let i = 0; i < 200; ++i)
                set.delete(base + i);
            for (let i = 0; i < 200; ++i)
                set.add(base + i);
        }
        return ROUNDS;
    } finally {
        Atomics.add(gate, "done", 1);
    }
});

const deadline = Date.now() + 60000;
while (Atomics.load(gate, "done") < READERS + WRITERS) {
    if (Date.now() > deadline)
        throw new Error("threads did not finish in 60 s");
    const probe = new Array(1);
    probe[0] = 1; // Undecided to Int32: a stop-the-world with the flag on.
    sleepMs(2);
}

for (const longest of joinAll(readers))
    shouldBeTrue(longest >= KEYS - 400, "reader saw the set");
for (const rounds of joinAll(writers))
    shouldBe(rounds, ROUNDS, "writer rounds");
shouldBe(set.size, KEYS, "set.size at the end");
for (let i = 0; i < KEYS; ++i)
    shouldBeTrue(set.has(i), "set.has(" + i + ")");
