// A function's name, length and prototype are made when they are first asked for, and an array's length is no entry of its property table.
// Asked of an object that was made immutable before anybody looked, they have the attributes they have on any other object.

function shouldBe(actual, expected, message) { if (actual !== expected) throw new Error(message + ": expected " + String(expected) + " but got " + String(actual)); }
function shouldThrow(func, message) { let error; try { func(); } catch (e) { error = e; } if (!(error instanceof TypeError)) throw new Error(message + ": expected a TypeError but got " + String(error)); }
const flags = (o, keys = Reflect.ownKeys(o)) => keys.map(k => { const d = Object.getOwnPropertyDescriptor(o, k); return String(k) + ":" + (d ? [d.writable, d.enumerable, d.configurable].map(b => b === undefined ? "-" : b ? "T" : "f").join("") : "none"); }).join(" ");
const functions = {
    ordinary: () => function f(x, y) { }, anonymous: () => (() => function () { })(), arrow: () => (x => x), klass: () => class C { constructor(a) { } }, method: () => ({ m(a) { } }).m, getter: () => Object.getOwnPropertyDescriptor({ get g() { return 1; } }, "g").get,
    generator: () => function* g() { }, async: () => async function a() { }, asyncGenerator: () => async function* ag() { }, bound: () => (function b(x, y) { }).bind(null, 1), host: () => createGlobalObject().parseInt, hostConstructor: () => createGlobalObject().Map,
};
const arrays = { int32: () => [1, 2, 3], double: () => [1.5, 2.5], contiguous: () => ["a", {}], holey: () => [1, , 3], empty: () => [], arrayStorage: () => { const a = [1, 2]; a[100000] = 3; a.length = 2; return a; }, lengthReadOnly: () => Object.defineProperty([1, 2], "length", { writable: false }) };
for (const [name, make] of Object.entries(functions)) {
    const expected = flags(make());
    shouldBe(flags($vm.makePropertiesImmutable(make())), expected, name + ": all at once");
    for (const key of ["name", "length", "prototype"]) shouldBe(flags($vm.makePropertiesImmutable(make()), [key]), flags(make(), [key]), name + ": " + key + " alone");
    const f = $vm.makePropertiesImmutable(make()); shouldBe(delete f.name, false, name + ": name is not deleted (this is sloppy code: the refusal has the language's form)"); shouldBe(Object.hasOwn(f, "name"), Object.hasOwn(make(), "name"), name + ": and is there");
    shouldBe(Reflect.defineProperty(f, "length", { value: 99 }), false, name + ": length is not redefined"); shouldBe(f.length, make().length, name + ": and is what it was");
}
for (const [name, make] of Object.entries(arrays)) {
    shouldBe(flags($vm.makePropertiesImmutable(make()), ["length"]), flags(make(), ["length"]), name + ": length");
    const a = $vm.makePropertiesImmutable(make()), n = a.length; a.length = 0; shouldBe(a.length, n, name + ": length stays");
}
