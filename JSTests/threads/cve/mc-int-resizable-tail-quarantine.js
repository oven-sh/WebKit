//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0")
// MC-INT susceptibility test (docs/threads/cve/map-MC-INT.md S4, plus S6).
//
// DO NOT RUN during bring-up: written for post-ungil execution (the targeted
// code, resizeGILOff / deferShrinkTailGILOff / consumeQuarantinedTailOnRegrow
// in runtime/ArrayBuffer.cpp, is only reachable when useJSThreads is on AND
// useThreadGIL is off — annex N6 arms 3/4).
//
// Target: the tail-quarantine size arithmetic for GIL-off resizable
// ArrayBuffer shrink/grow. The extension subtraction
//     newlyQuarantined = entry.tailOffset - desiredSize     (ArrayBuffer.cpp:516)
//     entry.tailSize   = handle.size()    - desiredSize     (ArrayBuffer.cpp:518)
// is guarded only by debug ASSERTs; soundness rests on the inductive
// invariant "published logical length <= pending tailOffset, tailOffset
// page-aligned, every grow consumes the pending tail FIRST" spanning three
// functions, the handle lock, and the heap stop hook. A release-mode breach
// underflows size_t and feeds a ~2^64 tailSize to OSAllocator::protect at
// the NEXT collection — so every phase below forces full GCs to drain the
// quarantine, where a corrupted entry crashes deterministically.
//
// Phase 1 is deterministic (single thread, exact shrink/regrow-over-pending-
// tail/shrink-deeper sequences crossing the 64 KiB page boundary in every
// alignment). Phase 2 is the amplifier-ready cross-thread churn: N Threads
// race resize() on ONE buffer object — the only way to attack the
// handle-lock serialization leg of the invariant. Phase 3 is the S6
// belt-and-braces growable-SAB grow storm (CVE-2024-2887 underflow-leg
// analog; expected immune: grow is monotone + RELEASE_ASSERT-guarded).
load("../harness.js", "caller relative");

const PAGE = 64 * 1024; // PageCount::pageSize
const MAX_PAGES = 16;
const MAX = MAX_PAGES * PAGE;

function forceStop() {
    // The quarantine retires (protect + decommit + updateSize) at a heap §10
    // stop; fullGC conducts one. Fall back to allocation pressure if the
    // shell function is absent.
    if (typeof fullGC === "function")
        fullGC();
    else if (typeof gc === "function")
        gc();
    else {
        let sink = [];
        for (let i = 0; i < 1e4; ++i)
            sink.push({ p: i });
    }
}

function checkZeroRange(u8, begin, end, label) {
    // Sampled: ends, page edges, and a stride within.
    const probes = [begin, end - 1, begin + 1, end - 2];
    for (let p = begin; p < end; p += 4099)
        probes.push(p);
    for (let a = Math.ceil(begin / PAGE) * PAGE; a < end; a += PAGE) {
        probes.push(a);
        if (a - 1 >= begin)
            probes.push(a - 1);
    }
    for (const p of probes) {
        if (p < begin || p >= end)
            continue;
        shouldBe(u8[p], 0, label + " @" + p);
    }
}

// ---------------------------------------------------------------------------
// Phase 1: deterministic shrink / regrow-over-pending-tail / shrink-deeper.
// Each step's expected quarantine action is noted; the assertions are the
// observable contract (byteLength, zero-fill of regrown ranges, round-trip
// writes at both edges), and forceStop() makes any corrupted tail entry
// retire (and crash) HERE rather than later.
// ---------------------------------------------------------------------------
{
    const buf = new ArrayBuffer(MAX, { maxByteLength: MAX });
    const u8 = new Uint8Array(buf); // length-tracking view

    const stamp = (len) => { if (len) { u8[0] = 0x5a; u8[len - 1] = 0xa5; } };
    const checkStamp = (len, label) => {
        if (len) {
            shouldBe(u8[0], 0x5a, label + " lo");
            shouldBe(u8[len - 1], 0xa5, label + " hi");
        }
    };

    u8.fill(0x77);

    // Shrink to a NON-page-aligned length: desired = 6P, pending tail [6P,16P).
    let len = 5 * PAGE + 1;
    buf.resize(len);
    shouldBe(buf.byteLength, len, "p1 shrink1");
    stamp(len); checkStamp(len, "p1 shrink1");

    // Regrow ACROSS the pending tail start (partial consume: trims the entry
    // to [10P,16P); pages still committed, range must read back zero-filled).
    let prev = len;
    len = 10 * PAGE - 1;
    buf.resize(len);
    shouldBe(buf.byteLength, len, "p1 regrow1");
    checkZeroRange(u8, prev, len, "p1 regrow1 zero-fill");
    checkStamp(prev, "p1 regrow1 preserved"); // prefix untouched

    // Shrink DEEPER than the trimmed tail start (the :516 extension
    // subtraction: tailOffset 10P -> 7P; underflow here would need
    // desiredSize > tailOffset, which the invariant forbids).
    prev = len;
    len = 6 * PAGE + 1; // desired = 7P <= tailOffset 10P
    buf.resize(len);
    shouldBe(buf.byteLength, len, "p1 shrink2");
    stamp(len); checkStamp(len, "p1 shrink2");

    // Retire the pending [7P,16P) tail under a stop NOW.
    forceStop();
    shouldBe(buf.byteLength, len, "p1 post-retire length");
    checkStamp(len, "p1 post-retire contents");

    // Regrow after retirement: desiredSize > handle.size() => the commit
    // (bytesToAdd = desiredSize - handle.size()) leg, then zeroFill.
    prev = len;
    len = MAX;
    buf.resize(len);
    shouldBe(buf.byteLength, MAX, "p1 regrow-after-retire");
    checkZeroRange(u8, prev, len, "p1 regrow-after-retire zero-fill");
    stamp(len); checkStamp(len, "p1 full");

    // Edge alignments around one page boundary, with a retire between each:
    // shrink targets PAGE-1 / PAGE / PAGE+1 / 1 / 0 all keep desired <=
    // tailOffset; each pass re-grows over the fresh tail before stopping.
    for (const target of [PAGE - 1, PAGE, PAGE + 1, 1, 0]) {
        buf.resize(MAX);
        u8.fill(0x33);
        buf.resize(target);
        shouldBe(buf.byteLength, target, "p1 edge shrink " + target);
        buf.resize(target + PAGE <= MAX ? target + PAGE : MAX); // regrow over pending tail
        checkZeroRange(u8, target, buf.byteLength, "p1 edge regrow zero " + target);
        buf.resize(target); // shrink again: extension subtraction once more
        forceStop(); // retire with tailOffset = roundUp(target)
        shouldBe(buf.byteLength, target, "p1 edge post-retire " + target);
        if (target)
            shouldBe(u8[target - 1], 0x33, "p1 edge prefix preserved " + target);
    }
}

// ---------------------------------------------------------------------------
// Phase 2: cross-thread resize churn on ONE buffer (amplifier target).
// Each thread runs a seeded LCG so the schedule, not the data, is the only
// nondeterminism. Threads tolerate exactly the spec-legal races: a typed-
// array store/load can be silently OOB-dropped, resize() itself must never
// throw (targets are always within [0, MAX] and the buffer is never
// detached). Main conducts stops throughout, so quarantine entries built by
// every interleaving are continuously retired under load.
// ---------------------------------------------------------------------------
{
    const buf = new ArrayBuffer(MAX, { maxByteLength: MAX });
    const N = 4;
    const ITERS = 300;
    const gate = { done: 0 };

    const threads = spawnN(N, (index) => {
        let s = (index + 1) * 0x9e3779b9 >>> 0;
        const rnd = () => (s = (Math.imul(s, 1103515245) + 12345) >>> 0);
        const view = new Uint8Array(buf);
        for (let i = 0; i < ITERS; ++i) {
            // Bias toward page-edge targets: the arithmetic under test only
            // does interesting work at round-up boundaries.
            const page = rnd() % (MAX_PAGES + 1);
            const jitter = [0, 1, PAGE - 1][rnd() % 3];
            const target = Math.min(page * PAGE + (page === MAX_PAGES ? 0 : jitter), MAX);
            buf.resize(target); // must NEVER throw: always <= maxByteLength
            const len = buf.byteLength; // racy sample; only used to pick probes
            if (len) {
                const p = rnd() % len;
                view[p] = index + 1; // may be dropped if a racing shrink won
                const v = view[p]; // may be undefined (OOB) — both legal
                if (v !== undefined && v !== 0 && !(v >= 1 && v <= N))
                    throw new Error("p2 read tore a non-written value: " + v + " @" + p);
            }
        }
        Atomics.add(gate, "done", 1);
        return index;
    });

    // Conduct stops while the churn runs: every stop retires whatever tail
    // entries the current interleaving produced. A :516/:518 underflow
    // surfaces here as a crash in the retire hook (OSAllocator::protect with
    // a wrapped size), not as a JS-visible value.
    while (Atomics.load(gate, "done") < N) {
        forceStop();
        sleepMs(5);
    }
    joinAll(threads);
    forceStop(); // drain the final pending entry, if any

    // Post-churn sanity: the buffer is still a functioning resizable buffer
    // over its whole range.
    buf.resize(0);
    forceStop();
    buf.resize(MAX);
    const u8 = new Uint8Array(buf);
    checkZeroRange(u8, 0, MAX, "p2 final regrow zero-fill");
    for (let page = 0; page < MAX_PAGES; ++page)
        u8[page * PAGE] = 0xee;
    for (let page = 0; page < MAX_PAGES; ++page)
        shouldBe(u8[page * PAGE], 0xee, "p2 final page " + page + " writable");
}

// ---------------------------------------------------------------------------
// Phase 3 (S6 belt-and-braces): growable SharedArrayBuffer grow storm.
// Expected immune: SharedArrayBufferContents::grow rejects non-growth before
// any arithmetic (ArrayBuffer.cpp:1448) and RELEASE_ASSERTs the commit
// subtraction. The storm checks the observable contract: byteLength is
// monotone per observer, a racing grow loses no commit (the final length is
// the max requested), and grown space reads zero where never written.
// ---------------------------------------------------------------------------
if (typeof SharedArrayBuffer === "function") {
    const gsab = new SharedArrayBuffer(PAGE, { maxByteLength: MAX });
    const N = 4;
    const STEPS = MAX_PAGES * 2;

    const threads = spawnN(N, (index) => {
        const view = new Uint8Array(gsab);
        let last = gsab.byteLength;
        for (let i = 0; i < STEPS; ++i) {
            const target = Math.min((i + 1) * PAGE + index, MAX); // staggered, some non-aligned
            try {
                gsab.grow(target);
            } catch (e) {
                // A racing larger grow makes this a shrink request. Per
                // ECMA-262 SharedArrayBuffer.prototype.grow ("If
                // newByteLength < currentByteLength or newByteLength >
                // O.[[ArrayBufferMaxByteLength]], throw a RangeError
                // exception") the legal failure is a RangeError — and it is
                // legal ONLY when the length is already at/above the target
                // (byteLength is monotone, so that condition still holds at
                // observation time). A RangeError with byteLength < target
                // would be the skewed-arithmetic failure this phase hunts.
                if (!(e instanceof RangeError))
                    throw e;
                if (gsab.byteLength < target)
                    throw new Error("p3 GSAB in-range grow(" + target + ") threw RangeError with byteLength " + gsab.byteLength);
            }
            const len = gsab.byteLength;
            if (len < last)
                throw new Error("p3 GSAB length regressed: " + last + " -> " + len);
            last = len;
            if (len > 0 && view[len - 1] !== 0 && view[len - 1] !== 0xee)
                throw new Error("p3 GSAB grown tail not zero: " + view[len - 1]);
        }
        return last;
    });
    joinAll(threads);

    gsab.grow(MAX);
    shouldBe(gsab.byteLength, MAX, "p3 final GSAB length");
    const u8 = new Uint8Array(gsab);
    shouldBe(u8[MAX - 1], 0, "p3 GSAB last byte zero");
    shouldThrow(RangeError, () => gsab.grow(MAX - PAGE)); // shrink request: rejected before arithmetic (ECMA-262 grow step: RangeError)
    shouldBe(gsab.byteLength, MAX, "p3 GSAB length unchanged after rejected shrink");
}

print("mc-int-resizable-tail-quarantine: PASS");
