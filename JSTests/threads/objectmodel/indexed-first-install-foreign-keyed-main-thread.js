//@ requireOptions("--useJSThreads=1")
// A butterfly-less object keys its first indexed install on the structure's
// transition TID. Object.create(proto) on a spawned thread creates a fresh
// root structure owned by that thread; the first indexed store from the main
// thread (TID 0, the same TID a null butterfly word decodes to) is therefore
// a foreign-keyed install and must take the shared stop-the-world leg of
// createInitialIndexedStorageConcurrent, not the lock-free owner leg.
load("../harness.js", "caller relative");

function makeOnSpawnedThread(extra) {
    return new Thread((extra) => {
        const proto = { p: "proto" };
        const o = Object.create(proto);
        if (extra)
            o.a = 1; // inline property: still butterfly-less
        return o;
    }, extra).join();
}

const cases = [
    ["int32", 7, (o) => { o[0] = 7; }],
    ["double", 1.5, (o) => { o[0] = 1.5; }],
    ["contiguous", "s", (o) => { o[0] = "s"; }],
    ["far index", "far", (o) => { o[5] = "far"; }],
];

for (const [label, expected, install] of cases) {
    for (const extra of [false, true]) {
        const o = makeOnSpawnedThread(extra);
        shouldBe(o.p, "proto", label + ": prototype");
        install(o);
        const index = label === "far index" ? 5 : 0;
        shouldBe(o[index], expected, label + " (extra=" + extra + "): first indexed install from the main thread");
        shouldBe(Object.keys(o).indexOf(String(index)) >= 0, true, label + ": index is an own enumerable key");
        if (extra)
            shouldBe(o.a, 1, label + ": inline property survives the install");
        // The object keeps working after the install: more indexed stores, a
        // named add, and a read back of everything.
        o[index + 1] = "next";
        o.b = 2;
        shouldBe(o[index + 1], "next", label + ": second indexed store");
        shouldBe(o.b, 2, label + ": named add after the install");
        shouldBe(o[index], expected, label + ": first element intact");
        shouldBe(Object.getPrototypeOf(o).p, "proto", label + ": prototype intact");
    }
}

// The same install performed by the owning spawned thread stays on the owner
// path and yields the same observable object.
{
    const o = new Thread(() => {
        const proto = { p: "proto" };
        const o = Object.create(proto);
        o[0] = 7;
        o[1] = 1.5;
        return o;
    }).join();
    shouldBe(o[0], 7);
    shouldBe(o[1], 1.5);
    o[2] = "main";
    shouldBe(o[2], "main");
}
