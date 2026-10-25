//@ defaultRun; run("no-cjit-force-ic-failure", "--useConcurrentJIT=false", "--forceICFailure=true")

// A write in strict mode code to a name that resolves to a property of the global object is
// a ReferenceError when that property is not there, in every tier, and does not add it. A
// plain put by id of the global object adds the property.
// global-property-read-not-found.js is the same for a read.

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
global.strictData = 0;
function writeStrict(value) { "use strict"; strictData = value; }
noInline(writeStrict);
writeStrict(1);
shouldBe(global.strictData, 1, "strictData before");
delete global.strictData;
for (let i = 0; i < throwCount; ++i) {
    shouldThrow(() => writeStrict(i), isNotDefined("strictData"), "strictData at " + i);
    shouldBe(Object.hasOwn(global, "strictData"), false, "strictData was not created at " + i);
}

// The same with the optimized code already there when the property goes away, and the property coming back.
global.warmStrictData = 0;
function writeWarmStrict(value) { "use strict"; warmStrictData = value; }
noInline(writeWarmStrict);
for (let i = 0; i < testLoopCount; ++i)
    writeWarmStrict(i);
shouldBe(global.warmStrictData, testLoopCount - 1, "warmStrictData before");
for (let round = 0; round < 3; ++round) {
    delete global.warmStrictData;
    for (let i = 0; i < fewThrows; ++i) {
        shouldThrow(() => writeWarmStrict(i), isNotDefined("warmStrictData"), "warmStrictData, round " + round + " at " + i);
        shouldBe(Object.hasOwn(global, "warmStrictData"), false, "warmStrictData was not created, round " + round + " at " + i);
    }
    global.warmStrictData = -1;
    for (let i = 0; i < throwCount; ++i) {
        writeWarmStrict(i);
        shouldBe(global.warmStrictData, i, "warmStrictData again, round " + round);
    }
}

// A setter: the lower tiers cache no structure for it, so the optimized code never had one to check.
let setterValue = 0;
Object.defineProperty(global, "strictAccessor", { get() { return setterValue; }, set(value) { setterValue = value; }, configurable: true });
function writeStrictAccessor(value) { "use strict"; strictAccessor = value; }
noInline(writeStrictAccessor);
for (let i = 0; i < testLoopCount; ++i)
    writeStrictAccessor(i);
shouldBe(setterValue, testLoopCount - 1, "strictAccessor before");
delete global.strictAccessor;
for (let i = 0; i < fewThrows; ++i) {
    shouldThrow(() => writeStrictAccessor(-1), isNotDefined("strictAccessor"), "strictAccessor at " + i);
    shouldBe(Object.hasOwn(global, "strictAccessor"), false, "strictAccessor was not created at " + i);
}
shouldBe(setterValue, testLoopCount - 1, "strictAccessor after");

// A setter that the global object inherits.
let inheritedSetterValue = 0;
Object.defineProperty(Object.prototype, "inheritedSetter", { get() { return inheritedSetterValue; }, set(value) { inheritedSetterValue = value; }, configurable: true });
function writeInheritedSetter(value) { "use strict"; inheritedSetter = value; }
noInline(writeInheritedSetter);
for (let i = 0; i < testLoopCount; ++i)
    writeInheritedSetter(i);
shouldBe(inheritedSetterValue, testLoopCount - 1, "inheritedSetter before");
shouldBe(Object.hasOwn(global, "inheritedSetter"), false, "inheritedSetter is not a property of the global object");
delete Object.prototype.inheritedSetter;
for (let i = 0; i < fewThrows; ++i) {
    shouldThrow(() => writeInheritedSetter(-1), isNotDefined("inheritedSetter"), "inheritedSetter at " + i);
    shouldBe("inheritedSetter" in global, false, "inheritedSetter was not created at " + i);
}
shouldBe(inheritedSetterValue, testLoopCount - 1, "inheritedSetter after");

// A later script declares a variable with the name of an inherited setter. The variable is no property of the
// structure of the global object, so a write cached for that structure would still call the setter.
let shadowedSetterCalls = 0;
Object.defineProperty(Object.prototype, "shadowedSetter", { get() { return -1; }, set(value) { ++shadowedSetterCalls; }, configurable: true });
function writeShadowedSetter(value) { "use strict"; shadowedSetter = value; }
noInline(writeShadowedSetter);
for (let i = 0; i < testLoopCount; ++i)
    writeShadowedSetter(i);
shouldBe(shadowedSetterCalls, testLoopCount, "shadowedSetter calls before");
$.evalScript("var shadowedSetter = 33;");
for (let i = 0; i < throwCount; ++i)
    writeShadowedSetter(i);
shouldBe(shadowedSetterCalls, testLoopCount, "shadowedSetter calls after the declaration");
shouldBe(new Function("return shadowedSetter;")(), throwCount - 1, "shadowedSetter as a variable");
delete Object.prototype.shadowedSetter;

// An accessor with no setter: a TypeError while it is there, a ReferenceError once it is gone.
Object.defineProperty(global, "getterOnly", { get() { return 29; }, configurable: true });
function writeGetterOnly(value, reallyWrite) { "use strict"; if (reallyWrite) getterOnly = value; }
noInline(writeGetterOnly);
for (let i = 0; i < testLoopCount; ++i)
    writeGetterOnly(i, false);
for (let i = 0; i < 20; ++i)
    shouldThrow(() => writeGetterOnly(i, true), "TypeError: Attempted to assign to readonly property.", "getterOnly at " + i);
delete global.getterOnly;
for (let i = 0; i < fewThrows; ++i) {
    shouldThrow(() => writeGetterOnly(i, true), isNotDefined("getterOnly"), "getterOnly, gone, at " + i);
    shouldBe(Object.hasOwn(global, "getterOnly"), false, "getterOnly was not created at " + i);
}

// A writable property that the global object inherits: the write adds the property to the global object.
// Once both are gone, nothing is added.
Object.prototype.inheritedWritable = 31;
function writeInheritedWritable(value) { "use strict"; inheritedWritable = value; }
noInline(writeInheritedWritable);
for (let i = 0; i < testLoopCount; ++i) {
    writeInheritedWritable(i);
    shouldBe(global.inheritedWritable, i, "inheritedWritable");
    if (!(i % 1024)) {
        shouldBe(delete global.inheritedWritable, true, "delete inheritedWritable");
        shouldBe(global.inheritedWritable, 31, "inheritedWritable is inherited again");
    }
}
delete global.inheritedWritable;
delete Object.prototype.inheritedWritable;
for (let i = 0; i < fewThrows; ++i) {
    shouldThrow(() => writeInheritedWritable(i), isNotDefined("inheritedWritable"), "inheritedWritable, gone, at " + i);
    shouldBe("inheritedWritable" in global, false, "inheritedWritable was not created at " + i);
}

// The value that is written deletes the property: the name was found, but the write still finds nothing to write to.
global.deletedByItsValue = 0;
function writeDeletedByItsValue(reallyDelete) { "use strict"; deletedByItsValue = reallyDelete ? delete global.deletedByItsValue : false; }
noInline(writeDeletedByItsValue);
for (let i = 0; i < testLoopCount; ++i)
    writeDeletedByItsValue(false);
for (let i = 0; i < 20; ++i) {
    shouldThrow(() => writeDeletedByItsValue(true), isNotDefined("deletedByItsValue"), "deletedByItsValue at " + i);
    shouldBe(Object.hasOwn(global, "deletedByItsValue"), false, "deletedByItsValue was not created at " + i);
    global.deletedByItsValue = 0;
}
delete global.deletedByItsValue;

// A write in sloppy mode code does create the property.
global.sloppyData = 0;
function writeSloppy(value) { sloppyData = value; }
noInline(writeSloppy);
for (let i = 0; i < testLoopCount; ++i)
    writeSloppy(i);
for (let i = 0; i < fewThrows; ++i) {
    delete global.sloppyData;
    writeSloppy(i);
    shouldBe(global.sloppyData, i, "sloppyData at " + i);
}
delete global.sloppyData;

// After this many deletes in a row the global object is a dictionary that nothing is cached for, and the structures
// it had before are dead: a collection resets the inline caches that hold them. Names that are there are still read
// and written, and names that are not are still errors.
global.survivor = 37;
Object.defineProperty(global, "survivingAccessor", { get() { return 41; }, set(value) { setterValue = value; }, configurable: true });
global.casualty = 43;
function readSurvivor() { return survivor + survivingAccessor; }
function writeSurvivor(value) { "use strict"; survivor = value; survivingAccessor = value; }
function readCasualty() { return casualty; }
function writeCasualty(value) { "use strict"; casualty = value; }
noInline(readSurvivor);
noInline(writeSurvivor);
noInline(readCasualty);
noInline(writeCasualty);
function checkTheOnesThatAreThere(what) {
    shouldBe(readSurvivor(), 37 + 41, "survivor, " + what);
    writeSurvivor(37);
    shouldBe(setterValue, 37, "survivingAccessor was written, " + what);
}
for (let round = 0; round < 2; ++round) {
    for (let i = 0; i < testLoopCount / 2; ++i) {
        checkTheOnesThatAreThere("round " + round);
        shouldBe(readCasualty(), 43, "casualty, round " + round);
        writeCasualty(43);
    }
    if (!round) {
        for (let i = 0; i < 200; ++i)
            global["temporary" + i] = i;
        for (let i = 0; i < 200; ++i)
            delete global["temporary" + i];
        fullGC();
    }
}
delete global.casualty;
for (let i = 0; i < fewThrows; ++i) {
    checkTheOnesThatAreThere("at " + i);
    shouldThrow(readCasualty, isNotDefined("casualty"), "casualty, read at " + i);
    shouldThrow(() => writeCasualty(i), isNotDefined("casualty"), "casualty, written at " + i);
    shouldBe(Object.hasOwn(global, "casualty"), false, "casualty was not created at " + i);
}
