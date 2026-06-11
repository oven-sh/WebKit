//@ requireOptions("--useJSThreads=1")
// MC-VAL susceptibility test (docs/threads/cve/map-MC-VAL.md, surface V4):
// compile-time validation vs. link-time consumption — a TTL/structure
// watchpoint set fires BETWEEN the compiler's validity check and the
// watchpoint registration/installation of optimized code.
//
// Validator: the DFG/FTL plan proves "transitionThreadLocal /
// writeThreadLocal / structure sets valid" on the compiler thread and elides
// checks (SPEC-jit §5.5 E1-E3). Consumer: the installed code runs on N
// mutators under those elisions. The published defense chain is:
//   - Class-A fires run world-stopped + jettison in the same stop
//     (SPEC-jit §5.6, I10), and
//   - link-time revalidation: Plan::reallyAdd re-checks
//     areStillValidOnMainThread and the per-set hasBeenInvalidated arm
//     (DFGPlan.cpp:595-614, DFGDesiredWatchpoints.cpp:166,201-206), with no
//     park point between revalidation and registration (heap deferred,
//     cooperative stops only — jit R1.f).
// The window this storm targets: a foreign transition fires the sets while
// a sibling lite is inside finalize()/reallyAdd, and the in-tree KNOWN
// RESIDUAL of unsynchronized profile reads during compileInThread
// (DFGPlan.cpp:640-646) — profiles must stay advisory (jit I12: profiles
// select, guards validate).
//
// Detection: hot() computes o.x + o.y where each object's slots are bound
// by construction (y === 2x); any code running with an elision justified by
// a fired set can pair a stale offset/shape and break the relation (read of
// f returning g's value, OM I21). Amplifier-ready; green under phase-1 GIL.
load("../harness.js", "caller relative");

const ROUNDS = 40;          // recompile generations
const HOT_ITERS = 20000;    // per-generation warmup => DFG (and FTL later)
const POOL = 16;

const shared = {};
for (let s = 0; s < POOL; ++s)
    shared["p" + s] = null;
const gate = { started: 0, stop: 0, transitions: 0 };

// Foreign-transition storm: every property add on a main-created object is
// a foreign transition (F2) => fires BOTH TTL sets on source+target under a
// per-event stop, racing whatever compile/link is in flight on the carrier.
const firer = new Thread((shared, gate, POOL) => {
    Atomics.add(gate, "started", 1);
    let n = 0;
    while (Atomics.load(gate, "stop") === 0) {
        for (let s = 0; s < POOL; ++s) {
            const o = Atomics.load(shared, "p" + s);
            if (o === null)
                continue;
            // Foreign write (SW flip + writeThreadLocal fire) then foreign
            // transition (transitionThreadLocal fire).
            o.x = o.x | 0;
            o["foreign" + (n & 7)] = n;
            ++n;
        }
        Atomics.add(gate, "transitions", 1);
    }
    return n;
}, shared, gate, POOL);

waitUntil(() => Atomics.load(gate, "started") === 1);

function freshHot() {
    // A fresh function identity per generation => fresh CodeBlock => a new
    // compile + link racing the firer.
    return Function("o", "return o.x + o.y;");
}

let bad = 0;
for (let r = 0; r < ROUNDS; ++r) {
    const hot = freshHot();
    // Generation-private leading shape so speculation re-proves validity
    // against structures whose sets the firer keeps killing.
    const mk = (i) => {
        const o = { x: 0, y: 0 };
        o["gen" + r] = r;
        o.x = i;
        o.y = 2 * i;
        return o;
    };
    const locals = [];
    for (let s = 0; s < POOL; ++s) {
        const o = mk(s + 1);
        locals.push(o);
        Atomics.store(shared, "p" + s, o); // expose to the firer
    }
    for (let i = 0; i < HOT_ITERS; ++i) {
        const o = locals[i & (POOL - 1)];
        const got = hot(o);
        // x/y are never rewritten by the firer (o.x |= 0 is value-neutral),
        // so any violation of got === 3x is a stale-elision consumption.
        if (got !== 3 * o.x)
            ++bad;
    }
}

Atomics.store(gate, "stop", 1);
shouldBeTrue(firer.join() > 0);
shouldBeTrue(Atomics.load(gate, "transitions") > 0);
shouldBe(bad, 0);
