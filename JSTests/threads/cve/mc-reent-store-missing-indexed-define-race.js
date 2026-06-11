//@ requireOptions("--useJSThreads=1")
// MC-REENT S3c susceptibility test (docs/threads/cve/map-MC-REENT.md):
// GIL-off, Atomics.store's Missing-arm INDEXED add is a validate-then-act
// window — probe {Missing, extensible}, then generic putDirectIndex
// (ThreadAtomics.cpp atomicsStoreOnPropertyGilOff, KNOWN RESIDUAL recorded
// in docs/threads/INTEGRATE-ungil.md U-T10 item 3: the named-add fix via
// putDirectForAtomicsMissingAdd does NOT cover this leg). A racing indexed
// defineProperty (accessor / non-writable) forces a sparse-map/SlowPutAS
// conversion the put is not conditional on; the put can clobber the freshly
// defined element.
//
// Oracle is linearization-exact, so the test is deterministic-on-outcome
// even though the trigger is a race:
//   store-then-define  => define wins; final descriptor = the defined one;
//   define-then-store  => store throws TypeError (accessor / not writable);
//                         final descriptor = the defined one.
// EVERY legal interleaving therefore ends with the defineProperty result in
// place once both sides returned. A surviving plain data value (accessor
// leg) or a wrong value / writable:true (non-writable leg) is an
// indistinguishable-heap violation (THREAD.md); memory-unsafe outcomes of
// the racing AS conversion surface as crashes under ASAN/TSAN.
//
// Deterministically green under the phase-1 GIL (the GIL serializes the
// whole step); the residual window only exists GIL-off — run post-ungil and
// under Tools/threads/amplify.sh. Annex T2: bounded blocking (waits use
// bounded quanta), every thread joined.
load("../harness.js", "caller relative");

const ROUNDS = 200;
const IDX = 5;

function runRound(defineUnderRace, checkFinal) {
    const o = {};
    const gate = { go: 0, done: 0 };
    const t = new Thread(function () {
        while (Atomics.load(gate, "go") === 0)
            Atomics.wait(gate, "go", 0, 100);
        let threw = false;
        try {
            Atomics.store(o, String(IDX), 123); // Missing INDEXED add: the residual leg.
        } catch (e) {
            if (!(e instanceof TypeError))
                throw e;
            threw = true;
        }
        return threw;
    });
    Atomics.store(gate, "go", 1);
    defineUnderRace(o);
    const storeThrew = t.join();
    checkFinal(o, storeThrew);
}

// Leg A: racing indexed ACCESSOR define. Final descriptor must be the
// accessor under every legal linearization.
for (let r = 0; r < ROUNDS; ++r) {
    runRound(
        o => {
            Object.defineProperty(o, IDX, {
                get() { return "fromGetter"; },
                configurable: true,
            });
        },
        (o, storeThrew) => {
            const d = Object.getOwnPropertyDescriptor(o, String(IDX));
            if (!d || typeof d.get !== "function")
                throw new Error("leg A round " + r + ": indexed Missing-add clobbered a racing accessor define"
                    + " (storeThrew=" + storeThrew + ", descriptor=" + JSON.stringify(d) + ")");
            shouldBe(o[IDX], "fromGetter", "leg A: accessor must answer reads");
        });
}

// Leg B: racing indexed NON-WRITABLE data define. Final must be
// {value: 7, writable: false} under every legal linearization
// (store-then-define: define overwrites the fresh element, configurable
// elements permit it; define-then-store: store throws the writability
// TypeError).
for (let r = 0; r < ROUNDS; ++r) {
    runRound(
        o => {
            Object.defineProperty(o, IDX, {
                value: 7,
                writable: false,
                enumerable: true,
                configurable: true,
            });
        },
        (o, storeThrew) => {
            const d = Object.getOwnPropertyDescriptor(o, String(IDX));
            if (!d || d.value !== 7 || d.writable !== false)
                throw new Error("leg B round " + r + ": indexed Missing-add overwrote / out-ordered a racing"
                    + " non-writable define (storeThrew=" + storeThrew + ", descriptor=" + JSON.stringify(d) + ")");
        });
}
