//@ requireOptions("--useJSThreads=1")
// mc-jit-delete-reuse-stale-offset.js — MC-JIT surface S2(c) (docs/threads/
// cve/map-MC-JIT.md): foreign delete -> quarantine-epoch reuse vs a compiled
// loop holding a hoisted CheckStructure proof + butterfly base across the
// epoch-bump stop.
//
// EXECUTE POST-UNGIL (amplifier; deterministic oracle, no ASAN needed —
// the violation is OM I21's "read of f returning g's value").
//
// Mechanism: with the victim structure's TTL sets dead, a foreign delete is
// cell-locked but NOT a stop; the deleted out-of-line slot is quarantined
// (OM I18/D1) and promoted to Reusable only after the owning heap's
// quarantine-epoch bump — which happens at a collection stop the victim
// loop can park through while its hoisted {CheckStructure(S_old), masked
// base} live in registers (CheckTraps clobberize preserves them). Post-
// resume the loop keeps writing property f's old offset; if that slot has
// been promoted and reused for a brand-new property g, the write lands in
// g — cross-property aliasing.
//
// Oracle: writer thread hammers o.f with values from DOMAIN_F; main deletes
// f, forces GC (epoch bumps), re-adds fresh properties g_k expecting
// DOMAIN_G values. Any g_k ever observed holding a DOMAIN_F value is a hit.
load("../harness.js", "caller relative");

const ROUNDS = 150;
const WRITES_PER_ROUND = 5000;
const gate = { go: 0, started: 0, phase: 0, stop: 0 };

// Build the victim with enough inline-capacity pressure that f lands
// out-of-line (only out-of-line slots are quarantined/reused).
function makeVictim() {
    const o = {};
    for (let i = 0; i < 100; ++i)
        o["pad" + i] = i;                 // spill past inline capacity
    o.f = 0xf000;
    return o;
}

const shared = { o: makeVictim() };

const F_BASE = 0xf000;                    // DOMAIN_F: [0xf000, 0xf000+WRITES)
const G_BASE = 0x6000;                    // DOMAIN_G: [0x6000, 0x6000+ROUNDS)
const isDomainF = v => typeof v === "number" && v >= F_BASE && v < F_BASE + WRITES_PER_ROUND;

// Hot writer loop on the spawned thread: proves the structure once, then
// stores o.f repeatedly with poll sites at the back edge. This thread is
// FOREIGN to the object (created on main) — its first write fires F1 and
// kills writeThreadLocal, ensuring later compiles are unregistered.
const writer = spawnN(1, () => {
    Atomics.add(gate, "started", 1);
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0, 100);

    function fStorm(o) {
        for (let i = 0; i < WRITES_PER_ROUND; ++i) {
            // PutByOffset under a (potentially hoisted) structure proof.
            // If main deletes f mid-loop, these writes must die in the
            // QUARANTINED slot (or miss/slow-path) — never land in a
            // reused slot.
            o.f = F_BASE + i;
        }
    }
    noInline(fStorm);

    let spins = 0;
    while (!Atomics.load(gate, "stop")) {
        const o = shared.o;
        if (typeof o.f === "number" || o.f === undefined)
            fStorm(o);
        spins++;
    }
    return spins > 0 ? "stormed" : "idle";
})[0];

waitUntil(() => Atomics.load(gate, "started") === 1);
Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go", Infinity);

const haveGC = typeof gc === "function";
for (let r = 0; r < ROUNDS; ++r) {
    const o = shared.o;

    // 1. Delete f: slot goes Quarantined (release-stored jsUndefined first,
    //    OM D1/I30 — the writer may still be storing into it).
    delete o.f;

    // 2. Force collection stops so the owning heap's quarantine epoch bumps
    //    past the deletion and the slot is promoted to Reusable. The writer
    //    loop parks through these stops holding its compiled state.
    if (haveGC) { gc(); gc(); }
    else { for (let i = 0; i < 1e4; ++i) ({ waste: i }); }

    // 3. Re-add fresh properties: the first out-of-line add after promotion
    //    draws from Reusable — i.e. may land in f's old offset.
    const gName = "g" + r;
    o[gName] = G_BASE + r;

    // 4. The probe: g must NEVER read back a DOMAIN_F value. The writer is
    //    still (or was, mid-park) storing DOMAIN_F numbers at f's old
    //    offset under its stale structure proof.
    for (let probe = 0; probe < 50; ++probe) {
        const v = o[gName];
        if (v !== G_BASE + r) {
            if (isDomainF(v))
                throw new Error("I21 violation round " + r + ": " + gName
                    + " aliased a stale o.f write: 0x" + v.toString(16));
            throw new Error("round " + r + ": " + gName + " corrupted: " + String(v));
        }
    }

    // 5. Restore f for the next round (new offset or reused — both fine;
    //    the writer re-proves the new structure on its next compile/exit).
    o.f = F_BASE;

    // Periodically replace the victim so the writer also exercises the
    // re-prove path against a fresh structure chain.
    if ((r & 31) === 31)
        shared.o = makeVictim();
}

Atomics.store(gate, "stop", 1);
shouldBe(writer.join(), "stormed");
print("mc-jit-delete-reuse-stale-offset: PASS");
