//@ defaultRun
//@ runDefault("--useCopyOnWriteArraysForImmutableProperties=0")

// A JSArray whose elements are Int32, Double or Contiguous without holes, and which has no named properties, keeps that shape when
// its properties are made immutable: it becomes a copy-on-write array, whose storage every tier reads in place and no tier stores
// into in place. Other arrays, and other objects with elements, get dictionary indexing mode. Either way nothing changes
// afterwards. With --useCopyOnWriteArraysForImmutableProperties=0 every array takes the second path, and everything here but
// the storage kind still holds.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function snapshot(object) {
    return JSON.stringify(Reflect.ownKeys(object).map(key => {
        let d = Object.getOwnPropertyDescriptor(object, key);
        return [String(key), "value" in d ? (typeof d.value === "object" || typeof d.value === "function" ? typeof d.value : String(d.value)) : "accessor", d.writable, d.enumerable, d.configurable];
    })) + " length:" + String(object.length) + " extensible:" + Object.isExtensible(object);
}

function isCopyOnWrite(object) { return /CopyOnWrite/.test($vm.indexingMode(object)); }

// An allocation site learns from what becomes of its arrays: an array that is written to, frozen or given dictionary indexing
// teaches its site to hand out slower storage. So the arrays that must start without holes and in Int32, Double or Contiguous shape come from sites of their own, whose arrays
// are never written after they are built, and are all built before anything else here runs.
function int32Subject() { let a = []; for (let i = 0; i < 8; ++i) a.push(i); return a; }
function doubleSubject() { let a = []; for (let i = 0; i < 8; ++i) a.push(i + 0.5); return a; }
function contiguousSubject() { let a = []; for (let i = 0; i < 8; ++i) a.push(i % 2 ? "s" + i : { i }); return a; }
function largeSubject() { let a = []; for (let i = 0; i < 20000; ++i) a.push(i); return a; }
function literalSubject() { return [1, 2, 3, 4]; }
class Derived extends Array { }
function derivedSubject() { let a = new Derived; for (let i = 0; i < 8; ++i) a.push(i); return a; }
function usedAsPrototypeSubject() { let a = int32Subject(); Object.create(a); return a; }
let copyOnWriteKinds = { int32: int32Subject, double: doubleSubject, contiguous: contiguousSubject, literal: literalSubject, derived: derivedSubject, usedAsPrototype: usedAsPrototypeSubject };

let mutators = [
    a => { a[0] = 9; }, a => { "use strict"; a[1] = 9; }, a => { a[a.length] = 9; }, a => { a[a.length + 5] = 9; }, a => { a.length = 0; }, a => { a.length = 99; },
    a => a.push(9), a => a.push(9, 9, 9), a => a.pop(), a => a.shift(), a => a.unshift(9), a => a.splice(0, 1), a => a.splice(1, 0, 9), a => a.splice(1, 2, 7, 7, 7),
    a => a.reverse(), a => a.sort(), a => a.sort((x, y) => (x < y) - (x > y)), a => a.fill(9), a => a.fill(9, 1, 2), a => a.copyWithin(0, 1), a => { delete a[0]; },
    a => Object.defineProperty(a, 0, { value: 9 }), a => Object.defineProperty(a, 0, { get() { return 9; } }), a => Object.defineProperty(a, "length", { value: 0 }), a => Object.defineProperty(a, "length", { writable: false }),
    a => { a.named = 9; }, a => Reflect.set(a, 1, 9), a => Reflect.deleteProperty(a, 1), a => Object.assign(a, [9, 9]), a => Array.prototype.push.apply(a, [7, 8]), a => Array.prototype.fill.call(a, 9, 0, 1), a => Array.prototype.copyWithin.call(a, 1, 0),
    a => { a[1]++; }, a => { a[1] += 1; }, a => { [a[0], a[1]] = [a[1], a[0]]; }, a => { for (let k in a) { a[k] = 0; break; } }, a => Object.setPrototypeOf(a, null), a => { a.__proto__ = {}; },
    a => { class Base { constructor() { return a; } } class Field extends Base { field = 1; } new Field; },
    // Stores of a value the storage cannot hold as it is, which convert it first.
    a => { a[0] = "X"; }, a => Array.prototype.push.call(a, "X"), a => Array.prototype.unshift.call(a, "X", "X"), a => Array.prototype.splice.call(a, 0, 1, "X", "X"),
    a => Array.prototype.sort.call(a, () => -1), a => { a[40000] = "X"; }, a => Array.prototype.fill.call(a, "X"),
];
// These make an ordinary array non-extensible, which is one of the things that teach an allocation site: they are not warmed up.
let mutatorsNotWarmedUp = [a => Object.freeze(a), a => Object.seal(a)];

let targets = {};
for (let name in copyOnWriteKinds)
    targets[name] = $vm.makePropertiesImmutable(copyOnWriteKinds[name]());
let largeTarget = $vm.makePropertiesImmutable(largeSubject());
let usesCopyOnWriteStorage = isCopyOnWrite(targets.int32);
// (Every array has slow-put array storage in a realm that has a bad time from the start, --alwaysHaveABadTime=1: not even an array literal.)
let literalsStartCopyOnWrite = isCopyOnWrite(literalSubject());

// 1. Which arrays become copy-on-write arrays, with their shape kept.
for (let name in copyOnWriteKinds) {
    let array = copyOnWriteKinds[name]();
    let before = $vm.indexingMode(array);
    shouldBe(isCopyOnWrite(array), name === "literal" && literalsStartCopyOnWrite, name + " before: " + before);
    shouldBe($vm.makePropertiesImmutable(array), array);
    shouldBe($vm.hasImmutableProperties(array), true);
    shouldBe(isCopyOnWrite(array), usesCopyOnWriteStorage, name + ": " + before + " became " + $vm.indexingMode(array));
    if (usesCopyOnWriteStorage && name !== "literal")
        shouldBe($vm.indexingMode(array), before.replace("ArrayWith", "CopyOnWriteArrayWith"), name + " keeps its shape");
}

// 2. Every mutator, hot on ordinary arrays of the same kinds first, against every one of those kinds: nothing changes, the storage included.
let ordinarySources = [int32Subject, doubleSubject, contiguousSubject, literalSubject, derivedSubject].map(make => make());
for (let mutator of mutators) {
    noInline(mutator);
    for (let i = 0; i < Math.ceil(testLoopCount / 4); ++i) {
        try { mutator(ordinarySources[i % ordinarySources.length].slice()); } catch { }
    }
}
for (let name in copyOnWriteKinds) {
    let array = targets[name];
    if (usesCopyOnWriteStorage)
        shouldBe(isCopyOnWrite(array), true, "the " + name + " target is copy-on-write");
    let before = snapshot(array), prototype = Object.getPrototypeOf(array), mode = $vm.indexingMode(array);
    for (let mutator of mutators.concat(mutatorsNotWarmedUp)) {
        for (let i = 0; i < 4; ++i) {
            try { mutator(array); } catch (e) { if (!(e instanceof TypeError)) throw e; }
        }
        shouldBe(snapshot(array), before, name + " after " + String(mutator));
        shouldBe(Object.getPrototypeOf(array), prototype, name + " prototype after " + String(mutator));
        shouldBe($vm.indexingMode(array), mode, name + " storage after " + String(mutator));
    }
}

// 3. An array whose elements take a large allocation: a few mutators of each sort, compared by its contents as a string.
{
    shouldBe(isCopyOnWrite(largeTarget), usesCopyOnWriteStorage);
    let before = String(largeTarget), mode = $vm.indexingMode(largeTarget);
    for (let mutator of [a => a.push(1), a => a.pop(), a => a.shift(), a => a.unshift(1), a => a.splice(10, 5), a => a.reverse(), a => a.fill(0), a => a.copyWithin(0, 5), a => { "use strict"; a[19999] = 0; }, a => { "use strict"; a.length = 5; }, a => { "use strict"; a[20000] = 0; }]) {
        let error;
        try { mutator(largeTarget); } catch (e) { error = e; }
        shouldBe(error instanceof TypeError, true, String(mutator));
    }
    shouldBe(String(largeTarget) === before, true);
    shouldBe($vm.indexingMode(largeTarget), mode);
    shouldBe(largeTarget.length, 20000);
}

// 4. Array.prototype.concat() appends in place to what the species constructor returns.
{
    for (let make of [int32Subject, doubleSubject, contiguousSubject, literalSubject]) {
        let result = $vm.makePropertiesImmutable(make()), before = snapshot(result), mode = $vm.indexingMode(result);
        class Species extends Array { static get [Symbol.species]() { return function () { return result; }; } }
        for (let other of [[5], [5.5], ["s"], make()]) {
            let error;
            try { Species.from([1, 2]).concat(other); } catch (e) { error = e; }
            shouldBe(error instanceof TypeError, true, String(make.name));
        }
        shouldBe(snapshot(result), before);
        shouldBe($vm.indexingMode(result), mode);
    }
}
