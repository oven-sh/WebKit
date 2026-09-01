//@ requireOptions("--useJSThreads=1")
// MC-PRIM susceptibility test (docs/threads/cve/map-MC-PRIM.md, surface P4).
//
// Trusted-primitive invariant bypass, CVE-2012-0507 shape: the privileged
// primitive's least-checked store lands against an invariant another piece
// of machinery just established. Here the primitive is Atomics.store's
// Missing arm for INDEXED keys (ThreadAtomics.cpp, atomicsStoreOnProperty /
// atomicsStoreOnPropertyGilOff Missing case): a fresh indexed element is
// added via putDirectIndex with define-own semantics. The U-T10 amend fixed
// the NAMED-key TOCTOU with a conditional add (putDirectForAtomicsMissingAdd,
// re-derives existence at publication), but the indexed leg is an
// engine-acknowledged KNOWN RESIDUAL (ThreadAtomics.cpp ~:434, recorded in
// INTEGRATE-ungil): a racing indexed defineProperty (accessor or
// non-writable) forces a sparse-map/SlowPutAS conversion that putDirectIndex
// is not conditional on - so post-ungil, Atomics.store(o, "5", v) probing
// Missing can clobber an accessor/non-writable element defined by another
// thread between the probe and the put. No sequential interleaving of
// Atomics.store can produce that heap state (define-before-store must throw
// the D3/D7 TypeError; store-before-define leaves the definition final).
//
// Indexed twin of JSTests/threads/atomics/property-store-missing-define-race.js.
// Deterministic invariant, checked every owner iteration: immediately after
// defineProperty the descriptor MUST still be the accessor. Under the
// phase-1 GIL this passes trivially (one atomic step); post-ungil it is the
// targeted probe for the residual. Bounded loops; amplifier hooks not
// required (the window is the probe->put gap in every store call).
load("../harness.js", "caller relative");

const PER = 800;
const IDX = 5; // parseIndex hit: routes through the Missing indexed leg.

const o = {};
o.pad = 1; // Keep the object alive as a plain receiver with some shape history.
const gate = { go: 0 };

const foreign = new Thread(() => {
    while (Atomics.load(gate, "go") === 0)
        sleepMs(1);
    let stored = 0;
    let rejected = 0;
    for (let i = 0; i < PER; ++i) {
        try {
            Atomics.store(o, String(IDX), 7);
            ++stored; // Legal only while the element was absent or a plain data slot.
        } catch (e) {
            if (!(e instanceof TypeError))
                throw e;
            ++rejected; // The accessor/non-writable definition (D3/D7) won the race.
        }
    }
    return stored + rejected === PER;
});

Atomics.store(gate, "go", 1);
for (let i = 0; i < PER; ++i) {
    delete o[IDX]; // Opens the Missing window for the racing indexed store.
    Object.defineProperty(o, IDX, { get() { return 42; }, configurable: true });
    const d = Object.getOwnPropertyDescriptor(o, IDX);
    if (!d || typeof d.get !== "function")
        throw new Error("racing Atomics.store clobbered a defined indexed accessor (Missing-arm indexed TOCTOU, MC-PRIM): " + JSON.stringify(d));
    if (o[IDX] !== 42)
        throw new Error("indexed accessor result corrupted: " + String(o[IDX]));
}
shouldBeTrue(foreign.join());

// The owner's last action was a define: the accessor must be final.
const final = Object.getOwnPropertyDescriptor(o, IDX);
shouldBeTrue(typeof final.get === "function", "final indexed descriptor is the accessor");

// Second phase: non-writable data element instead of an accessor. A racing
// Missing-arm store may never overwrite the frozen value or flip writability.
const p = {};
const gate2 = { go: 0 };
const foreign2 = new Thread(() => {
    while (Atomics.load(gate2, "go") === 0)
        sleepMs(1);
    let ok = 0;
    for (let i = 0; i < PER; ++i) {
        try {
            Atomics.store(p, String(IDX), 9);
            ++ok;
        } catch (e) {
            if (!(e instanceof TypeError))
                throw e;
            ++ok; // D7: not writable.
        }
    }
    return ok === PER;
});
Atomics.store(gate2, "go", 1);
for (let i = 0; i < PER; ++i) {
    delete p[IDX];
    Object.defineProperty(p, IDX, { value: 1000, writable: false, configurable: true });
    const d = Object.getOwnPropertyDescriptor(p, IDX);
    if (!d || d.value !== 1000 || d.writable !== false)
        throw new Error("racing Atomics.store clobbered a non-writable indexed element (MC-PRIM): " + JSON.stringify(d));
}
shouldBeTrue(foreign2.join());
