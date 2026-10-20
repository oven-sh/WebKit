// hasOwnProperty.call(value, key) and Object.hasOwn(value, key) inside for (key in value) become EnumeratorHasOwnProperty, which takes
// any cell. The check that value is an object has to stay: a string is a cell too.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + String(expected) + " but got " + String(actual));
}

const hasOwnProperty = Object.prototype.hasOwnProperty;

function ownKeysByCall(value) {
    let keys = "";
    for (const key in value) {
        if (hasOwnProperty.call(value, key))
            keys += key + ",";
    }
    return keys;
}
noInline(ownKeysByCall);

function ownKeysByHasOwn(value) {
    let keys = "";
    for (const key in value) {
        if (Object.hasOwn(value, key))
            keys += key + ",";
    }
    return keys;
}
noInline(ownKeysByHasOwn);

function ownKeysByCallInlined(value) {
    let keys = "";
    for (const key in value) {
        if (hasOwnProperty.call(value, key))
            keys += key + ",";
    }
    return keys;
}

function ownKeysByHasOwnInlined(value) {
    let keys = "";
    for (const key in value) {
        if (Object.hasOwn(value, key))
            keys += key + ",";
    }
    return keys;
}

function runByCall(useString) {
    return useString ? ownKeysByCallInlined("ab") : ownKeysByCallInlined({ a: 1, b: 2 });
}
noInline(runByCall);

function runByHasOwn(useString) {
    return useString ? ownKeysByHasOwnInlined("ab") : ownKeysByHasOwnInlined({ a: 1, b: 2 });
}
noInline(runByHasOwn);

function ownKeysOfOther(value, other) {
    let keys = "";
    for (const key in value) {
        if (hasOwnProperty.call(other, key))
            keys += key + ",";
    }
    return keys;
}
noInline(ownKeysOfOther);

const objectWithInheritedKey = Object.assign(Object.create({ inherited: 1 }), { own: 2 });
const valuesWithoutKeys = [Symbol("s"), 2n ** 70n, 5, true, null, undefined];

for (let i = 0; i < 10; ++i) {
    shouldBe(runByCall(true), "0,1,");
    shouldBe(runByHasOwn(true), "0,1,");
}

for (let i = 0; i < testLoopCount * 10; ++i) {
    shouldBe(runByCall(false), "a,b,");
    shouldBe(runByHasOwn(false), "a,b,");
    shouldBe(ownKeysByCall({ a: 1, b: 2 }), "a,b,");
    shouldBe(ownKeysByHasOwn({ a: 1, b: 2 }), "a,b,");
    shouldBe(ownKeysByCall(objectWithInheritedKey), "own,");
    shouldBe(ownKeysByHasOwn(objectWithInheritedKey), "own,");
    shouldBe(ownKeysByCall([7, 8]), "0,1,");
    shouldBe(ownKeysByHasOwn([7, 8]), "0,1,");
    shouldBe(ownKeysOfOther({ a: 1, b: 2 }, { b: 3 }), "b,");
}

shouldBe(runByCall(true), "0,1,");
shouldBe(runByHasOwn(true), "0,1,");
for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(ownKeysByCall("ab"), "0,1,");
    shouldBe(ownKeysByHasOwn("ab"), "0,1,");
    shouldBe(ownKeysByCall("a" + String(i % 10)), "0,1,");
    shouldBe(ownKeysByHasOwn("a" + String(i % 10)), "0,1,");
    shouldBe(ownKeysByCall({ a: 1, b: 2 }), "a,b,");
    shouldBe(ownKeysByHasOwn({ a: 1, b: 2 }), "a,b,");
}

for (let i = 0; i < testLoopCount; ++i) {
    for (const value of valuesWithoutKeys) {
        shouldBe(ownKeysByCall(value), "");
        shouldBe(ownKeysByHasOwn(value), "");
    }
    shouldBe(ownKeysOfOther({ 0: 1 }, "x"), "0,");
    shouldBe(ownKeysOfOther({ a: 1, b: 2 }, { b: 3 }), "b,");
}
