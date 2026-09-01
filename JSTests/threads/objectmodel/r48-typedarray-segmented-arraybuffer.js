//@ requireOptions("--useJSThreads=1")
// r48 (FUZZ.md / SCALEBENCH §48; surfaced by the r47-002 fix): a Wasteful
// typed-array view CAN carry a SEGMENTED butterfly word — a foreign-thread
// named-property add that escapes E4 (TTL fired / foreign TID) and grows
// outOfLineCapacity falls into trySegmentedTransition (the §44 StayFlatShared
// gate requires !hasIndexingHeader, which a Wasteful view HAS). The
// existingBufferInButterfly() runtime path AND the JIT
// emitLoadTypedArrayArrayBuffer helper (AssemblyHelpers.cpp; previously
// "typed-array wasteful-mode butterflies are never segmented" — that claim
// is false) both read butterfly()->indexingHeader()->arrayBuffer() flat-only;
// segmented derefs [spine - 8] = scribble. Under ASAN that's
// 0xbebebebe poison (DeferrableRefCounted::ref SEGV); under Debug scribble
// it's 0xbadbeef0 (SEGV at 0xbadbef10 in JIT iterator-next).
//
// Covers, 50 rounds:
//   (1) growable SharedArrayBuffer view (m_mode = GrowableSharedAutoLength-
//       WastefulTypedArray): worker adds named props (segments), then spreads
//       — the JIT branchIfResizableOrGrowableSharedTypedArrayIsOutOfBounds /
//       loadTypedArrayByteLength path (the fuzzilli r48-001 8CAE8CE0 repro);
//   (2) plain Wasteful (explicit ArrayBuffer): worker adds named props, then
//       Atomics.load → validateTypedArray → existingBufferInButterfly (the
//       Debug butterfly() ASSERT(!isSegmented) site).
load("../harness.js", "caller relative");

const ROUNDS = 50;

// (1) Growable SharedArrayBuffer + foreign named-prop add + spread (JIT path).
for (let i = 0; i < ROUNDS; ++i) {
    const sab = new SharedArrayBuffer(64, { maxByteLength: 1024 });
    const ta = new Int32Array(sab);
    ta[0] = 0x600d + i;
    new Thread(() => {
        // Enough out-of-line adds to force outOfLineCapacity growth on the
        // worker (foreign TID -> trySegmentedTransition; hasIndexingHeader
        // blocks StayFlatShared).
        for (let p = 0; p < 8; ++p)
            ta["np" + p] = p;
        // Spread iterates -> JIT/LLInt typed-array length / OOB check reads
        // butterfly -> arrayBuffer; segmented spine must dispatch to
        // indexedFragment(0).slots[0].
        const arr = [...ta];
        shouldBe(arr.length, 16, "round " + i + ": growable spread length");
        shouldBe(arr[0], 0x600d + i, "round " + i + ": growable spread [0]");
    }).join();
    shouldBe(ta.np0, 0, "round " + i + ": named prop survived segment");
    shouldBe(ta.buffer.byteLength, 64, "round " + i + ": .buffer after segment");
}

// (2) Plain Wasteful (explicit ArrayBuffer) + foreign named-prop add +
// Atomics.load (runtime existingBufferInButterfly path).
for (let i = 0; i < ROUNDS; ++i) {
    const ab = new ArrayBuffer(32);
    const ta = new Int32Array(ab);
    ta[3] = 0x5e6f + i;
    new Thread(() => {
        for (let p = 0; p < 8; ++p)
            ta["q" + p] = p;
        shouldBe(Atomics.load(ta, 3), 0x5e6f + i, "round " + i + ": Atomics.load post-segment");
        shouldBe(ta.length, 8, "round " + i + ": .length post-segment");
    }).join();
    shouldBe(ta.buffer, ab, "round " + i + ": .buffer identity post-segment");
    shouldBe(ta.q0, 0, "round " + i + ": foreign-segmented named prop");
}
