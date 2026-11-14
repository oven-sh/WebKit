//@ requireOptions("--useDollarVM=1")
// VM::DeletePropertyMode::IgnoreConfigurable deletes an element that is not configurable, as it deletes such a named property.
// Code that was compiled while the object had the element does not keep it: the DFG folds a read of an element that is read-only
// and not configurable into the code (on x86 alone, webkit.org/b/134641) behind a check or a watchpoint of the object's structure,
// so the delete gives the object a structure that no other object has and that no compiled code lists.
const loopCount = typeof testLoopCount === "number" ? testLoopCount : 20000;
const hasDFG = $vm.useDFGJIT();

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

// Where there is a DFG, until it has compiled the reader.
function warm(reader, expected) {
    for (let i = 0; i < loopCount || (hasDFG && !numberOfDFGCompiles(reader) && i < 100 * loopCount); ++i)
        shouldBe(reader(), expected);
}
noInline(warm);

// Each holder is a constant of the code that reads it: Math through a property of the global object, the others as global constants.
const plain = { named: 1 };
const array = [];
array.named = 1;
const dictionary = $vm.toUncacheableDictionary({ named: 1 });
const cacheable = $vm.toCacheableDictionary({ named: 1 });

function readMath() { return Math[7]; }
function readMathGuarded() { return 7 in Math ? Math[7] : "absent"; }
function readPlain() { return plain[7]; }
function readPlainGuarded() { return 7 in plain ? plain[7] : "absent"; }
function readArray() { return array[7]; }
function readArrayGuarded() { return 7 in array ? array[7] : "absent"; }
function readDictionary() { return dictionary[7]; }
function readDictionaryGuarded() { return 7 in dictionary ? dictionary[7] : "absent"; }
function readCacheable() { return cacheable[7]; }
function readCacheableGuarded() { return 7 in cacheable ? cacheable[7] : "absent"; }
// These are inlined into the loop that calls them, so the element is folded into the code of the loop.
function readMathInlined() { return Math[7]; }
function readPlainInlined() { return plain[7]; }

function warmInlined(expectedOfMath, expectedOfPlain) {
    for (let i = 0; i < loopCount; ++i) {
        shouldBe(readMathInlined(), expectedOfMath);
        shouldBe(readPlainInlined(), expectedOfPlain);
    }
}
noInline(warmInlined);

const holders = [
    { object: Math, name: "Math", named: "abs", read: readMath, readGuarded: readMathGuarded },
    { object: plain, name: "plain", named: "named", read: readPlain, readGuarded: readPlainGuarded },
    { object: array, name: "array", named: "named", read: readArray, readGuarded: readArrayGuarded },
    { object: dictionary, name: "dictionary", named: "named", read: readDictionary, readGuarded: readDictionaryGuarded },
    { object: cacheable, name: "cacheable", named: "named", read: readCacheable, readGuarded: readCacheableGuarded },
];
for (const { read, readGuarded } of holders) {
    noInline(read);
    noInline(readGuarded);
}

// A reader that looks for a named property first, so that its code checks the structure of the holder before it comes to the
// element. A structure that nothing watches any more is still checked by such code. Each round has readers with a text of their own:
// functions of one text share what they have seen, and after three structures the check gives way to a lookup.
function makeCheckedReader({ name, named }, round) {
    const reader = new Function(`/* ${round} */ return ${JSON.stringify(named)} in ${name} ? ${name}[7] : "absent";`);
    noInline(reader);
    return reader;
}

function checkAbsent({ object, read, readGuarded }) {
    shouldBe(read(), undefined);
    shouldBe(readGuarded(), "absent");
    shouldBe(7 in object, false);
    shouldBe(Object.getOwnPropertyDescriptor(object, 7), undefined);
    shouldBe(Reflect.ownKeys(object).includes("7"), false);
}

// A read-only element: warmed readers return it, the default mode refuses to delete it, and the forced delete takes it from them.
for (const value of ["first", "second", "third"]) {
    const checkedReaders = holders.map(holder => makeCheckedReader(holder, value));
    for (const [at, holder] of holders.entries()) {
        Object.defineProperty(holder.object, 7, { value });
        warm(holder.read, value);
        warm(holder.readGuarded, value);
        warm(checkedReaders[at], value);
    }
    warmInlined(value, value);

    for (const [at, holder] of holders.entries()) {
        shouldBe(delete holder.object[7], false);
        shouldThrowTypeError(() => {
            "use strict";
            delete holder.object[7];
        });
        shouldBe(Reflect.deleteProperty(holder.object, 7), false);
        shouldBe(holder.read(), value);

        shouldBe(forceDelete(holder.object, 7), true);
        checkAbsent(holder);
        shouldBe(checkedReaders[at](), undefined);
    }
    shouldBe(readMathInlined(), undefined);
    shouldBe(readPlainInlined(), undefined);
    warmInlined(undefined, undefined);
    for (const [at, holder] of holders.entries()) {
        warm(holder.read, undefined);
        warm(holder.readGuarded, "absent");
        warm(checkedReaders[at], undefined);
        // An element that is absent is deleted in both modes.
        shouldBe(forceDelete(holder.object, 7), true);
        shouldBe(delete holder.object[7], true);
    }
}
shouldBe(dictionary.named, 1);
shouldBe(cacheable.named, 1);

// Two objects of one structure. The forced delete of the first leaves no structure behind that the second comes to with its own:
// the reader of the second is compiled in between, and its check lists every structure that the access to the named property saw.
function checkTwins(first, second, readFirst, readSecond) {
    Object.defineProperty(first, 7, { value: "first" });
    shouldBe(forceDelete(first, 7), true);
    Object.defineProperty(second, 7, { value: "second" });
    for (let i = 0; i < loopCount || (hasDFG && !numberOfDFGCompiles(readSecond) && i < 100 * loopCount); ++i) {
        shouldBe(readFirst(), undefined);
        shouldBe(readSecond(), "second");
    }
    shouldBe(forceDelete(second, 7), true);
    shouldBe(readSecond(), undefined);
    shouldBe(7 in second, false);
    shouldBe(readFirst(), undefined);
}
function makeTwin() { return { named: 1 }; }
const firstTwin = makeTwin();
const secondTwin = makeTwin();
// The branch comes first: the DFG then reaches `named` before it knows which object it has, and checks for each structure seen there.
function readNamedThenElement(object, skip) {
    if (skip)
        return "skipped";
    object.named;
    return object[7];
}
function readFirstTwin() { return readNamedThenElement(firstTwin, false); }
function readSecondTwin() { return readNamedThenElement(secondTwin, false); }
noInline(readFirstTwin);
noInline(readSecondTwin);
checkTwins(firstTwin, secondTwin, readFirstTwin, readSecondTwin);

// Two built-in functions have one structure as well, once each has its `length`: a function gets that property when it is first asked for.
shouldBe(Math.cos.length, 1);
shouldBe(Math.sin.length, 1);
function readLengthThenElement(object, skip) {
    if (skip)
        return "skipped";
    object.length;
    return object[7];
}
function readCos() { return readLengthThenElement(Math.cos, false); }
function readSin() { return readLengthThenElement(Math.sin, false); }
noInline(readCos);
noInline(readSin);
checkTwins(Math.cos, Math.sin, readCos, readSin);

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
