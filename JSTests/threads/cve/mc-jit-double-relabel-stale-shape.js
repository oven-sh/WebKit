//@ requireOptions("--useJSThreads=1")
// mc-jit-double-relabel-stale-shape.js — MC-JIT surface S2(b) (docs/threads/
// cve/map-MC-JIT.md): per-event-STW Double relabel (OM section 4.7 / I28) vs
// a compiled loop holding a hoisted shape proof across the stop.
//
// EXECUTE POST-UNGIL (amplifier + ASAN). Green-by-construction under the
// phase-1 GIL.
//
// Mechanism: shared ContiguousDouble slots are RAW doubles (OM GT#15). A
// shape change touching Double on an SW=1 object relabels slots IN PLACE
// under a per-event STW; the invariant "no reader holds the old shape across
// a stop" (I28/I34) must extend to generated code. A DFG/FTL loop whose
// CheckArray(Double)+GetButterfly were hoisted above its safepoint poll
// (CheckTraps clobbers only InternalState) parks during the relabel STW and
// resumes still storing RAW UNBOXED doubles into slots every other thread
// now reads as JSValues — an attacker-chosen 64-bit pattern interpreted as a
// cell pointer (fakeobj). Nothing jettisons the loop: the structure's TTL
// sets are already dead (that is why the object is shared), so the relabel
// fires no watchpoint the loop registered.
//
// Oracle: after each relabel round, every slot of the victim must be a
// number, undefined, or the object the relabeler stored — anything else
// (or a crash while the runtime/GC visits the slot as a JSValue) is a hit.
// The double bit patterns written are chosen to look like plausible heap
// pointers if ever misinterpreted.
load("../harness.js", "caller relative");

const LEN = 128;
const ROUNDS = 200;
const gate = { go: 0, started: 0, round: 0, stop: 0 };

// Victim: starts as a Double array, gets shared (foreign write => SW=1).
const victim = new Array(LEN);
for (let i = 0; i < LEN; ++i)
    victim[i] = i + 0.5;                  // ArrayWithDouble
const box = { victim };

// Doubles whose bit patterns resemble tagged heap pointers / small cells if
// reinterpreted as JSValues. (0x0000_7ff8... style payloads via subnormals
// and crafted exponents — close enough for the oracle; ASAN/validateHeap do
// the real judging.)
const SPRAY = [
    2.121995791e-314,                     // 0x0000_0000_4141_4141-ish subnormal
    6.36598737437e-314,
    1.2882297539194267e-231,
    5.4861240687936887e-303,
];

// Hot writer: proves Double shape once per compile, stores raw doubles in a
// loop with poll sites at the back edge. This is the stale-shape holder.
function doubleStorm(a, seed) {
    for (let i = 0; i < LEN; ++i)
        a[i] = SPRAY[(i + seed) & 3];     // raw 8B stores under Double proof
}
noInline(doubleStorm);

const relabeler = spawnN(1, () => {
    Atomics.add(gate, "started", 1);
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0, 100);
    const a = box.victim;
    a[0] = 0.5;                           // foreign write: F1 fire + SW=1
    const marker = { tag: "relabel-marker" };
    let flips = 0;
    while (!Atomics.load(gate, "stop")) {
        // Double -> Contiguous: per-event STW relabel in place (OM 4.7).
        a[1] = marker;                    // non-double store forces relabel
        // back toward Double territory (new transition; may segment — both
        // directions exercise the stale-shape window):
        a[1] = 1.5;
        for (let i = 2; i < 6; ++i)
            a[i] = i + 0.25;
        flips++;
        Atomics.add(gate, "round", 1);
    }
    return flips > 0 ? "relabeled" : "idle";
})[0];

waitUntil(() => Atomics.load(gate, "started") === 1);

// Warm to FTL under the Double proof before opening the race.
for (let w = 0; w < 2e3; ++w)
    doubleStorm(victim, w);

Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go", Infinity);

for (let r = 0; r < ROUNDS; ++r) {
    doubleStorm(victim, r);               // races the in-place relabel STWs
    // Integrity sweep: every slot must be a number or the relabeler's
    // marker object with its exact payload. A raw SPRAY bit-pattern
    // surfacing as an OBJECT here is a fakeobj — crash or fail loudly.
    for (let i = 0; i < LEN; ++i) {
        const v = victim[i];
        const t = typeof v;
        if (t === "number" || v === undefined)
            continue;
        if (t === "object" && v !== null && v.tag === "relabel-marker")
            continue;
        throw new Error("slot " + i + " holds non-domain value (fakeobj?): " + t);
    }
}

Atomics.store(gate, "stop", 1);
shouldBe(relabeler.join(), "relabeled");

// Final full-heap touch of the victim so a lingering raw-double-as-JSValue
// is visited/dereferenced by GC and string conversion.
shouldBeTrue(JSON.stringify(victim.map(v => typeof v)).length > 0,
    "victim must remain walkable");
print("mc-jit-double-relabel-stale-shape: PASS");
