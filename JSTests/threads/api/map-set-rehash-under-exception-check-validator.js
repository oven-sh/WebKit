//@ requireOptions("--useJSThreads=1", "--validateExceptionChecks=1")
// AUDIT R10-29 (tenth round). With the GIL off a Map or Set rehash copies the
// live entries under the table's cell lock (SPEC-ungil N.1) and hashes every
// copied key through a throw-scoped helper; the copy loop did not check after
// the hash, so with the exception-check validator on (a Debug-build facility;
// the option is inert in Release) the second string key of any rehash aborted
// the process: "Unchecked JS exception ... jsMapHashImpl". Two of Bun's tests
// run fixtures under the validator and failed GIL off on module load, in
// `new Set([...])`. GIL on and flag off run main's loop, which checks.
// Here: rehashes of string-keyed Sets and Maps, on the main thread and on two
// threads sharing one Map, with the validator on. The test passes by finishing
// (and checks the contents).
load("../harness.js", "caller relative");

function fill(n, tag) {
    const keys = [];
    for (let i = 0; i < n; ++i)
        keys.push(tag + "-key-" + i);
    const set = new Set(keys); // constructor path: forEachInIterable -> add -> rehash
    const map = new Map();
    for (const key of keys)
        map.set(key, key.length); // set path
    for (let i = 0; i < n; i += 3)
        set.delete(tag + "-key-" + i); // deleted entries, so the next rehash compacts
    for (let i = 0; i < n; ++i)
        set.add(tag + "-more-" + i);
    let live = 0;
    for (const key of set)
        ++live;
    shouldBe(live, n - Math.ceil(n / 3) + n);
    shouldBe(map.size, n);
    return set.size + map.size;
}

fill(100, "main");

const shared = new Map();
const threads = [];
for (let t = 0; t < 2; ++t) {
    threads.push(new Thread((id) => {
        let total = fill(200, "t" + id);
        for (let i = 0; i < 300; ++i)
            shared.set("shared-" + id + "-" + i, i);
        return total;
    }, t));
}
for (const thread of threads)
    thread.join();
shouldBe(shared.size, 600);
