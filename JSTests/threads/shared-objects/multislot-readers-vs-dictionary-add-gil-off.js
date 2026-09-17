//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
//@ threadsRequireGILOff
// SPEC-objectmodel I42, history §41 (AUDIT R10-24). A reader that lists a
// structure's properties and then takes each slot by its listed offset can,
// GIL off, read an EMPTY value: a dictionary's in-place add enters the
// property into the table and stores the value a few instructions later, both
// under the writer's cell lock, which these readers do not take, and the
// dictionary keeps its structure ID. The ninth round gave the fast
// data-property copies the rule (re-check the structure ID and non-emptiness
// after the read, else read property by property); the general JSON
// stringifier, the fast one, the Object.entries and the Object.defineProperties
// fast paths were missed. The stringifiers and defineProperties dereference
// what they read: signal 11 in Stringifier::appendStringifiedValue
// (cve/mc-val-multislot-clone.js under the amplifier on a loaded machine, on
// every build back to the round's first).
//
// The window is a few instructions wide, and the slot in it is the LAST one
// listed, so a reader of a large object reaches it microseconds too late unless
// the scheduler has taken the writer off its core. This test keeps the objects
// small instead: main makes a fresh one-property object a cacheable dictionary
// ($vm.toCacheableDictionary), publishes it, and runs the readers on it over and
// over while two threads add a few properties each, in place; then the next
// object. Every value encodes its key, so a wrong value is detected as well as
// a crash; a property missing from a result is allowed (the staleness model).
//
// The same window on a REUSED offset shows a well-formed residue instead of an
// empty value (what a deleted property left in a slot the quarantine has since
// released), which no after-read check can tell from a value; that is why these
// readers refuse dictionaries GIL off (history §41, amendment). Every object
// here is therefore prepared with deleted properties whose slots a collection
// has released before it becomes a dictionary, so the writers' adds land in
// them; nothing is ever deleted afterwards, so `undefined` under a listed key
// is a finding too, not the staleness the delete tests allow.
load("../harness.js", "caller relative");

const WRITERS = 2;
const OBJECTS = 6000;
const ADDS_PER_OBJECT = 6; // per writer
const READS_PER_OBJECT = 24;
const TIME_BUDGET_MS = 8000;

function descriptorFor(k) { return { value: k + "!", enumerable: true, configurable: true, writable: true }; }

// Objects with holes: HOLES properties added and deleted again, so their slots are quarantined; the collection below
// releases them for reuse. Prepared up front because the release needs a collection.
const HOLES = 2 * ADDS_PER_OBJECT;
function withHoles(first) {
    const o = { anchor: first };
    for (let h = 0; h < HOLES; ++h)
        o["hole" + h] = "hole" + h + "!";
    for (let h = 0; h < HOLES; ++h)
        delete o["hole" + h];
    return o;
}
const pool = [];
for (let n = 0; n < OBJECTS + 1; ++n)
    pool.push({ o: withHoles("anchor!"), d: withHoles(descriptorFor("anchor")) });
fullGC();

function fresh() {
    const pair = pool.pop();
    $vm.toCacheableDictionary(pair.o);
    $vm.toCacheableDictionary(pair.d);
    return pair;
}

const box = { current: fresh() };
const gate = { started: 0, stop: 0 };

const writers = spawnN(WRITERS, (id) => {
    Atomics.add(gate, "started", 1);
    let seen = null;
    let added = 0;
    let total = 0;
    while (!Atomics.load(gate, "stop")) {
        const current = box.current;
        if (current !== seen) {
            seen = current;
            added = 0;
        }
        if (added >= ADDS_PER_OBJECT)
            continue; // wait for the next pair of objects
        const k = "w" + id + "_" + added;
        current.o[k] = k + "!";
        current.d[k] = descriptorFor(k);
        ++added;
        ++total;
    }
    return total;
});

while (Atomics.load(gate, "started") < WRITERS) { }

function expected(k) { return k === "anchor" ? "anchor!" : k + "!"; }

function check(k, v, how) {
    if (v !== expected(k))
        throw new Error(how + ": " + k + " read as " + String(v));
}

function checkJSON(text, how) {
    const parsed = JSON.parse(text);
    for (const k in parsed)
        check(k, parsed[k], how);
    if (parsed.anchor !== "anchor!")
        throw new Error(how + ": the anchor is missing");
}

const identity = (key, value) => value;
const start = preciseTime();
let reads = 0;
for (let n = 0; n < OBJECTS && (preciseTime() - start) * 1000 < TIME_BUDGET_MS; ++n) {
    const pair = fresh();
    box.current = pair;
    const { o, d } = pair;
    for (let r = 0; r < READS_PER_OBJECT; ++r, ++reads) {
        switch (reads % 5) {
        case 0:
            checkJSON(JSON.stringify(o), "JSON.stringify");
            break;
        case 1:
            checkJSON(JSON.stringify(o, identity), "JSON.stringify with a replacer function");
            break;
        case 2:
            checkJSON(JSON.stringify(o, null, 1), "JSON.stringify with a gap");
            break;
        case 3:
            for (const [k, v] of Object.entries(o))
                check(k, v, "Object.entries");
            break;
        case 4: {
            const target = Object.defineProperties({}, d);
            for (const k in target)
                check(k, target[k], "Object.defineProperties");
            break;
        }
        }
    }
}

Atomics.store(gate, "stop", 1);
let adds = 0;
for (const writer of writers)
    adds += writer.join();
if (adds < WRITERS * ADDS_PER_OBJECT * 20) // twenty objects' worth: on a loaded machine the time budget ends the run early
    throw new Error("the writers added only " + adds + " properties: the readers did not race anything");
