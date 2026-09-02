//@ requireOptions("--useJSThreads=1")
// A property add that grows the butterfly nukes the object's structure ID (sets
// its low bit) until the new butterfly is published. With the GIL off, another
// thread can read the ID in that window. The JIT decoded the ID without
// clearing the bit, so it read the structure one byte off. An instanceof then
// walked to a bad prototype: it returned false, or it crashed. A nuked ID names
// the old structure.
load("../harness.js", "caller relative");

class Base {}
class Derived extends Base {}

function isBase(o) { return o instanceof Base; }
noInline(isBase);

const OBJS = 64;
const targets = [];
for (let i = 0; i < OBJS; ++i)
    targets.push(new Derived());

for (let i = 0; i < 20000; ++i)
    isBase(targets[i % OBJS]);

const state = { stop: 0 };
const grower = new Thread((objs, s) => {
    let round = 0;
    while (Atomics.load(s, "stop") === 0 && round < 400) {
        for (let i = 0; i < objs.length; ++i)
            objs[i]["p" + round] = round;
        ++round;
    }
    return round;
}, targets, state);

let checks = 0;
for (let n = 0; n < 200000; ++n) {
    if (!isBase(targets[n % OBJS]))
        throw new Error("instanceof gave false at " + n);
    ++checks;
}
Atomics.store(state, "stop", 1);
grower.join();
shouldBe(checks, 200000, "checks");
