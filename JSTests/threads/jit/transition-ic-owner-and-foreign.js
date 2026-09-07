//@ requireOptions("--useJSThreads=1")
// Cached property-add transitions (SPEC-jit §5.5 Transition, OM E4 / N2-LF).
// Part 1: the thread that created a structure adds properties through the
// inline cache (butterfly-less objects, inline slots; and objects that already
// have out-of-line storage). Part 2: another thread runs the SAME functions on
// its own objects that wear those structures; it is not the structures' owner,
// so its cached transition must refuse and take the slow path (which fires the
// structures' thread-local sets and retires the stub), and every object on
// both threads must end up with every property, exactly once, with the right
// value. Part 3: after the fire the owner keeps adding (now uncached): still
// exact.
load("../harness.js", "caller relative");

function addInline(o, base) { o.a = base + 1; o.b = base + 2; o.c = base + 3; return o; }
noInline(addInline);
function makeWithOutOfLine(seed) {
    // 8 named properties: the later ones live out of line, so the adds in
    // addMore run on an object that already has a butterfly.
    return { p0: seed, p1: seed, p2: seed, p3: seed, p4: seed, p5: seed, p6: seed, p7: seed };
}
noInline(makeWithOutOfLine);
function addMore(o, base) { o.q = base + 10; o.r = base + 11; return o; }
noInline(addMore);

function checkInline(o, base, who) {
    if (o.a !== base + 1 || o.b !== base + 2 || o.c !== base + 3)
        throw new Error(who + ": bad inline adds " + JSON.stringify(o) + " base " + base);
    if (Object.keys(o).join() !== "a,b,c")
        throw new Error(who + ": bad keys " + Object.keys(o).join());
}
function checkMore(o, base, who) {
    if (o.q !== base + 10 || o.r !== base + 11 || o.p7 !== base)
        throw new Error(who + ": bad out-of-line adds " + JSON.stringify(o));
    if (Object.keys(o).length !== 10)
        throw new Error(who + ": bad key count " + Object.keys(o).length);
}

// Part 1: owner, warm the caches well past the JIT thresholds.
const N = 20000;
const mine = [];
for (let i = 0; i < N; ++i) {
    const o = addInline({}, i);
    checkInline(o, i, "owner");
    const p = addMore(makeWithOutOfLine(i), i);
    checkMore(p, i, "owner");
    if (i % 1000 === 0) { mine.push(o); mine.push(p); }
}

// Part 2: a foreign thread through the same (now hot) functions.
const foreign = new Thread((count) => {
    let made = 0;
    for (let i = 0; i < count; ++i) {
        const o = addInline({}, i * 7);
        checkInline(o, i * 7, "foreign");
        const p = addMore(makeWithOutOfLine(i), i);
        checkMore(p, i, "foreign");
        ++made;
    }
    return made;
}, N);

// Part 3: meanwhile and after, the owner keeps going.
for (let i = 0; i < N; ++i) {
    const o = addInline({}, -i);
    checkInline(o, -i, "owner-after");
}
shouldBe(foreign.join(), N, "objects made by the foreign thread");
for (let i = 0; i < N; ++i) {
    const p = addMore(makeWithOutOfLine(i), i);
    checkMore(p, i, "owner-after");
}
for (const o of mine)
    shouldBeTrue(Object.keys(o).length === 3 || Object.keys(o).length === 10, "kept objects intact");
