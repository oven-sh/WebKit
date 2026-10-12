// An object that has lost a property, so that its property table has a free offset, can be made immutable like any other, with its
// attributes left as they are ($vm.makePropertiesImmutable(object)) or fixed ($vm.makePropertiesImmutable(object, true)).

function shouldBe(actual, expected, message) { if (actual !== expected) throw new Error(message + ": expected " + String(expected) + " but got " + String(actual)); }
function shouldThrow(func, message) { let error; try { func(); } catch (e) { error = e; } if (!(error instanceof TypeError)) throw new Error(message + ": expected a TypeError but got " + String(error)); }
const flags = o => Reflect.ownKeys(o).map(k => { const d = Object.getOwnPropertyDescriptor(o, k); return String(k) + ":" + [d.writable, d.enumerable, d.configurable].map(b => b === undefined ? "-" : b ? "T" : "f").join(""); }).join(" ");
const makers = {
    first: () => { const o = { a: 1, b: 2 }; delete o.a; return o; }, last: () => { const o = { a: 1, b: 2 }; delete o.b; return o; }, middle: () => { const o = { a: 1, b: 2, c: 3 }; delete o.b; return o; },
    all: () => { const o = { a: 1, b: 2 }; delete o.a; delete o.b; return o; }, two: () => { const o = { a: 1, b: 2, c: 3, d: 4 }; delete o.a; delete o.c; return o; },
    outOfLine: () => { const o = {}; for (let i = 0; i < 20; ++i) o["p" + i] = i; delete o.p3; delete o.p17; return o; },
    dictionary: () => { const o = { x: 0 }; for (let i = 0; i < 300; ++i) o["k" + i] = i; for (let i = 0; i < 150; ++i) delete o["k" + i]; return o; },
    accessor: () => { const o = { a: 1, get g() { return 1; }, b: 2 }; delete o.a; return o; }, func: () => { const f = Object.assign(function () { }, { a: 1, b: 2 }); delete f.a; return f; },
    functionName: () => { const f = function named() { }; delete f.name; return f; }, array: () => { const a = Object.assign([1, 2, 3], { a: 1, b: 2 }); delete a.a; return a; },
    prototypeInUse: () => { const p = { a: 1, m() { } }; Object.create(p).w = 1; delete p.a; return p; }, symbol: () => { const s = Symbol(), o = { [s]: 1, b: 2 }; delete o[s]; return o; },
};
for (const fix of [false, true]) {
    for (const [name, make] of Object.entries(makers)) {
        const what = name + (fix ? ", fixed" : ""), o = make(), before = flags(o), keys = Reflect.ownKeys(o), values = keys.map(k => o[k]);
        shouldBe($vm.makePropertiesImmutable(o, fix), o, what);
        shouldBe($vm.hasImmutableProperties(o), true, what + ": immutable"); shouldBe(Object.isExtensible(o), false, what + ": not extensible");
        shouldBe(flags(o), fix ? before.split(" ").map(entry => /^\d+:/.test(entry) ? entry : entry.replace(/:T(.).$/, ":f$1f").replace(/:(.)(.)T$/, ":$1$2f")).join(" ") : before, what + ": the attributes (what is fixed is the table: an element says what it said)");
        shouldBe(Reflect.ownKeys(o).length, keys.length, what + ": as many keys");
        for (const lost of ["a", "b", "c", "p3", "k7", "name", "fresh"]) {
            if (Object.hasOwn(o, lost)) continue;
            o[lost] = 1; shouldBe(Object.hasOwn(o, lost), false, what + ": " + lost + " does not come back"); shouldThrow(() => { "use strict"; o[lost] = 1; }, what + ": " + lost + ", strict");
            shouldBe(Reflect.defineProperty(o, lost, { value: 1 }), false, what + ": " + lost + " is not defined");
        }
        keys.forEach((k, i) => {
            const d = Object.getOwnPropertyDescriptor(o, k);
            if ("value" in d) { o[k] = "other"; shouldBe(o[k], values[i], what + ": " + String(k) + " keeps its value"); shouldThrow(() => { "use strict"; o[k] = "other"; }, what + ": " + String(k) + ", strict"); }
            shouldBe(delete o[k], false, what + ": " + String(k) + " is not deleted"); shouldBe(Object.hasOwn(o, k), true, what + ": " + String(k) + " is there");
        });
        const heir = Object.create(o); for (const k of keys) { const d = Object.getOwnPropertyDescriptor(o, k); if (typeof k !== "string" || !("value" in d)) continue; heir[k] = "own"; shouldBe(Object.hasOwn(heir, k), fix || d.writable, what + ": an heir has its own " + k + ", unless it was read-only all along"); }
        heir.a = "own"; shouldBe(heir.a, "own", what + ": and what was lost");
        shouldBe($vm.hasImmutableProperties({ ...o }), false, what + ": a copy is not immutable"); shouldBe(Object.keys({ ...o }).join(), Object.keys(o).join(), what + ": a copy has its keys");
    }
}
// Hot: the sites have deleted from and stored into objects of these shapes.
function del(o) { delete o.a; } function set(o, v) { o.b = v; } function add(o) { o.a = 1; } for (const f of [del, set, add]) noInline(f);
for (const fix of [false, true]) {
    for (let i = 0; i < testLoopCount; ++i) { const o = { a: 1, b: 2 }; del(o); set(o, i); if (i % 2) add(o); }
    const o = { a: 1, b: 2 }; del(o); set(o, "kept"); $vm.makePropertiesImmutable(o, fix);
    for (let i = 0; i < testLoopCount; ++i) { set(o, i); add(o); del(o); }
    shouldBe(o.b, "kept", "hot: b"); shouldBe("a" in o, false, "hot: a"); shouldBe(Reflect.ownKeys(o).length, 1, "hot: keys");
}
