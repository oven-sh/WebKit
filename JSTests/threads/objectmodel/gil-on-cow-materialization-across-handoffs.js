//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// SPEC-objectmodel E4-G, history §38 (tenth round). GIL on, the thread that
// allocated an array literal materializes its CopyOnWrite butterfly on the
// first write as flag off does - copy, nuke, tagged word, new shape - without
// the cell lock, the StructureID claim and the 128-bit publication that the
// flag-on route used for every materialization (1,518 M cycles against 607 M
// flag off for two million four-element literals). A thread that did not
// allocate the literal still takes the locked route and fires the shape's
// thread-local sets. This test writes to fresh literals of the three
// CopyOnWrite shapes from their allocating thread and from another thread,
// with the GIL handed back and forth as often as it can be (timed waits,
// notifies, joins of short-lived threads), and checks every array's contents
// and length and that the literal itself (the shared immutable butterfly) is
// never changed. GIL off the locked route runs and the same checks apply.
load("../harness.js", "caller relative");

function int32Literal() { return [1, 2, 3, 4]; }
function doubleLiteral() { return [1.5, 2.5, 3.5, 4.5]; }
function contiguousLiteral() { return ["a", "b", "c", "d"]; }
noInline(int32Literal);
noInline(doubleLiteral);
noInline(contiguousLiteral);
const makers = [int32Literal, doubleLiteral, contiguousLiteral];
const originals = [[1, 2, 3, 4], [1.5, 2.5, 3.5, 4.5], ["a", "b", "c", "d"]];

function checkPristine(kind) {
    const fresh = makers[kind]();
    for (let i = 0; i < 4; ++i) {
        if (fresh[i] !== originals[kind][i])
            throw new Error("a fresh literal of kind " + kind + " has [" + i + "] = " + fresh[i] + ": a write reached the shared butterfly");
    }
    if (fresh.length !== 4)
        throw new Error("a fresh literal of kind " + kind + " has length " + fresh.length);
}

// The kinds of first write: in place, type-changing, appending, and by length.
function firstWrite(a, how, tag) {
    switch (how) {
    case 0: a[1] = a[1]; a[2] = tag; break; // in place (int32 into each shape: may convert the double/contiguous lanes' type, not the shape)
    case 1: a[0] = { tag }; break; // to Contiguous for the numeric shapes
    case 2: a[4] = tag; break; // append within the materialized copy's slack
    case 3: a.length = 2; break;
    case 4: a.push(tag, tag + 1); break;
    }
}
function verify(a, kind, how, tag, who) {
    const o = originals[kind];
    const fail = what => { throw new Error(who + ": kind " + kind + " write " + how + ": " + what + " (" + JSON.stringify(a) + ")"); };
    switch (how) {
    case 0:
        if (a.length !== 4 || a[0] !== o[0] || a[1] !== o[1] || a[2] !== tag || a[3] !== o[3]) fail("in-place write");
        break;
    case 1:
        if (a.length !== 4 || typeof a[0] !== "object" || a[0].tag !== tag || a[1] !== o[1] || a[3] !== o[3]) fail("object write");
        break;
    case 2:
        if (a.length !== 5 || a[4] !== tag || a[0] !== o[0] || a[3] !== o[3]) fail("append");
        break;
    case 3:
        if (a.length !== 2 || a[0] !== o[0] || a[1] !== o[1] || a[2] !== undefined) fail("length write");
        break;
    case 4:
        if (a.length !== 6 || a[4] !== tag || a[5] !== tag + 1 || a[3] !== o[3]) fail("push");
        break;
    }
}
noInline(firstWrite);
noInline(verify);

const gate = { main: 0, sibling: 0 }; // property-path waits: they park with the GIL released (the typed-array path may not block the main thread GIL on)
function handoff() {
    // Every one of these releases the GIL (GIL on) or is a plain blocking call (GIL off).
    Atomics.wait(gate, "main", 0, 0.05);
    Atomics.notify(gate, "sibling");
    new Thread(() => 1).join();
}

const ROUNDS = 300;
// Owner writes on the main thread, with a second thread doing the same on its own literals and handing the GIL over.
const sibling = new Thread(() => {
    for (let round = 0; round < ROUNDS; ++round) {
        for (let kind = 0; kind < 3; ++kind) {
            for (let how = 0; how < 5; ++how) {
                const a = makers[kind]();
                if (round & 1)
                    Atomics.wait(gate, "sibling", 0, 0.02);
                firstWrite(a, how, round);
                verify(a, kind, how, round, "sibling owner");
            }
        }
    }
    return "ok";
});
for (let round = 0; round < ROUNDS; ++round) {
    for (let kind = 0; kind < 3; ++kind) {
        for (let how = 0; how < 5; ++how) {
            const a = makers[kind]();
            if (!(round % 7))
                handoff();
            firstWrite(a, how, round + 1000);
            verify(a, kind, how, round + 1000, "main owner");
        }
        checkPristine(kind);
    }
}
if (sibling.join() !== "ok")
    throw new Error("sibling failed");

// Foreign first writes: the main thread allocates, another thread writes first, the main thread writes again.
for (let round = 0; round < 60; ++round) {
    const batch = [];
    for (let kind = 0; kind < 3; ++kind) {
        for (let how = 0; how < 5; ++how)
            batch.push({ a: makers[kind](), kind, how });
    }
    new Thread(() => {
        for (const entry of batch) {
            firstWrite(entry.a, entry.how, round + 5000);
            verify(entry.a, entry.kind, entry.how, round + 5000, "foreign writer");
        }
    }).join();
    for (const entry of batch) {
        verify(entry.a, entry.kind, entry.how, round + 5000, "allocating thread after a foreign write");
        entry.a.push(-1);
        if (entry.a[entry.a.length - 1] !== -1)
            throw new Error("the allocating thread's push after a foreign first write was lost");
    }
    for (let kind = 0; kind < 3; ++kind)
        checkPristine(kind);
}
