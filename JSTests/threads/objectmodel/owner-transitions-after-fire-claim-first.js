//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// SPEC-objectmodel §5 E4-C / history §26 (r17): once a structure's thread-local
// sets have fired (the first cross-thread transition of ANY object of that
// shape), the OWNER of every other object of that shape keeps transitioning it
// without the cell lock - claim-first (CAS on the StructureID lane) in C++ and
// in the inline caches - instead of taking the locked C++ path forever; foreign
// cell-locked writers claim the same lane first and restart when they lose.
// Checks: (1) an object-building loop on the main thread slows down by a small
// factor, not 20x, after one object of its shapes was extended by another
// thread; (2) owner adds (JIT, claim-first) racing foreign adds (locked,
// claim-first) on the SAME objects lose nothing: every property every thread
// added is present with its own value.
load("../harness.js", "caller relative");

function mk() { return {}; }
noInline(mk);
function build(n) {
    let s = 0;
    for (let i = 0; i < n; ++i) { const o = mk(); o.a = i; o.b = 1; o.c = 2; o.d = 3; o.e = 4; o.f = 5; o.g = 6; o.h = 7; s += o.h; } // inline, then out-of-line adds
    return s;
}
noInline(build);
function timeBuild() { build(20000); let best = 1e9; for (let r = 0; r < 5; ++r) { const t0 = preciseTime(); build(300000); best = Math.min(best, preciseTime() - t0); } return best; }

const before = timeBuild();
// Fire the sets of every shape on the {a..h} chain: a foreign thread extends one
// object wearing each intermediate shape.
{
    const victims = [];
    const names = ["a", "b", "c", "d", "e", "f", "g", "h"];
    for (let k = 0; k <= names.length; ++k) { const o = mk(); for (let j = 0; j < k; ++j) o[names[j]] = j; victims.push(o); }
    const probe = mk(); probe.a = 0; probe.b = 1; // keeps wearing {a,b}
    new Thread(() => { for (const o of victims) o.zz = 1; }).join();
    for (const o of victims) if (o.zz !== 1) throw new Error("foreign add lost");
    if ($vm.structureThreadLocalSetsValid(probe)[0]) throw new Error("expected the {a,b} shape's transition-thread-local set to have fired");
}
const lockedBefore = $vm.jsThreadsLockedTransitionCount();
const after = timeBuild();
const locked = $vm.jsThreadsLockedTransitionCount() - lockedBefore;
// (No timings in the output: the amplifier compares outputs across runs.)
if (typeof AMPLIFY_VERBOSE !== "undefined") print("build before fire " + (before * 1000).toFixed(1) + " ms, after " + (after * 1000).toFixed(1) + " ms; locked transitions after the fire: " + locked);
// timeBuild() performs about 10M owner transitions; before r17 every one of
// them took the cell lock once the sets had fired.
if (locked > 1000) throw new Error(locked + " owner transitions took the cell-locked path after the fire");
// The locked-transition count above is the exact check; the time ratio is a
// loose backstop (the after-fire adds run through the inline caches' claimed
// form, the before-fire ones are inlined by the DFG; the locked path this test
// guards against measured 40x and more of the inlined loop).
if (after > before * 20) throw new Error("owner transitions after the fire are " + (after / before).toFixed(1) + "x slower");

// (2) Owner vs foreign adds on the same objects, sets already fired.
const N = 2000;
const objs = [];
for (let i = 0; i < N; ++i) objs.push(mk());
const box = { go: 0, done: 0 };
const adder = new Thread(() => {
    while (!Atomics.load(box, "go")) { }
    let added = 0;
    for (let round = 0; round < 6; ++round) {
        for (let i = 0; i < N; ++i) { const o = objs[i]; o["q" + round] = i * 10 + round; ++added; }
    }
    return added;
});
Atomics.store(box, "go", 1);
for (let round = 0; round < 6; ++round) {
    for (let i = 0; i < N; ++i) {
        const o = objs[i];
        switch (round) { // literal names so the puts go through inline caches
        case 0: o.a = i; break;
        case 1: o.b = i + 1; break;
        case 2: o.c = i + 2; break;
        case 3: o.d = i + 3; break;
        case 4: o.e = i + 4; break;
        case 5: o.f = i + 5; break;
        }
    }
}
const added = adder.join();
if (added !== 6 * N) throw new Error("adder count " + added);
for (let i = 0; i < N; ++i) {
    const o = objs[i];
    const want = { a: i, b: i + 1, c: i + 2, d: i + 3, e: i + 4, f: i + 5 };
    for (const k in want) if (o[k] !== want[k]) throw new Error("object " + i + " ." + k + " = " + o[k] + ", want " + want[k]);
    for (let r = 0; r < 6; ++r) if (o["q" + r] !== i * 10 + r) throw new Error("object " + i + " .q" + r + " = " + o["q" + r]);
    if (Object.keys(o).length !== 12) throw new Error("object " + i + " has keys " + Object.keys(o));
}
print("PASS");
