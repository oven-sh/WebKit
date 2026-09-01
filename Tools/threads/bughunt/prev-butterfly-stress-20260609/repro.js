//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// repro.js — instrumented copy of JSTests/threads/jit/spawned-thread-butterfly-stress.js
// (bughunt evidence pack). Oracle identical; on mismatch we dump rich state:
//   - the got/want decode (tid/serial/prop)
//   - immediate re-reads of the same property (same access form + megamorphic forms)
//   - all sibling named props p0..p23 read fresh
//   - $vm.dumpCell(o) (structure ID + butterfly pointer) and indexingMode
//   - a second full re-check after a 5ms sleep (does corruption persist?)

load("../../../JSTests/threads/harness.js", "caller relative");

const THREADS = 4;
const OBJECTS_PER_THREAD = 32;
const PROPS = 24; // out-of-line for any inline capacity
const ROUNDS = 60;

const registry = [];
for (let t = 0; t <= THREADS; ++t)
    registry.push([]);
const ready = { count: 0 };
const shared = { hits: 0 };

function buildOne(tid, serial) {
    const o = { tid: tid, serial: serial };
    for (let p = 0; p < PROPS; ++p)
        o["p" + p] = tid * 1000000 + serial * 1000 + p;
    const a = [];
    for (let p = 0; p < PROPS; ++p)
        a[p] = tid * 1000000 + serial * 1000 + p;
    o.indexed = a;
    return o;
}
noInline(buildOne);

function decode(v) {
    if (typeof v !== "number")
        return "non-number(" + typeof v + "):" + String(v);
    const tid = Math.floor(v / 1000000);
    const serial = Math.floor((v % 1000000) / 1000);
    const p = v % 1000;
    return "tid=" + tid + " serial=" + serial + " p=" + p;
}

function freshRead(o, key) {
    // megamorphic-ish / uncached read forms
    return [o[key], Reflect.get(o, key), Object.getOwnPropertyDescriptor(o, key)?.value];
}
noInline(freshRead);

function dumpMismatch(kind, o, p, got, want, readerSlot, round) {
    let lines = [];
    lines.push("=== MISMATCH (" + kind + ") readerSlot=" + readerSlot + " round=" + round + " ===");
    lines.push("prop=p" + p + " got=" + got + " [" + decode(got) + "] want=" + want + " [" + decode(want) + "]");
    lines.push("owner: o.tid=" + o.tid + " o.serial=" + o.serial);
    // immediate re-reads
    const rr1 = o["p" + p];
    const fr = freshRead(o, "p" + p);
    lines.push("reread-same-form=" + rr1 + " reflect=" + fr[1] + " gopd=" + fr[2]);
    // all siblings, fresh
    let sib = [];
    for (let q = 0; q < PROPS; ++q)
        sib.push("p" + q + "=" + o["p" + q]);
    lines.push("siblings: " + sib.join(" "));
    let idx = [];
    for (let q = 0; q < PROPS; ++q)
        idx.push(o.indexed[q]);
    lines.push("indexed: " + idx.join(","));
    lines.push("ownKeys: " + Object.keys(o).join(","));
    lines.push("indexingMode: " + $vm.indexingMode(o));
    print(lines.join("\n"));
    try { $vm.dumpCell(o); } catch (e) { print("dumpCell failed: " + e); }
    // does it persist after a sleep?
    sleepMs(5);
    const later = o["p" + p];
    print("after-5ms-reread: p" + p + "=" + later + (later === want ? " (HEALED)" : " (STILL WRONG)"));
    throw new Error(kind + " property corrupt: got " + got + " want " + want);
}
noInline(dumpMismatch);

let g_readerSlot = -1;
let g_round = -1;

function checkOne(o) {
    const base = o.tid * 1000000 + o.serial * 1000;
    let sum = 0;
    for (let p = 0; p < PROPS; ++p) {
        const named = o["p" + p];
        const idx = o.indexed[p];
        if (named !== base + p)
            dumpMismatch("named", o, p, named, base + p, g_readerSlot, g_round);
        if (idx !== base + p)
            dumpMismatch("indexed", o, p, idx, base + p, g_readerSlot, g_round);
        sum += named + idx;
    }
    return sum;
}
noInline(checkOne);

function threadBody(slot) {
    for (let serial = 0; serial < OBJECTS_PER_THREAD; ++serial)
        registry[slot].push(buildOne(slot, serial));
    Atomics.add(ready, "count", 1);
    waitUntil(() => Atomics.load(ready, "count") > THREADS, 30000);

    let checksum = 0;
    for (let round = 0; round < ROUNDS; ++round) {
        g_readerSlot = slot; g_round = round;
        const own = registry[slot];
        for (const o of own)
            checksum += checkOne(o);
        own[round % own.length]["late" + round] = slot * 7 + round;
        for (let t = 0; t <= THREADS; ++t) {
            if (t === slot)
                continue;
            const theirs = registry[t];
            for (let i = 0; i < theirs.length; ++i)
                checksum += checkOne(theirs[i]);
        }
        Atomics.add(shared, "hits", 1);
        shared["fromSlot" + slot] = round;
        if (!(round % 16))
            sleepMs(1);
    }
    for (let round = 0; round < ROUNDS; ++round) {
        const o = registry[slot][round % registry[slot].length];
        if (o["late" + round] !== slot * 7 + round)
            throw new Error("lost late property late" + round + " on slot " + slot);
    }
    return checksum;
}

const threads = spawnN(THREADS, threadBody);
const mainChecksum = threadBody(THREADS);
const results = joinAll(threads);

shouldBeTrue(mainChecksum > 0);
for (const r of results)
    shouldBeTrue(r > 0);
shouldBe(shared.hits, (THREADS + 1) * ROUNDS, "no lost Atomics.add on the shared object");
for (let t = 0; t <= THREADS; ++t)
    shouldBe(shared["fromSlot" + t], ROUNDS - 1, "last foreign write visible for slot " + t);

print("spawned-thread-butterfly-stress: PASS");
