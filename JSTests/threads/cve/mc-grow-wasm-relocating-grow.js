//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useWasmFastMemory=0")
// MC-GROW surface S5b (docs/threads/cve/map-MC-GROW.md): relocating
// BoundsChecking wasm memory grow vs spawned typed-array readers.
//
// WRITTEN FOR THE POST-UNGIL EXECUTION PASS — do not run against a
// mid-bring-up tree.
//
// --useWasmFastMemory=0 forces every non-shared WebAssembly.Memory into
// MemoryMode::BoundsChecking with NO VA reservation (the handle is sized
// exactly initialBytes, WasmMemory.cpp:212-217), so EVERY grow relocates:
// fresh Gigacage allocation + memcpy + handle swap (WasmMemory.cpp:337-358).
// SPEC-ungil annex N6 arm 4 requires that relocation to run under a heap §10
// stop ("grow relocate: stop-separated, no concurrent reader"); the stop
// conduction is NOW LANDED in Memory::grow's BoundsChecking arm
// (wasm/WasmMemory.cpp, gilOffProcess-gated stopTheWorldAndRun around the
// handle swap + success() publication; CVE-AUDIT Tier-B B4). The keepalive
// quarantine in runtime/ArrayBuffer.cpp covers captured/hoisted pre-grow
// snapshots to the NEXT stop.
//
// REGRESSION GATE: this test must PASS once the GIL-off wasm refusal lifts
// (it premise-SKIPs until then). A crash / ASAN fault here means a
// {post-grow length, pre-grow base} pairing escaped the stop — the B4
// invariant has regressed.
//
// Wasm EXECUTION on spawned threads is refused (§I); this test never runs
// wasm code off-main — the spawned threads touch the memory purely through
// typed-array views, which annex N6 explicitly admits ("views over a
// main-created WebAssembly.Memory reach spawned threads as plain TA
// accesses").
load("../resources/assert.js", "caller relative");

if (typeof WebAssembly === "undefined" || typeof WebAssembly.Memory !== "function") {
    // No wasm in this build: surface unreachable, trivially pass.
} else {
    const PATTERN = 0x5a;
    const READERS = 3;
    const ROUNDS = 6;
    const PAGES = 64;       // grow 1 -> 64 pages, 63 relocations per round
    const PAGE = 65536;

    const mailbox = { stop: false, view: null };

    const probeMemory = new WebAssembly.Memory({ initial: 1, maximum: PAGES });
    const haveResizable = typeof probeMemory.toResizableBuffer === "function";

    // The dangerous shape needs a length-tracking view whose buffer is
    // REFRESHED in place across the relocation (refreshAfterWasmMemoryGrow +
    // the per-view refreshVector walk, runtime/ArrayBuffer.cpp:882-899,
    // :1536-1573). Without toResizableBuffer the classic semantics detach
    // the old buffer per grow — that route is the S4 arm, and this test
    // then degrades to a detach-storm regression guard rather than the
    // targeted S5b probe.

    const readers = spawnN(READERS, () => {
        let sink = 0;
        while (!mailbox.stop) {
            const view = mailbox.view;
            if (!view)
                continue;
            for (let i = 0; i < 20000; ++i) {
                // Load length FIRST, then access — the exact two-load shape
                // every tier's TA fast path uses. Index length-1 passed the
                // bounds check against the just-loaded length, so a correct
                // engine must make the access safe even if a relocating
                // grow lands between the two loads. While S5b is open, a
                // post-grow length over the pre-grow base sends this past
                // the end of the old mapping (OOB read AND write).
                const len = view.length;
                if (!len)
                    continue;
                const last = view[len - 1];
                if (!(last === undefined || last === 0 || last === PATTERN))
                    throw new Error("S5b: illegal observation " + last);
                view[len - 1] = PATTERN; // in-bounds write per loaded length
                const tail = view[len - (len > PAGE ? PAGE : 1)];
                if (!(tail === undefined || tail === 0 || tail === PATTERN))
                    throw new Error("S5b: illegal tail observation " + tail);
                sink += (last | 0) + (tail | 0);
            }
        }
        return sink;
    });

    for (let round = 0; round < ROUNDS; ++round) {
        const mem = new WebAssembly.Memory({ initial: 1, maximum: PAGES });
        let buffer;
        if (haveResizable) {
            try { buffer = mem.toResizableBuffer(); } catch { buffer = mem.buffer; }
        } else
            buffer = mem.buffer;
        mailbox.view = new Uint8Array(buffer); // length-tracking when resizable
        for (let p = 1; p < PAGES; ++p) {
            mem.grow(1); // relocates under --useWasmFastMemory=0
            if (!haveResizable)
                mailbox.view = new Uint8Array(mem.buffer); // classic: rebind after detach
        }
        shouldBe(mem.buffer.byteLength, PAGES * PAGE);
        // Drop the round's memory while readers may still hold the view:
        // exercises the stale-mapping keepalive list across the next stop.
        mailbox.view = null;
        if (typeof gc === "function")
            gc();
    }

    mailbox.stop = true;
    joinAll(readers);
}
