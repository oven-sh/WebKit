//@ requireOptions("--useJSThreads=1")
// The owner adds properties through the cached transition while another thread
// performs structure-only transitions (preventExtensions, seal, a prototype
// change) and its own property adds on the same objects. The foreign
// operations must fire the structures' thread-local sets first (retiring the
// cache) and serialize with the owner's adds: an add that lands before a
// preventExtensions must be visible, one that lands after must be refused
// (TypeError in strict code / silently in sloppy), never half-applied; the
// prototype change must be visible; nothing may crash.
load("../harness.js", "caller relative");

const OBJECTS = 3000;

function addNamed(o, v) { o.a = v; o.b = v + 1; return o; }
noInline(addNamed);
for (let i = 0; i < 20000; ++i)
    addNamed({}, i);

const proto = { marker: 42 };
const objects = [];
for (let i = 0; i < OBJECTS; ++i)
    objects.push({});
const gate = { go: 0 };
const foreign = new Thread((objs, g, p) => {
    Atomics.wait(g, "go", 0);
    let done = 0;
    for (let i = 0; i < objs.length; ++i) {
        const o = objs[i];
        switch (i % 3) {
        case 0: Object.preventExtensions(o); break;
        case 1: Object.setPrototypeOf(o, p); break;
        case 2: o.z = -i; break; // a foreign add through the same structures
        }
        ++done;
    }
    return done;
}, objects, gate, proto);
sleepMs(20);
Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go");
let refused = 0;
for (let i = OBJECTS - 1; i >= 0; --i) {
    try {
        "use strict";
        addNamed(objects[i], i);
    } catch (e) {
        if (!(e instanceof TypeError))
            throw e;
        ++refused;
    }
}
shouldBe(foreign.join(), OBJECTS, "foreign operations");
for (let i = 0; i < OBJECTS; ++i) {
    const o = objects[i];
    const hasA = Object.prototype.hasOwnProperty.call(o, "a");
    const hasB = Object.prototype.hasOwnProperty.call(o, "b");
    if (hasA && o.a !== i)
        throw new Error("object " + i + ": a is " + o.a);
    if (hasB && (o.b !== i + 1 || !hasA))
        throw new Error("object " + i + ": b without a, or wrong b: " + JSON.stringify(o));
    switch (i % 3) {
    case 0:
        shouldBe(Object.isExtensible(o), false, "object " + i + " not extensible");
        break;
    case 1:
        shouldBe(Object.getPrototypeOf(o), proto, "object " + i + " prototype");
        shouldBeTrue(hasA && hasB, "object " + i + " got both adds (setPrototypeOf does not block them)");
        break;
    case 2:
        shouldBe(o.z, -i, "object " + i + " foreign add");
        shouldBeTrue(hasA && hasB, "object " + i + " got both owner adds");
        break;
    }
}
// `refused` varies with the interleaving; keep the output deterministic for the amplifier.
shouldBeTrue(refused >= 0 && refused <= OBJECTS, "refused count in range");
