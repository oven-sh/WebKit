//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// SPEC-objectmodel E4-G (history §33): GIL on, the inline caches' and the
// megamorphic probe's owner transitions do not claim the StructureID lane; the
// GIL is the exclusion, because it is handed over only inside blocking
// primitives and no transition window contains one. This test hands the GIL
// back and forth as often as it can - timed Atomics.wait, notify, joins of
// short-lived threads - while the owner adds properties through the inline
// caches (inline slots, out-of-line within capacity, out-of-line growth, a
// megamorphic put site) and a second thread adds other properties to the same
// objects (cell-locked foreign adds), on shapes whose thread-local sets have
// already fired. Every property either thread added must be present with its
// own value. GIL off the claimed forms run and the same checks apply.
load("../harness.js", "caller relative");

function mk() { return {}; }
noInline(mk);

// Fire the thread-local sets of every shape on the {a..h} chain: a foreign
// thread extends one object wearing each intermediate shape.
const names = ["a", "b", "c", "d", "e", "f", "g", "h"];
{
    const victims = [];
    for (let k = 0; k <= names.length; ++k) {
        const o = mk();
        for (let j = 0; j < k; ++j)
            o[names[j]] = j;
        victims.push(o);
    }
    new Thread(() => { for (const o of victims) o.zz = 1; }).join();
    for (const o of victims) {
        if (o.zz !== 1)
            throw new Error("foreign add lost while firing the sets");
    }
}

function ownerAdd(o, round, i) {
    switch (round) { // literal names: the puts go through inline caches
    case 0: o.a = i; break;
    case 1: o.b = i + 1; break;
    case 2: o.c = i + 2; break;
    case 3: o.d = i + 3; break;
    case 4: o.e = i + 4; break;
    case 5: o.f = i + 5; break;
    case 6: o.g = i + 6; break;
    case 7: o.h = i + 7; break;
    }
}
noInline(ownerAdd);
function putX(o, v) { o.x = v; } // sees every shape the pool ends up with: megamorphic
noInline(putX);

const N = 1500;
const ROUNDS = 8;
const pool = [];
for (let i = 0; i < N; ++i)
    pool.push(mk());
const box = { go: 0, beat: 0 };

const foreign = new Thread(() => {
    while (!Atomics.load(box, "go"))
        Atomics.wait(box, "go", 0, 1);
    let added = 0;
    for (let round = 0; round < ROUNDS; ++round) {
        for (let i = 0; i < N; ++i) {
            pool[i]["q" + round] = i * 100 + round;
            ++added;
            if (!(i & 63)) {
                Atomics.wait(box, "beat", -1, 0.05); // times out: two GIL hand-offs
                if (!(i & 255))
                    new Thread(() => 1).join();
            }
        }
    }
    return added;
});
Atomics.store(box, "go", 1);
Atomics.notify(box, "go");

for (let round = 0; round < ROUNDS; ++round) {
    for (let i = 0; i < N; ++i) {
        ownerAdd(pool[i], round, i);
        if (!(i & 127)) {
            Atomics.notify(box, "beat");
            Atomics.wait(box, "beat", -1, 0.02);
        }
    }
}
for (let i = 0; i < N; ++i) {
    putX(pool[i], -i);
    if (!(i & 127))
        Atomics.wait(box, "beat", -1, 0.02);
}

const added = foreign.join();
if (added !== ROUNDS * N)
    throw new Error("foreign add count " + added);
for (let i = 0; i < N; ++i) {
    const o = pool[i];
    for (let round = 0; round < ROUNDS; ++round) {
        if (o[names[round]] !== i + round)
            throw new Error("object " + i + ": owner-added ." + names[round] + " is " + o[names[round]] + ", expected " + (i + round));
        if (o["q" + round] !== i * 100 + round)
            throw new Error("object " + i + ": foreign-added .q" + round + " is " + o["q" + round] + ", expected " + (i * 100 + round));
    }
    if (o.x !== -i)
        throw new Error("object " + i + ": megamorphic .x is " + o.x);
}
print("PASS");
