//@ defaultRun; run("no-cjit-force-ic-failure", "--useConcurrentJIT=false", "--forceICFailure=true")

// A name that resolves to a property of the global object is a ReferenceError when that
// property is not there, in every tier. The DFG and the FTL did a plain get by id of the
// global object when they could not prove where the property is, and a plain get by id
// of a property that is not there gives undefined.
// global-property-strict-write-not-found.js is the same for a write.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + String(expected) + " but got " + String(actual));
}

function shouldThrow(func, expected, what) {
    let error = null;
    let result;
    try {
        result = func();
    } catch (e) {
        error = e;
    }
    if (!error)
        throw new Error(what + ": did not throw, returned " + String(result));
    if (String(error) !== expected)
        throw new Error(what + ": threw " + String(error));
}

function isNotDefined(name) {
    return "ReferenceError: " + name + " is not defined";
}

const global = globalThis;
// A call that throws costs far more than one that returns, so the loops that throw are short. The longer one is
// for a function that is optimized while it throws. The shorter one is for a function that already is.
const throwCount = Math.ceil(testLoopCount / 20);
const fewThrows = Math.ceil(testLoopCount / 50);

// A data property that is there when the function is linked, and gone before the function is optimized.
global.linkedData = 1;
function readLinkedData() { return linkedData; }
noInline(readLinkedData);
shouldBe(readLinkedData(), 1, "linkedData before");
delete global.linkedData;
for (let i = 0; i < throwCount; ++i)
    shouldThrow(readLinkedData, isNotDefined("linkedData"), "linkedData at " + i);

// The same for a call, a construction and an update: the read comes first.
global.calledFunction = function () { return 11; };
global.constructedFunction = function () { };
global.counter = 0;
function callIt() { return calledFunction(); }
function constructIt() { return new constructedFunction; }
function increment() { return counter++; }
noInline(callIt);
noInline(constructIt);
noInline(increment);
shouldBe(callIt(), 11, "calledFunction before");
shouldBe(constructIt() instanceof constructedFunction, true, "constructedFunction before");
shouldBe(increment(), 0, "counter before");
delete global.calledFunction;
delete global.constructedFunction;
delete global.counter;
for (let i = 0; i < throwCount; ++i) {
    shouldThrow(callIt, isNotDefined("calledFunction"), "calledFunction at " + i);
    shouldThrow(constructIt, isNotDefined("constructedFunction"), "constructedFunction at " + i);
    shouldThrow(increment, isNotDefined("counter"), "counter at " + i);
}
shouldBe(Object.hasOwn(global, "counter"), false, "counter was not created");

// A data property with the optimized code already there when it goes away, and the property coming back.
global.warmData = 1;
function readWarmData() { return warmData; }
noInline(readWarmData);
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(readWarmData(), 1, "warmData before");
for (let round = 2; round < 5; ++round) {
    delete global.warmData;
    for (let i = 0; i < fewThrows; ++i)
        shouldThrow(readWarmData, isNotDefined("warmData"), "warmData, round " + round + " at " + i);
    global.warmData = round;
    for (let i = 0; i < throwCount; ++i)
        shouldBe(readWarmData(), round, "warmData again, round " + round);
}

// A name that is not there when the function is linked, appears, and goes away.
function readLateData() { return lateData; }
noInline(readLateData);
shouldThrow(readLateData, isNotDefined("lateData"), "lateData before");
global.lateData = 5;
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(readLateData(), 5, "lateData");
delete global.lateData;
for (let i = 0; i < fewThrows; ++i)
    shouldThrow(readLateData, isNotDefined("lateData"), "lateData at " + i);

// An accessor: the lower tiers cache no structure for it, so the optimized code never had one to check.
let getterCalls = 0;
Object.defineProperty(global, "accessorProperty", { get() { ++getterCalls; return 7; }, configurable: true });
function readAccessor() { return accessorProperty; }
noInline(readAccessor);
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(readAccessor(), 7, "accessorProperty");
shouldBe(getterCalls, testLoopCount, "getter calls");
delete global.accessorProperty;
for (let i = 0; i < fewThrows; ++i)
    shouldThrow(readAccessor, isNotDefined("accessorProperty"), "accessorProperty at " + i);
shouldBe(getterCalls, testLoopCount, "getter calls after the delete");
// A later script declares the name as a variable. The variable is no property of the structure of the global
// object, so an answer cached for "not there" would stay.
$.evalScript("var accessorProperty = 9;");
for (let i = 0; i < throwCount; ++i)
    shouldBe(readAccessor(), 9, "accessorProperty as a variable");

// A property that is there with the value undefined is not an error, whether it is a value or an accessor.
global.undefinedData = undefined;
Object.defineProperty(global, "undefinedAccessor", { get() { return undefined; }, configurable: true });
function readUndefinedData() { return undefinedData; }
function readUndefinedAccessor() { return undefinedAccessor; }
noInline(readUndefinedData);
noInline(readUndefinedAccessor);
for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(readUndefinedData(), undefined, "undefinedData");
    shouldBe(readUndefinedAccessor(), undefined, "undefinedAccessor");
}
delete global.undefinedData;
delete global.undefinedAccessor;
for (let i = 0; i < fewThrows; ++i) {
    shouldThrow(readUndefinedData, isNotDefined("undefinedData"), "undefinedData at " + i);
    shouldThrow(readUndefinedAccessor, isNotDefined("undefinedAccessor"), "undefinedAccessor at " + i);
}

// A getter that deletes its own property and returns undefined: that read found the property.
let selfDeletingIsArmed = false;
function defineSelfDeleting() {
    Object.defineProperty(global, "selfDeleting", {
        get() {
            if (selfDeletingIsArmed)
                delete global.selfDeleting;
            return undefined;
        },
        configurable: true,
    });
}
defineSelfDeleting();
function readSelfDeleting() { return selfDeleting; }
noInline(readSelfDeleting);
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(readSelfDeleting(), undefined, "selfDeleting");
selfDeletingIsArmed = true;
for (let i = 0; i < 10; ++i) {
    shouldBe(readSelfDeleting(), undefined, "selfDeleting, the read that deletes it, round " + i);
    shouldBe(Object.hasOwn(global, "selfDeleting"), false, "selfDeleting is gone, round " + i);
    shouldThrow(readSelfDeleting, isNotDefined("selfDeleting"), "selfDeleting after it deleted itself, round " + i);
    defineSelfDeleting();
}
delete global.selfDeleting;

// A property that the global object inherits.
Object.prototype.inheritedData = 3;
function readInherited() { return inheritedData; }
noInline(readInherited);
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(readInherited(), 3, "inheritedData");
delete Object.prototype.inheritedData;
for (let i = 0; i < fewThrows; ++i)
    shouldThrow(readInherited, isNotDefined("inheritedData"), "inheritedData at " + i);

// A built-in function of the global object.
function readBuiltin() { return escape; }
noInline(readBuiltin);
const originalEscape = escape;
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(readBuiltin(), originalEscape, "escape");
delete global.escape;
for (let i = 0; i < fewThrows; ++i)
    shouldThrow(readBuiltin, isNotDefined("escape"), "escape at " + i);

// The name "length", which the inline caches treat specially for arrays and strings.
Object.defineProperty(global, "length", { get() { return 4; }, configurable: true });
function readLength() { return length; }
noInline(readLength);
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(readLength(), 4, "length");
delete global.length;
for (let i = 0; i < fewThrows; ++i)
    shouldThrow(readLength, isNotDefined("length"), "length at " + i);

// The read inlined into its caller.
Object.defineProperty(global, "inlinedAccessor", { get() { return 13; }, configurable: true });
function readInlinedAccessor() { return inlinedAccessor; }
function callsReadInlinedAccessor() {
    try {
        return readInlinedAccessor();
    } catch (e) {
        return String(e);
    }
}
noInline(callsReadInlinedAccessor);
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(callsReadInlinedAccessor(), 13, "inlinedAccessor");
delete global.inlinedAccessor;
for (let i = 0; i < fewThrows; ++i)
    shouldBe(callsReadInlinedAccessor(), isNotDefined("inlinedAccessor"), "inlinedAccessor at " + i);

// The property comes and goes many times, as a value and as an accessor: more times than an inline cache has cases.
function readComesAndGoes() { return comesAndGoes; }
noInline(readComesAndGoes);
global.comesAndGoes = -1;
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(readComesAndGoes(), -1, "comesAndGoes before");
for (let cycle = 0; cycle < 40; ++cycle) {
    delete global.comesAndGoes;
    for (let i = 0; i < 5; ++i)
        shouldThrow(readComesAndGoes, isNotDefined("comesAndGoes"), "comesAndGoes, cycle " + cycle + " at " + i);
    if (cycle % 2)
        global.comesAndGoes = cycle;
    else
        Object.defineProperty(global, "comesAndGoes", { get() { return cycle; }, configurable: true });
    for (let i = 0; i < 50; ++i)
        shouldBe(readComesAndGoes(), cycle, "comesAndGoes, cycle " + cycle);
}
delete global.comesAndGoes;

// A lexical declaration of the name after the property is gone.
Object.defineProperty(global, "declaredLater", { get() { return 19; }, configurable: true });
function readDeclaredLater() { return declaredLater; }
noInline(readDeclaredLater);
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(readDeclaredLater(), 19, "declaredLater");
delete global.declaredLater;
for (let i = 0; i < 20; ++i)
    shouldThrow(readDeclaredLater, isNotDefined("declaredLater"), "declaredLater at " + i);
$.evalScript("let declaredLater = 23;");
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(readDeclaredLater(), 23, "declaredLater as a lexical binding");

// Properties whose getters are C++ functions, on the prototype chain of another global object, read by name in
// the realm of that global object.
const customHolder = $vm.createCustomTestGetterSetter();
const staticValueHolder = $vm.createStaticCustomValue();
const staticAccessorHolder = $vm.createStaticCustomAccessor();
Object.setPrototypeOf(staticAccessorHolder, null);
Object.setPrototypeOf(staticValueHolder, staticAccessorHolder);
Object.setPrototypeOf(customHolder, staticValueHolder);
const otherGlobal = $vm.createGlobalObject(customHolder);
function testCustomGetter(name, holder, expected) {
    const read = otherGlobal.Function("return " + name + ";");
    noInline(read);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(read(), expected, name);
    shouldBe(delete holder[name], true, "delete " + name);
    for (let i = 0; i < fewThrows; ++i)
        shouldThrow(read, isNotDefined(name), name + " at " + i);
}
// A custom value: its getter gets the holder. The second one returns undefined, which is not an error.
testCustomGetter("customValue", customHolder, customHolder);
testCustomGetter("customValue2", customHolder, undefined);
// A custom accessor: its getter gets the object that was read from.
testCustomGetter("customAccessor", customHolder, otherGlobal);
// Properties of a static table: the structure of the holder does not have them.
testCustomGetter("testStaticValue", staticValueHolder, undefined);
testCustomGetter("testStaticAccessor", staticAccessorHolder, undefined);

// typeof does not throw.
Object.defineProperty(global, "typeofAccessor", { get() { return 1; }, configurable: true });
function typeofIt() { return typeof typeofAccessor; }
noInline(typeofIt);
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(typeofIt(), "number", "typeof");
delete global.typeofAccessor;
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(typeofIt(), "undefined", "typeof at " + i);
