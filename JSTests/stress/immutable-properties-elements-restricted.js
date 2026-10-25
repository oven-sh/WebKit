//@ defaultRun
//@ runDefault("--useCopyOnWriteArraysForImmutableProperties=0")

// An object with immutable properties takes a definition that only restricts a property: configurable or writable goes to false, as
// Object.seal() and Object.freeze() ask. For an element the new attributes lie with the element: the object gets no property named
// "0" beside its element 0, each key is listed once however it is asked for, and the elements are as unchangeable as before.
// An array answers for its length by itself: it gets no property named "length", and its storage stays as it is.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function keysInEveryWay(object) {
    let walked = [];
    for (let key in object)
        walked.push(key);
    return JSON.stringify([Reflect.ownKeys(object).map(String), Object.getOwnPropertyNames(object), Object.keys(object), walked, Object.entries(object), Object.keys({ ...object }), Object.keys(Object.assign({}, object))]);
}

function valuesInEveryWay(object) {
    let values = [];
    for (let key of Object.keys(object))
        values.push(object[key], object[Number(key)], Object.getOwnPropertyDescriptor(object, key).value, { ...object }[key], Reflect.get(object, key));
    return JSON.stringify(values) + Array.prototype.join.call(object) + JSON.stringify(object);
}

let stores = [
    a => { a[0] = 9; }, a => { "use strict"; a[0] = 9; }, a => { a["0"] = 9; }, a => { a[1] += 1; }, a => { delete a[0]; }, a => { "use strict"; delete a[1]; },
    a => Reflect.set(a, 0, 9), a => Reflect.deleteProperty(a, 0), a => Reflect.defineProperty(a, 0, { value: 9 }), a => Object.defineProperty(a, 0, { value: 9 }),
    a => Object.defineProperty(a, 0, { get() { return 9; } }), a => Array.prototype.fill.call(a, 9), a => Array.prototype.reverse.call(a), a => Array.prototype.push.call(a, 9),
    a => Array.prototype.pop.call(a), a => Array.prototype.shift.call(a), a => Object.assign(a, [9, 9]), a => { a[a.length] = 9; }, a => { a.length = 0; },
];
for (let store of stores) {
    noInline(store);
    for (let i = 0; i < testLoopCount / 10; ++i) {
        try { store([1, 2, 3]); store([1.5, 2.5, 3.5]); store(["a", "b", "c"]); } catch { }
    }
}

let subjects = {
    int32: () => [1, 2, 3],
    double: () => [1.5, 2.5, 3.5],
    contiguous: () => ["a", "b", "c"],
    built: () => { let a = []; for (let i = 0; i < 8; ++i) a.push(i); return a; },
    holes: () => { let a = [1, 2, 3]; a[6] = 4; return a; },
    sparse: () => { let a = [1, 2, 3]; a[100000] = 4; return a; },
    named: () => { let a = [1, 2, 3]; a.named = 4; return a; },
    accessor: () => { let a = [1, 2, 3]; Object.defineProperty(a, 1, { get() { return 2; }, enumerable: true, configurable: true }); return a; },
    arrayLike: () => ({ 0: "x", 1: "y", length: 2, named: 1 }),
    function: () => { let f = function () { }; f[0] = 1; f[1] = 2; return f; },
};

let restrictions = [
    [a => { let mode = $vm.indexingMode(a); Object.defineProperty(a, "length", { writable: false }); shouldBe($vm.indexingMode(a), mode, "the storage behind a restricted length"); }, d => true],
    [a => Object.defineProperty(a, 0, { configurable: false }), d => !d.configurable],
    [a => Object.defineProperty(a, "0", { writable: false }), d => !d.configurable && !d.writable],
    [a => Reflect.defineProperty(a, 1, { configurable: false }), d => !d.configurable],
    [a => Object.seal(a), d => !d.configurable],
    [a => Object.freeze(a), d => !d.configurable && !d.writable],
];

for (let name in subjects) {
    let object = $vm.makePropertiesImmutable(subjects[name]());
    shouldBe($vm.hasImmutableProperties(object), true, name);
    let keys = keysInEveryWay(object), values = valuesInEveryWay(object);
    for (let [restrict, isRestricted] of restrictions) {
        let label = name + " after " + String(restrict);
        restrict(object);
        shouldBe(isRestricted(Object.getOwnPropertyDescriptor(object, 0)), true, label + ": the attributes of element 0");
        shouldBe(JSON.stringify(Object.getOwnPropertyDescriptor(object, 0)), JSON.stringify(Object.getOwnPropertyDescriptor(object, "0")), label + ": by number and by text");
        shouldBe($vm.hasImmutableProperties(object), true, label);
        shouldBe(keysInEveryWay(object), keys, label + ": keys");
        shouldBe(valuesInEveryWay(object), values, label + ": values");
        for (let store of stores) {
            for (let i = 0; i < 3; ++i) {
                try { store(object); } catch (e) { if (!(e instanceof TypeError)) throw e; }
            }
            shouldBe(keysInEveryWay(object), keys, label + ", then " + String(store) + ": keys");
            shouldBe(valuesInEveryWay(object), values, label + ", then " + String(store) + ": values");
        }
    }
    shouldBe(Object.isSealed(object), true, name + " is sealed");
    for (let key of Object.keys(object)) {
        let descriptor = Object.getOwnPropertyDescriptor(object, key);
        shouldBe(descriptor.configurable || descriptor.writable === true, false, name + ": " + key + " is frozen");
    }
}
