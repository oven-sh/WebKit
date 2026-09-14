//@ requireOptions("--useJSThreads=1")
//@ threadsRequireGILOff
// The fast data-property copies - spread ({...o}), Object.assign with one or two
// sources, rest destructuring - read an object's slots under one structure
// check. GIL off another thread can delete from the object meanwhile; a slot it
// clears reads empty, and the copies stored that empty value into the copy
// (Debug: "ASSERTION FAILED: value" in validatePutOwnDataProperty; AUDIT R9-19).
// They now check afterwards that the object kept the structure they checked and
// that no value they read is empty, and copy property by property otherwise.
// Every copied value is the one its key was written with, or undefined for a
// slot deleted while it was read (SPEC-objectmodel D1).
load("../harness.js", "caller relative");

const WRITERS = 2;
const ROUNDS = 4000;
const o = { anchor: "anchor!" };
const gate = new Int32Array(new SharedArrayBuffer(8));

const writers = spawnN(WRITERS, (id) => {
    let i = 0;
    Atomics.add(gate, 1, 1);
    while (!Atomics.load(gate, 0)) {
        const k = "w" + id + "_" + (i & 63);
        o[k] = k + "!";
        if (i > 8)
            delete o["w" + id + "_" + ((i - 8) & 63)];
        ++i;
    }
    return i;
});

while (Atomics.load(gate, 1) < WRITERS) { }

function check(copy, how) {
    for (const k in copy) {
        const v = copy[k];
        const expected = k === "anchor" ? "anchor!" : k + "!";
        if (v !== expected && v !== undefined)
            throw new Error(how + ": " + k + " copied as " + String(v));
    }
}

for (let r = 0; r < ROUNDS; ++r) {
    switch (r & 3) {
    case 0:
        check({ ...o }, "spread");
        break;
    case 1:
        check(Object.assign({}, o), "Object.assign");
        break;
    case 2: {
        const { anchor, ...rest } = o;
        check(rest, "rest destructuring");
        break;
    }
    case 3:
        check(Object.assign({}, { anchor: "anchor!" }, o), "Object.assign with two sources");
        break;
    }
}

Atomics.store(gate, 0, 1);
joinAll(writers);
