//@ defaultRun
//@ runDefault("--useCopyOnWriteArraysForImmutableProperties=0")

// The arrays and array-likes that do not become copy-on-write arrays when their properties are made immutable (holes, a named property, no
// elements yet, sparse or array storage, an array-like object, an arguments object) get dictionary indexing mode and are as
// unchangeable. And what the engine does on its own account to every array in a realm, when the realm starts having a bad time,
// leaves all of them as they are. (See immutable-properties-array-storage.js for the arrays that do.)

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
function literalSubject() { return [1, 2, 3, 4]; }
class Derived extends Array { }
function derivedSubject() { let a = new Derived; for (let i = 0; i < 8; ++i) a.push(i); return a; }

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

let keptForTheEnd = [int32Subject(), doubleSubject(), contiguousSubject(), literalSubject()].map(a => $vm.makePropertiesImmutable(a));
// Arrays without element storage, as Array.prototype is: the bad time gives them storage, which must have no room to store into.
let blankForTheEnd = [$vm.createGlobalObject().Array.prototype, $vm.createGlobalObject().Array.prototype].map(a => $vm.makePropertiesImmutable(a));
let usesCopyOnWriteStorage = isCopyOnWrite(keptForTheEnd[0]);

// 1. Every mutator against every kind.
{
    let others = {
        holey: () => { let a = int32Subject(); delete a[1]; return a; }, holeyDouble: () => { let a = doubleSubject(); delete a[1]; return a; },
        named: () => { let a = int32Subject(); a.tag = 1; return a; }, undecided: () => new Array(4), empty: () => [], sparse: () => { let a = [1, 2]; a[100000] = 3; return a; },
        arrayStorage: () => { let a = [1, 2, 3]; a[20000] = 1; delete a[20000]; a.length = 3; return a; },
        arrayLike: () => ({ 0: 1, 1: 2, length: 2 }), argumentsObject: () => (function () { "use strict"; return arguments; })(1, 2, 3),
    };
    for (let name in others) {
        let object = $vm.makePropertiesImmutable(others[name]());
        shouldBe(isCopyOnWrite(object), false, name + ": " + $vm.indexingMode(object));
        let before = snapshot(object);
        for (let mutator of mutators.concat(mutatorsNotWarmedUp)) {
            for (let i = 0; i < 3; ++i) { try { mutator(object); } catch (e) { if (!(e instanceof TypeError)) throw e; } }
            shouldBe(snapshot(object), before, name + " after " + String(mutator));
        }
    }
}

// 2. Assigning length is refused before the value is converted: no valueOf call and no RangeError, for a copy-on-write array and for the others.
{
    let holey = int32Subject(); delete holey[1];
    for (let array of [$vm.makePropertiesImmutable(int32Subject()), $vm.makePropertiesImmutable(holey), $vm.makePropertiesImmutable([])]) {
        let calls = 0, length = array.length;
        let value = { valueOf() { calls++; return 0; } };
        let error;
        try { (function () { "use strict"; array.length = value; })(); } catch (e) { error = e; }
        shouldBe(error instanceof TypeError, true);
        array.length = value;
        array.length = -1;
        array.length = 4294967296;
        shouldBe(Reflect.set(array, "length", value), false);
        shouldBe(calls, 0);
        shouldBe(array.length, length);
        // Defining length converts the value before it validates, as ArraySetLength does: the length it has succeeds.
        shouldBe(Reflect.defineProperty(array, "length", { value: String(length) }), true);
        shouldBe(Reflect.defineProperty(array, "length", { value: { valueOf() { calls++; return length; } } }), true);
        shouldBe(calls > 0, true);
        shouldBe(Reflect.defineProperty(array, "length", { value: length + 1 }), false);
        let rangeError;
        try { Reflect.defineProperty(array, "length", { value: -1 }); } catch (e) { rangeError = e; }
        shouldBe(rangeError instanceof RangeError, true);
        shouldBe(array.length, length);
    }
}

// 3. Last, because it changes every array in the realm: the realm starts having a bad time. The arrays made before are left in
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
    for (let blank of blankForTheEnd) {
        let before = snapshot(blank);
        class Base { constructor() { return blank; } }
        class Elements extends Base { 0 = 1; 3 = 1; }
        let attempts = [() => new Elements, () => Object.defineProperty(blank, 0, { value: 1 }), () => { "use strict"; blank[0] = 1; }, () => { "use strict"; blank[3] = 1; },
            () => Array.prototype.push.call(blank, 1), () => Array.prototype.unshift.call(blank, 1), () => Reflect.set(blank, 1, 1) || (() => { throw new TypeError; })()];
        for (let attempt of attempts) {
            for (let i = 0; i < 3; ++i) {
                let error;
                try { attempt(); } catch (e) { error = e; }
                shouldBe(error instanceof TypeError || error?.constructor?.name === "TypeError", true, String(attempt));
            }
            shouldBe(snapshot(blank), before, "no element storage, after the bad time: " + String(attempt));
        }
    }
    // Compiled push reaches JSArray::push() without Array.prototype.push's own checks. Hot, on an array without element storage
    // and on one with dictionary indexing; and with a setter on the prototype chain, which a put with such a receiver never asks.
    {
        let other = $vm.createGlobalObject();
        let blank = $vm.makePropertiesImmutable(other.Array.prototype);
        let named = int32Subject(); named.tag = 1; $vm.makePropertiesImmutable(named);
        let setterCalls = 0;
        Object.defineProperty(Array.prototype, 8, { set() { setterCalls++; }, configurable: true });
        $vm.haveABadTime(other);
        let blankBefore = snapshot(blank), namedBefore = snapshot(named);
        function push(array, value) { return array.push(value); }
        noInline(push);
        for (let i = 0; i < testLoopCount; ++i) {
            push([], i);
            let error;
            try { push(i % 2 ? named : blank, i); } catch (e) { error = e; }
            shouldBe(error?.constructor?.name, "TypeError");
        }
        delete Array.prototype[8];
        shouldBe(snapshot(blank), blankBefore);
        shouldBe(snapshot(named), namedBefore);
        shouldBe(setterCalls, 0);
        shouldBe(new other.Array(3)[1], undefined);
    }
    let late = $vm.makePropertiesImmutable(int32Subject()), lateBefore = snapshot(late);
    for (let mutator of mutators) { try { mutator(late); } catch (e) { if (!(e instanceof TypeError)) throw e; } }
    shouldBe(snapshot(late), lateBefore, "made immutable after the bad time");
    let fresh = []; fresh[3] = 1; fresh.push(2); shouldBe(fresh.length, 5);
}
