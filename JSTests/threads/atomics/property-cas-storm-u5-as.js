//@ requireOptions("--useJSThreads=1")
// SPEC-ungil ANNEX C1 / U-T10, U5 amplifier: owner UNLOCKED ArrayStorage
// store storm vs foreign CAS, same index, SW initially 0.
//
// The cell lock suffices only AFTER SW=1 (jit §5.5 owner AS fast paths store
// unlocked while SW=0), so the foreign thread's very first CAS must run the
// AS pre-lock SW protocol (per-event STW, fire-then-publish) BEFORE entering
// the locked third arm. Two arms per round:
//   - storm arm (index 3): owner plain stores race foreign CAS; every value
//     either side ever observes must come from the legal set (owner numbers
//     or the foreign marker string) - a torn/aliased read or a CAS applied
//     against untracked storage surfaces as an impossible value;
//   - counter arm (index 5): BOTH sides increment through the locked
//     CAS/RMW; the final count is exact (lost-update freedom under the AS
//     cell lock).
// GIL-on this is the serialized oracle (U19).
load("../harness.js", "caller relative");

const ROUNDS = 3;
const PER = 800;

function makeArrayStorage() {
    // Same AS-forcing idiom as objectmodel/i03-as-shift-unshift.js.
    const a = [];
    a[100000] = "force-AS";
    delete a[100000];
    a.length = 0;
    return a;
}

for (let round = 0; round < ROUNDS; ++round) {
    const a = makeArrayStorage();
    for (let i = 0; i < 8; ++i)
        a[i] = 0;
    const gate = { go: 0 };

    const foreign = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        for (let k = 0; k < PER; ++k) {
            // Counter arm: locked third-arm CAS loop.
            for (;;) {
                const c = Atomics.load(a, "5");
                if (Atomics.compareExchange(a, "5", c, c + 1) === c)
                    break;
            }
            // Storm arm: CAS against the owner's unlocked plain stores
            // (SW was 0 when the round began - the first foreign access on
            // this object runs the pre-lock SW protocol).
            const seen = Atomics.load(a, "3");
            if (!(typeof seen === "number" && seen >= 0 && seen % 2 === 0) && seen !== "marker")
                throw new Error("round " + round + ": impossible AS value a[3] = " + String(seen) + " (U5)");
            const swapped = Atomics.compareExchange(a, "3", seen, "marker");
            if (!(typeof swapped === "number" && swapped >= 0 && swapped % 2 === 0) && swapped !== "marker")
                throw new Error("round " + round + ": CAS read impossible AS value " + String(swapped) + " (U5)");
        }
        return true;
    });

    Atomics.store(gate, "go", 1);
    for (let k = 0; k < PER; ++k) {
        a[3] = 2 * (k + 1); // Owner plain store: unlocked fast path while SW=0, locked after the foreign flip.
        for (;;) {
            const c = Atomics.load(a, "5");
            if (Atomics.compareExchange(a, "5", c, c + 1) === c)
                break;
        }
    }
    shouldBeTrue(foreign.join());

    shouldBe(a[5], 2 * PER, "round " + round + ": locked third-arm CAS counter is exact");
    const final3 = a[3];
    shouldBeTrue((typeof final3 === "number" && final3 >= 0 && final3 % 2 === 0) || final3 === "marker",
        "round " + round + ": final a[3] comes from the legal value set");
}
