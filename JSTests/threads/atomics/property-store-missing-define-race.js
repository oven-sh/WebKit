//@ requireOptions("--useJSThreads=1")
// SPEC-ungil §C.2 / U-T10 amend, Missing-arm conditional add: GIL-off, the
// store body's probe(Missing) -> put used to be three separate steps, so a
// key defined by another thread between the probe and the put (accessor or
// non-writable data) would be silently replaced by putDirect's define-own
// semantics - converting a racing accessor into a plain data property, a
// heap state no sequential interleaving of Atomics.store can produce
// (define-before-store must throw the D3/D7 TypeError; store-before-define
// leaves the definition final). The amended arm adds named keys through a
// conditional PutModePut path that re-derives existence at publication and
// restarts on loss.
//
// Owner delete/defineProperty(accessor) storm vs a foreign Atomics.store
// storm on the same missing-then-defined key. Invariant checked every owner
// iteration: immediately after defineProperty the descriptor MUST still be
// the accessor - a racing store may only land while the key is absent
// (which the subsequent define then replaces) or throw TypeError once it is
// an accessor; it may never clobber the accessor itself. Bounded loops
// (phase-1 GIL is cooperative).
load("../harness.js", "caller relative");

const PER = 800;

const o = {};
const gate = { go: 0 };

const foreign = new Thread(() => {
    while (Atomics.load(gate, "go") === 0)
        sleepMs(1);
    let stored = 0;
    let rejected = 0;
    for (let i = 0; i < PER; ++i) {
        try {
            Atomics.store(o, "m", 5);
            ++stored; // Legal only while the key was absent or a plain data slot.
        } catch (e) {
            if (!(e instanceof TypeError))
                throw e;
            ++rejected; // The accessor (D3) won the race.
        }
    }
    return stored + rejected === PER;
});

Atomics.store(gate, "go", 1);
for (let i = 0; i < PER; ++i) {
    delete o.m; // Opens the Missing window for the racing store.
    Object.defineProperty(o, "m", { get() { return 42; }, configurable: true });
    const d = Object.getOwnPropertyDescriptor(o, "m");
    if (!d || typeof d.get !== "function")
        throw new Error("racing Atomics.store clobbered a defined accessor (Missing-arm TOCTOU): " + JSON.stringify(d));
    if (o.m !== 42)
        throw new Error("accessor result corrupted: " + String(o.m));
}
shouldBeTrue(foreign.join());

// The owner's last action was a define: the accessor must be final.
const final = Object.getOwnPropertyDescriptor(o, "m");
shouldBeTrue(typeof final.get === "function", "final descriptor is the accessor");
