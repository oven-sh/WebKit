// yarr-noPoll-probe.js — EXPERIMENTER round 3, NPR-2 confirmIf #2.
//
// Claim under test: YarrInterpreter.cpp has zero poll/traps/safepoint sites,
// so a catastrophic-backtracking match holds heap access indefinitely with
// no §A.3 poll. A Class-A fire from a sibling thread should then produce the
// 30s watchdog abort with the NON-QUIESCENT lite EXECUTING inside
// Yarr::Interpreter::matchDisjunction (hasHeapAccess=true, NO WTF::Lock
// frames) — a signature distinct from the (now-fixed) H1 lock cycle.
//
// Thread A: exponential backtracking regexp (multi-minute, effectively
// unbounded: /(a+)+b/ against 'a'*40 + 'c').
// Main thread: waits ~1.5s for A to be inside Yarr, then runs the repro.js
// existing-property replace-put warmup loop, whose first cached replace per
// (structure, offset) fires a Class-A WatchpointSet
// (Structure::didCachePropertyReplacement) — post-H1-fix this fire happens
// BEFORE the codeBlock->m_lock locker, so the only thing that can starve the
// conductor is thread A itself.
//
// Expected: SIGABRT at ~30s ("failed to reach a stopped world"), core shows
// the regexp thread executing in Yarr interpreter frames.
// If it exits 0 (regexp finishing is impossible in <2^40 steps) or the stop
// completes without abort, the Yarr no-poll sub-claim is REFUTED and the
// quiescence path that saved it must be identified.

if (typeof Thread !== "function")
    throw new Error("requires --useJSThreads=1");

function regexpWork() {
    const re = /(a+)+b/;
    const subject = "a".repeat(40) + "c";
    // Single exec; exponential backtracking keeps it inside the Yarr
    // interpreter far longer than the 30s watchdog budget.
    const m = re.exec(subject);
    return m === null ? "no-match" : "match";
}

function putWork() {
    function Obj() {
        this.link = null;
        this.id = 0;
        this.kind = 0;
        this.a1 = 0;
        this.hops = 0;
        this.scratch = 0;
    }
    const objs = [];
    for (let i = 0; i < 8; ++i) {
        const o = new Obj();
        o.id = i;
        objs.push(o);
    }
    let acc = 0;
    for (let s = 0; s < 2000000; ++s) {
        const o = objs[s & 7];
        // Existing-property replace puts: first cached replace per
        // (structure, offset) fires Class-A.
        o.scratch = (o.scratch + s) & 0xffffff;
        o.a1 = (o.a1 + 1) | 0;
        o.hops = (o.hops + o.id) & 0xffff;
        o.kind = s & 3;
        acc = (acc + o.a1 + o.hops + o.scratch + o.kind) | 0;
    }
    return "" + acc;
}

const a = new Thread(regexpWork, 0);

// Busy-wait ~1.5s so thread A is deep inside the Yarr interpreter before the
// Class-A fire. Plain JS loop: this thread stays poll-capable throughout.
const SPIN_MS = (typeof globalThis.PROBE_SPIN_MS === "number") ? globalThis.PROBE_SPIN_MS : 1500;
const t0 = Date.now();
let spin = 0;
while (Date.now() - t0 < SPIN_MS)
    spin = (spin + 1) | 0;

// Trigger the Class-A fire(s).
const tPut = Date.now();
const acc = putWork();
print("PUTWORK-DONE acc=" + acc + " spin=" + spin + " putMs=" + (Date.now() - tPut) + " sinceStartMs=" + (Date.now() - t0));

// If we get here, every Class-A stop converged despite thread A being inside
// Yarr — the no-poll claim is refuted. (We never expect the join to finish.)
const r = a.join();
print("UNEXPECTED-JOIN result=" + r);
