//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// int-gate-direct-call-relink.js — SPEC-jit Task 13 INTEGRATION GATE:
// true-concurrent call link/relink/unlink stress (§5.8 records, F6, I16).
//
// Scaled-down smoke by default while STWR is stubbed; `-- int-gate` for the
// full loops at M4/CS2 (N-separate-VMs config, R1 freeze scope).
//
// Shape: worker threads hammer call sites in every linkage flavor while the
// conductor churns the callees:
//   - monomorphic link + upgrade: fresh closures of the same executable
//     repeatedly replace each other at a hot site (unlinkOrUpgrade publishes
//     a NEW record each time, F6 — a stale read must complete the OLD record,
//     never a torn pair),
//   - polymorphic stub publish: sites fed K rotating callees,
//   - virtual: sites fed fresh executables (sentinel comparand, always-call),
//   - unlink: GC drives visitWeak unlink (single null m_record store) after
//     callees are dropped.
// Acceptance: every call lands in a callee whose return value matches that
// callee's identity (a torn comparand/target pair would mismatch), no crash,
// I16 holds (no safepoint between record load and call — enforced by lint +
// validateButterflyTagDiscipline's poll-placement pass, exercised here).

load("../harness.js", "caller relative");

const FULL = typeof arguments !== "undefined" && Array.prototype.indexOf.call(arguments, "int-gate") >= 0;
const ROUNDS = FULL ? 400 : 12;
const THREADS = FULL ? 4 : 2;

function makeCallee(id) {
    // Same source per flavor => same executable family; id baked into the
    // closure so the return value identifies the callee that actually ran.
    return function callee(x) { return x * 1000 + id; };
}

function makeFreshExecutableCallee(id) {
    return Function("x", "return x * 1000 + " + id + ";");
}

// Published callee slots the workers call through.
const slots = {
    mono: makeCallee(1),
    poly: [makeCallee(10), makeCallee(11), makeCallee(12)],
    virt: makeFreshExecutableCallee(100),
    monoId: 1,
    virtId: 100,
};

function callMono(x) { return slots.mono(x); }
noInline(callMono);
function callPoly(f, x) { return f(x); }
noInline(callPoly);
function callVirt(x) { return slots.virt(x); }
noInline(callVirt);

const stop = { value: false };

const workers = spawnN(THREADS, function (slot) {
    let calls = 0;
    while (!stop.value) {
        // Monomorphic site: result must decode to SOME id the conductor has
        // published (old or new record — both complete; torn = garbage id).
        const m = callMono(7);
        const mid = m - 7000;
        if (!(mid >= 1 && mid <= 1 + ROUNDS))
            throw new Error("mono call decoded to invalid callee id " + mid);
        // Polymorphic site.
        const p = callPoly(slots.poly[calls % 3], 5);
        const pid = p - 5000;
        if (!(pid >= 10 && pid <= 12))
            throw new Error("poly call decoded to invalid callee id " + pid);
        // Virtual site.
        const v = callVirt(3);
        const vid = v - 3000;
        if (!(vid >= 100 && vid <= 100 + ROUNDS))
            throw new Error("virtual call decoded to invalid callee id " + vid);
        ++calls;
        if (!(calls % 64))
            sleepMs(0);
    }
    return calls;
});

// Warm from the conductor too so the sites tier up.
for (let i = 0; i < 20000; ++i) {
    callMono(7);
    callPoly(slots.poly[i % 3], 5);
    callVirt(3);
}

for (let round = 1; round <= ROUNDS; ++round) {
    // Relink monomorphic: replace the callee closure (same executable).
    slots.monoId = 1 + round;
    slots.mono = makeCallee(1 + round);
    // Rotate one polymorphic member (stub republish).
    slots.poly[round % 3] = makeCallee(10 + (round % 3));
    // Fresh executable for the virtual site every few rounds.
    if (!(round % 4)) {
        slots.virtId = 100 + round;
        slots.virt = makeFreshExecutableCallee(100 + round);
    }
    // GC: dead callees' CodeBlocks go away; visitWeak unlinks dead records
    // (single null store) and unlinkOrUpgrade re-publishes at next call.
    if (typeof $vm !== "undefined" && !(round % 25))
        $vm.gc();
    sleepMs(1);
}

stop.value = true;
const calls = joinAll(workers);
for (const c of calls)
    shouldBeTrue(c > 0, "every worker made progress across relinks");
print("int-gate-direct-call-relink: PASS (" + (FULL ? "FULL" : "smoke — rerun with -- int-gate at M4/CS2") + ")");
