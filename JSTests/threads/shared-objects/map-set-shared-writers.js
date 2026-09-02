//@ requireOptions("--useJSThreads=1")
// Several threads write one shared Map and one shared Set at once: set, add,
// delete, clear, and iteration. The hash table grows and rehashes while the
// others write, so each operation must see the table whole.
//
// Every thread uses its own keys, so each thread can check its own entries at
// the end. A crash, a lost entry, or a foreign value is a failure.
load("../harness.js", "caller relative");

const THREADS = 4;
const KEYS = 400;
const ROUNDS = 6;

for (let round = 0; round < ROUNDS; ++round) {
    const map = new Map();
    const set = new Set();
    const scratch = new Map();

    // A reader that walks both tables in every way while the writers run. It
    // checks what holds under any interleaving: every entry it sees is one that
    // a writer added, with the value that writer stores for that key.
    // It walks a fixed number of times, so it never waits for the writers: with
    // the GIL on, a thread that spins on shared state never lets them run.
    const reader = new Thread(() => {
        function checkMapEntry(key, value) {
            if (typeof key === "number") {
                const t = Math.floor(key / 100000);
                if (value !== "v" + t + "_" + (key % 100000))
                    throw new Error("reader: wrong value " + value + " for key " + key);
            } else if (typeof key !== "string" || typeof value !== "number")
                throw new Error("reader: unexpected entry " + String(key) + " -> " + String(value));
        }
        let walks = 0;
        while (walks < 40) {
            for (const [key, value] of map)
                checkMapEntry(key, value);
            map.forEach((value, key) => checkMapEntry(key, value));
            for (const key of Array.from(map.keys()))
                map.get(key);
            for (const key of [...set]) {
                if (typeof key !== "number")
                    throw new Error("reader: unexpected set key " + String(key));
            }
            set.forEach((key) => set.has(key));
            if (map.size < 0 || set.size < 0)
                throw new Error("reader: negative size");
            ++walks;
        }
        return walks;
    });

    const results = joinAll(spawnN(THREADS, (t) => {
        const base = t * 100000;
        for (let i = 0; i < KEYS; ++i) {
            map.set(base + i, "v" + t + "_" + i);
            map.set("s" + t + "_" + i, i);
            set.add(base + i);
            scratch.set(base + i, i);
            if (i % 7 === 0)
                scratch.delete(base + i - 1);
            if (i % 50 === 0)
                scratch.clear();
        }
        for (let i = 0; i < KEYS; i += 2) {
            map.delete(base + i);
            set.delete(base + i);
        }
        let seen = 0;
        for (const [key, value] of map) {
            if (typeof key === "number" && key >= base && key < base + KEYS) {
                if (value !== "v" + t + "_" + (key - base))
                    throw new Error("thread " + t + ": wrong value " + value + " for key " + key);
                ++seen;
            }
        }
        return seen;
    }));

    shouldBe(reader.join(), 40, "round " + round + ": walks the reader made");

    for (let t = 0; t < THREADS; ++t) {
        const base = t * 100000;
        shouldBe(results[t], KEYS / 2, "round " + round + ": entries thread " + t + " saw in its own range");
        for (let i = 0; i < KEYS; ++i) {
            const kept = i % 2 === 1;
            shouldBe(map.has(base + i), kept, "round " + round + ": map.has(" + (base + i) + ")");
            shouldBe(set.has(base + i), kept, "round " + round + ": set.has(" + (base + i) + ")");
            if (kept)
                shouldBe(map.get(base + i), "v" + t + "_" + i, "round " + round + ": map.get(" + (base + i) + ")");
            shouldBe(map.get("s" + t + "_" + i), i, "round " + round + ": string key of thread " + t);
        }
    }
    shouldBe(map.size, THREADS * (KEYS / 2 + KEYS), "round " + round + ": map.size");
    shouldBe(set.size, THREADS * KEYS / 2, "round " + round + ": set.size");
}
