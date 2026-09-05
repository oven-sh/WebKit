//@ requireOptions("--useJSThreads=1")
// Several threads write one shared WeakMap and one shared WeakSet at once:
// set, add, delete, and lookups. The tables grow, shrink and rehash while the
// others use them, so each operation must see a table whole; a rehash frees
// the old table.
//
// Every thread uses its own keys, so each thread can check its own entries at
// the end. A crash, a lost entry, or a foreign value is a failure.
load("../harness.js", "caller relative");

const THREADS = 4;
const KEYS = 600;
const ROUNDS = 6;

for (let round = 0; round < ROUNDS; ++round) {
    const map = new WeakMap();
    const set = new WeakSet();
    const scratch = new WeakMap();
    // Keys are objects; each thread gets its own, made here so that they are
    // shared cells like the tables.
    const keys = [];
    for (let t = 0; t < THREADS + 1; ++t) {
        const mine = [];
        for (let i = 0; i < KEYS; ++i)
            mine.push({ t, i });
        keys.push(mine);
    }

    // A reader that probes both tables while the writers run. Whatever it
    // finds for a writer's key is either nothing yet or that writer's value.
    const reader = new Thread(() => {
        let probes = 0;
        for (let walk = 0; walk < 60; ++walk) {
            for (let t = 0; t < THREADS; ++t) {
                for (let i = 0; i < KEYS; i += 3) {
                    const key = keys[t][i];
                    const value = map.get(key);
                    if (value !== undefined && value !== "v" + t + "_" + i)
                        throw new Error("reader: wrong value " + value + " for key " + t + "/" + i);
                    set.has(key);
                    scratch.has(key);
                    ++probes;
                }
            }
        }
        return probes;
    });

    const results = joinAll(spawnN(THREADS, (t) => {
        const mine = keys[t];
        for (let i = 0; i < KEYS; ++i) {
            map.set(mine[i], "v" + t + "_" + i);
            set.add(mine[i]);
            scratch.set(mine[i], i);
            if (i % 7 === 0 && i > 0)
                scratch.delete(mine[i - 1]);
            if (i % 50 === 49) {
                // Delete and re-add a run of keys, so the tables shrink too.
                for (let j = i - 40; j < i; ++j) {
                    map.delete(mine[j]);
                    set.delete(mine[j]);
                }
                for (let j = i - 40; j < i; ++j) {
                    map.set(mine[j], "v" + t + "_" + j);
                    set.add(mine[j]);
                }
            }
        }
        let ok = 0;
        for (let i = 0; i < KEYS; ++i) {
            if (map.get(mine[i]) === "v" + t + "_" + i && set.has(mine[i]))
                ++ok;
        }
        return ok;
    }));

    shouldBeTrue(reader.join() > 0, "reader probes, round " + round);
    for (let t = 0; t < THREADS; ++t)
        shouldBe(results[t], KEYS, "entries thread " + t + " finds at the end, round " + round);
    // The main thread's view after the joins.
    for (let t = 0; t < THREADS; ++t) {
        for (let i = 0; i < KEYS; ++i) {
            shouldBe(map.get(keys[t][i]), "v" + t + "_" + i, "map value " + t + "/" + i + ", round " + round);
            shouldBeTrue(set.has(keys[t][i]), "set has " + t + "/" + i + ", round " + round);
        }
    }
    for (let i = 0; i < KEYS; ++i)
        shouldBe(map.get(keys[THREADS][i]), undefined, "unused key " + i + ", round " + round);
}
