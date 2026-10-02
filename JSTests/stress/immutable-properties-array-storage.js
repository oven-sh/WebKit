// A JSArray whose elements are packed (Int32, Double or Contiguous, no holes) and which has no named properties keeps packed
// elements when its properties are made immutable: they move to copy-on-write storage, which every tier reads in place and no tier
// stores into in place. Other arrays, and other objects with elements, get dictionary indexing mode. Either way nothing changes
// afterwards. With --useCopyOnWriteStorageForImmutableArrayElements=0 every array takes the second path, and everything here but
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
// teaches its site to hand out slower storage. So the arrays that must start packed come from sites of their own, whose arrays
// are never written after they are built, and are all built before anything else here runs.
function int32Subject() { let a = []; for (let i = 0; i < 8; ++i) a.push(i); return a; }
function doubleSubject() { let a = []; for (let i = 0; i < 8; ++i) a.push(i + 0.5); return a; }
function contiguousSubject() { let a = []; for (let i = 0; i < 8; ++i) a.push(i % 2 ? "s" + i : { i }); return a; }
function largeSubject() { let a = []; for (let i = 0; i < 5000; ++i) a.push(i); return a; }
function literalSubject() { return [1, 2, 3, 4]; }
class Derived extends Array { }
function derivedSubject() { let a = new Derived; for (let i = 0; i < 8; ++i) a.push(i); return a; }
let packedKinds = { int32: int32Subject, double: doubleSubject, contiguous: contiguousSubject, large: largeSubject, literal: literalSubject, derived: derivedSubject };

let mutators = [
    a => { a[0] = 9; }, a => { "use strict"; a[1] = 9; }, a => { a[a.length] = 9; }, a => { a[a.length + 5] = 9; }, a => { a.length = 0; }, a => { a.length = 99; },
    a => a.push(9), a => a.push(9, 9, 9), a => a.pop(), a => a.shift(), a => a.unshift(9), a => a.splice(0, 1), a => a.splice(1, 0, 9), a => a.splice(1, 2, 7, 7, 7),
    a => a.reverse(), a => a.sort(), a => a.sort((x, y) => (x < y) - (x > y)), a => a.fill(9), a => a.fill(9, 1, 2), a => a.copyWithin(0, 1), a => { delete a[0]; },
    a => Object.defineProperty(a, 0, { value: 9 }), a => Object.defineProperty(a, 0, { get() { return 9; } }), a => Object.defineProperty(a, "length", { value: 0 }), a => Object.defineProperty(a, "length", { writable: false }),
    a => { a.named = 9; }, a => Reflect.set(a, 1, 9), a => Reflect.deleteProperty(a, 1), a => Object.assign(a, [9, 9]), a => Array.prototype.push.apply(a, [7, 8]), a => Array.prototype.fill.call(a, 9, 0, 1), a => Array.prototype.copyWithin.call(a, 1, 0),
    a => { a[1]++; }, a => { a[1] += 1; }, a => { [a[0], a[1]] = [a[1], a[0]]; }, a => { for (let k in a) { a[k] = 0; break; } }, a => Object.setPrototypeOf(a, null), a => { a.__proto__ = {}; },
    a => { class Base { constructor() { return a; } } class Field extends Base { field = 1; } new Field; },
];
// These make an ordinary array non-extensible, which is one of the things that teach an allocation site: they are not warmed up.
let mutatorsNotWarmedUp = [a => Object.freeze(a), a => Object.seal(a)];

let targets = {};
for (let name in packedKinds)
    targets[name] = mutators.concat(mutatorsNotWarmedUp).map(() => $vm.makePropertiesImmutable(packedKinds[name]()));
let keptForTheEnd = [int32Subject(), doubleSubject(), contiguousSubject(), literalSubject()].map(a => $vm.makePropertiesImmutable(a));
let usesCopyOnWriteStorage = isCopyOnWrite(targets.int32[0]);

// 1. Which arrays keep packed elements.
for (let name in packedKinds) {
    let array = packedKinds[name]();
    let before = $vm.indexingMode(array);
    shouldBe(isCopyOnWrite(array), name === "literal", name + " before: " + before);
    shouldBe($vm.makePropertiesImmutable(array), array);
    shouldBe($vm.hasImmutableProperties(array), true);
    shouldBe(isCopyOnWrite(array), usesCopyOnWriteStorage, name + ": " + before + " became " + $vm.indexingMode(array));
    if (usesCopyOnWriteStorage && name !== "literal")
        shouldBe($vm.indexingMode(array), before.replace("ArrayWith", "CopyOnWriteArrayWith"), name + " keeps its shape");
}

// 2. Every mutator, hot on ordinary arrays of the same kinds first, against every packed kind: nothing changes, the storage included.
for (let mutator of mutators) {
    noInline(mutator);
    for (let i = 0; i < 2000; ++i) {
        for (let name in packedKinds) {
            if (name === "large" && i % 100)
                continue;
            try { mutator(packedKinds[name]().slice()); } catch { }
        }
    }
}
for (let name in packedKinds) {
    mutators.concat(mutatorsNotWarmedUp).forEach((mutator, index) => {
        let array = targets[name][index];
        if (usesCopyOnWriteStorage)
            shouldBe(isCopyOnWrite(array), true, "the " + name + " target is packed");
        let before = snapshot(array), prototype = Object.getPrototypeOf(array), mode = $vm.indexingMode(array);
        for (let i = 0; i < 100; ++i) {
            try { mutator(array); } catch (e) { if (!(e instanceof TypeError)) throw e; }
        }
        shouldBe(snapshot(array), before, name + " after " + String(mutator));
        shouldBe(Object.getPrototypeOf(array), prototype, name + " prototype after " + String(mutator));
        shouldBe($vm.indexingMode(array), mode, name + " storage after " + String(mutator));
    });
}

// 3. Reads agree with an ordinary array of the same contents, hot.
{
    let reads = [(a, i) => a[i], (a, i) => a.at(-1 - i), a => a.length, (a, i) => a.indexOf(a[i]), (a, i) => a.includes(a[i]), (a, i) => a.lastIndexOf(a[i]), (a, i) => a.slice(i).length, a => a.map(x => x).length,
        a => a.filter(() => true).length, a => { let n = 0; for (let x of a) n++; return n; }, a => [...a].length, a => Math.max(...a.map(Number).filter(x => x === x), 0), a => a.join(), a => String(a), a => `${a}`, a => a.concat([1]).length,
        a => Object.keys(a).length, a => a.reduce(n => n + 1, 0), a => { let [x, y] = a; return typeof x + typeof y; }, a => Array.from(a).length, a => a.toReversed().length, a => a.toSorted(() => 0).length, a => a.with(0, 1)[0], a => a.flat().length,
        a => a.findLast(() => true) === a[a.length - 1], a => JSON.stringify(a), a => a.entries().next().value[0], a => a.slice().constructor === a.constructor, a => (function () { return arguments.length; })(...a)];
    for (let name in packedKinds) {
        let subject = $vm.makePropertiesImmutable(packedKinds[name]()), twin = packedKinds[name]();
        for (let read of reads) {
            for (let i = 0; i < 300; ++i)
                shouldBe(String(read(subject, i % 4)), String(read(twin, i % 4)), name + " " + String(read));
        }
        let copy = subject.slice(); copy[0] = "changed"; copy.push(1);
        shouldBe(copy[0], "changed"); shouldBe($vm.hasImmutableProperties(copy), false); shouldBe($vm.hasImmutableProperties([...subject]), false);
        shouldBe(Object.isFrozen(subject), false); shouldBe(Object.getOwnPropertyDescriptor(subject, 0).writable, true);
    }
}

// 4. An inheritor: JSArray::put is reached with another receiver. The inheritor gets its own properties; the array is untouched.
for (let name in packedKinds) {
    let array = $vm.makePropertiesImmutable(packedKinds[name]()), before = snapshot(array), mode = $vm.indexingMode(array);
    let child = Object.create(array);
    for (let i = 0; i < 200; ++i) { child.named = i; child[0] = "own"; child[20] = i; child.length = 50; Array.prototype.push.call(child, i); }
    shouldBe(child.named, 199); shouldBe(child[0], "own"); shouldBe(Object.hasOwn(child, "length"), true);
    shouldBe(snapshot(array), before, name + " under an inheritor"); shouldBe($vm.indexingMode(array), mode);
}

// 5. Garbage collection: the elements are reachable only through the array.
{
    let arrays = [];
    for (let i = 0; i < 500; ++i) { let a = []; for (let k = 0; k < 40; ++k) a.push({ value: i * 100 + k }); arrays.push($vm.makePropertiesImmutable(a)); }
    for (let round = 0; round < 3; ++round) { fullGC(); let junk = []; for (let i = 0; i < 20000; ++i) junk.push({ i }); edenGC(); }
    for (let i = 0; i < arrays.length; ++i) { for (let k = 0; k < 40; ++k) shouldBe(arrays[i][k].value, i * 100 + k); }
}

// 6. What does not keep packed elements, and is as unchangeable: holes, a named property, no elements yet, sparse, an array-like.
{
    let others = {
        holey: () => { let a = int32Subject(); delete a[1]; return a; }, holeyDouble: () => { let a = doubleSubject(); delete a[1]; return a; },
        named: () => { let a = int32Subject(); a.tag = 1; return a; }, undecided: () => new Array(4), empty: () => [], sparse: () => { let a = [1, 2]; a[100000] = 3; return a; },
        arrayLike: () => ({ 0: 1, 1: 2, length: 2 }), argumentsObject: () => (function () { "use strict"; return arguments; })(1, 2, 3),
    };
    for (let name in others) {
        let object = $vm.makePropertiesImmutable(others[name]());
        shouldBe(isCopyOnWrite(object), false, name + ": " + $vm.indexingMode(object));
        let before = snapshot(object);
        for (let mutator of mutators.concat(mutatorsNotWarmedUp)) {
            for (let i = 0; i < 20; ++i) { try { mutator(object); } catch (e) { if (!(e instanceof TypeError)) throw e; } }
            shouldBe(snapshot(object), before, name + " after " + String(mutator));
        }
    }
}

// 7. Last, because it changes every array in the realm: the realm starts having a bad time. The arrays made before are left in
// the storage they have, read as before and stay unchangeable, and ordinary arrays work.
{
    let modes = keptForTheEnd.map(a => $vm.indexingMode(a)), snapshots = keptForTheEnd.map(snapshot), ordinary = int32Subject();
    $vm.haveABadTime(globalThis);
    shouldBe(/SlowPut/.test($vm.indexingMode(ordinary)), true, "an ordinary array: " + $vm.indexingMode(ordinary));
    if (usesCopyOnWriteStorage)
        shouldBe(keptForTheEnd.map(a => $vm.indexingMode(a)).join(), modes.join());
    keptForTheEnd.forEach((array, index) => {
        for (let mutator of mutators.concat(mutatorsNotWarmedUp)) { try { mutator(array); } catch (e) { if (!(e instanceof TypeError)) throw e; } }
        shouldBe(snapshot(array), snapshots[index], "after the bad time");
    });
    shouldBe(keptForTheEnd[0][3] + keptForTheEnd[1][1] + keptForTheEnd[3][2], 3 + 1.5 + 3);
    let late = $vm.makePropertiesImmutable(int32Subject()), lateBefore = snapshot(late);
    for (let mutator of mutators) { try { mutator(late); } catch (e) { if (!(e instanceof TypeError)) throw e; } }
    shouldBe(snapshot(late), lateBefore, "made immutable after the bad time");
    let fresh = []; fresh[3] = 1; fresh.push(2); shouldBe(fresh.length, 5);
}
