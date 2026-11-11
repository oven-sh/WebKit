//@ requireOptions("--useDollarVM=1")
// VM::DeletePropertyMode::IgnoreConfigurable deletes an element that is not configurable, as it deletes such a named property.
// Code that was compiled while the object had the element does not keep it: the DFG folds a read of an element that is read-only
// and not configurable into the code (on x86 alone, webkit.org/b/134641), so the object gets a new structure with the delete.
const loopCount = typeof testLoopCount === "number" ? testLoopCount : 20000;

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${String(expected)} but got ${String(actual)}`);
}

function shouldThrowTypeError(operation) {
    let error;
    try {
        operation();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof TypeError))
        throw new Error(`expected a TypeError but got ${String(error)}: ${operation}`);
}

function forceDelete(object, key) {
    return $vm.deletePropertyIgnoringConfigurable(object, key);
}

// Each holder is a constant of the code that reads it: Math through a property of the global object, the others as global constants.
const plain = {};
const array = [];
const dictionary = $vm.toUncacheableDictionary({ named: 1 });

function readMath() { return Math[7]; }
function readMathGuarded() { return 7 in Math ? Math[7] : "absent"; }
function readPlain() { return plain[7]; }
function readPlainGuarded() { return 7 in plain ? plain[7] : "absent"; }
function readArray() { return array[7]; }
function readArrayGuarded() { return 7 in array ? array[7] : "absent"; }
function readDictionary() { return dictionary[7]; }
function readDictionaryGuarded() { return 7 in dictionary ? dictionary[7] : "absent"; }
// These are inlined into the loop that calls them, so the element is folded into the code of the loop.
function readMathInlined() { return Math[7]; }
function readPlainInlined() { return plain[7]; }

for (const reader of [readMath, readMathGuarded, readPlain, readPlainGuarded, readArray, readArrayGuarded, readDictionary, readDictionaryGuarded])
    noInline(reader);

function warm(reader, expected) {
    for (let i = 0; i < loopCount; ++i)
        shouldBe(reader(), expected);
}
noInline(warm);

function warmInlined(expectedOfMath, expectedOfPlain) {
    for (let i = 0; i < loopCount; ++i) {
        shouldBe(readMathInlined(), expectedOfMath);
        shouldBe(readPlainInlined(), expectedOfPlain);
    }
}
noInline(warmInlined);

const holders = [
    { object: Math, read: readMath, readGuarded: readMathGuarded },
    { object: plain, read: readPlain, readGuarded: readPlainGuarded },
    { object: array, read: readArray, readGuarded: readArrayGuarded },
    { object: dictionary, read: readDictionary, readGuarded: readDictionaryGuarded },
];

function checkAbsent({ object, read, readGuarded }) {
    shouldBe(read(), undefined);
    shouldBe(readGuarded(), "absent");
    shouldBe(7 in object, false);
    shouldBe(Object.getOwnPropertyDescriptor(object, 7), undefined);
    shouldBe(Reflect.ownKeys(object).includes("7"), false);
}

// A read-only element: warmed readers return it, the default mode refuses to delete it, and the forced delete takes it from them.
for (const value of ["first", "second", "third"]) {
    for (const holder of holders) {
        Object.defineProperty(holder.object, 7, { value });
        warm(holder.read, value);
        warm(holder.readGuarded, value);
    }
    warmInlined(value, value);

    for (const holder of holders) {
        shouldBe(delete holder.object[7], false);
        shouldThrowTypeError(() => {
            "use strict";
            delete holder.object[7];
        });
        shouldBe(Reflect.deleteProperty(holder.object, 7), false);
        shouldBe(holder.read(), value);

        shouldBe(forceDelete(holder.object, 7), true);
        checkAbsent(holder);
    }
    shouldBe(readMathInlined(), undefined);
    shouldBe(readPlainInlined(), undefined);
    warmInlined(undefined, undefined);
    for (const holder of holders) {
        warm(holder.read, undefined);
        warm(holder.readGuarded, "absent");
        // An element that is absent is deleted in both modes.
        shouldBe(forceDelete(holder.object, 7), true);
        shouldBe(delete holder.object[7], true);
    }
}
shouldBe(dictionary.named, 1);

// A writable element that is not configurable, and an accessor that is not.
for (const holder of holders) {
    const { object } = holder;
    let getterCalls = 0;
    Object.defineProperty(object, 7, { value: "writable", writable: true });
    Object.defineProperty(object, 8, { get() { ++getterCalls; return "getter"; } });
    warm(holder.read, "writable");
    shouldBe(object[8], "getter");
    shouldBe(delete object[7], false);
    shouldBe(delete object[8], false);

    shouldBe(forceDelete(object, 7), true);
    shouldBe(forceDelete(object, 8), true);
    checkAbsent(holder);
    shouldBe(8 in object, false);
    shouldBe(object[8], undefined);
    shouldBe(getterCalls, 1);

    // A configurable element is deleted as in the default mode.
    object[7] = "plain";
    shouldBe(forceDelete(object, 7), true);
    checkAbsent(holder);
}

// An element that Object.freeze() made not configurable. The object stays frozen.
{
    const frozen = { named: 1 };
    frozen[7] = "frozen";
    Object.freeze(frozen);
    shouldBe(delete frozen[7], false);
    shouldBe(forceDelete(frozen, 7), true);
    shouldBe(7 in frozen, false);
    shouldBe(Object.isFrozen(frozen), true);
    shouldThrowTypeError(() => {
        "use strict";
        frozen[7] = "again";
    });
}

// A named property is deleted as before.
{
    const object = {};
    Object.defineProperty(object, "fixed", { value: 1 });
    shouldBe(delete object.fixed, false);
    shouldBe(forceDelete(object, "fixed"), true);
    shouldBe("fixed" in object, false);
}

// The length of an array can go below an element only once the element is gone.
{
    const list = [1, 2];
    Object.defineProperty(list, 7, { value: "fixed" });
    shouldBe(list.length, 8);
    shouldThrowTypeError(() => {
        "use strict";
        list.length = 0;
    });
    shouldBe(list.length, 8);
    shouldBe(forceDelete(list, 7), true);
    shouldBe(list.length, 8);
    list.length = 0;
    shouldBe(list.length, 0);
    list.push("pushed");
    shouldBe(list[0], "pushed");
}

// The global object holds its elements behind the proxy that `globalThis` is.
Object.defineProperty(globalThis, 7, { value: "global" });
shouldBe(globalThis[7], "global");
shouldBe(delete globalThis[7], false);
shouldBe(forceDelete(globalThis, 7), true);
shouldBe(7 in globalThis, false);
shouldBe(globalThis[7], undefined);

// A setter that the prototype of arrays gets after the forced delete is called for their stores. The writable element comes first:
// it alone leaves the prototype without the mark of an object that intercepts indexed stores, and the setter has to bring it.
function store(target, value) { target[3] = value; }
noInline(store);
for (const writable of [true, false]) {
    class Derived extends Array { }
    const prototype = Derived.prototype;
    Object.defineProperty(prototype, 3, { value: "fixed", writable });
    shouldBe(new Derived()[3], "fixed");
    shouldBe(forceDelete(prototype, 3), true);
    shouldBe(new Derived()[3], undefined);
    for (let i = 0; i < loopCount; ++i) {
        const derived = new Derived();
        store(derived, i);
        shouldBe(derived[3], i);
    }

    let setterCalls = 0;
    Object.defineProperty(prototype, 3, { set(value) { ++setterCalls; }, configurable: true });
    for (let i = 0; i < loopCount; ++i) {
        const derived = new Derived();
        store(derived, i);
        shouldBe(Object.hasOwn(derived, 3), false);
    }
    shouldBe(setterCalls, loopCount);
}
