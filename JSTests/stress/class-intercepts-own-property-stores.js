// Structure::classInterceptsOwnPropertyStores() is true for a class that overrides put() or defineOwnProperty().
// It is false for a class that also overrides getOwnPropertySlot(). That part keeps the for-in fast path for arrays,
// functions, RegExp objects and errors, and the DFG store fold for the global object.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + expected + " but got " + actual);
}

function argumentsObject() {
    return arguments;
}

var doesNotIntercept = {
    "object": {},
    "object with a null prototype": Object.create(null),
    "Date": new Date(0),
    "array": [1, 2, 3],
    "function": function () { },
    "RegExp": /x/,
    "Error": new Error("message"),
    "arguments": argumentsObject(1, 2),
    "typed array": new Uint8Array(4),
    "String object": new String("abc"),
    "global object": globalThis,
    "defineOwnProperty() and getOwnPropertySlot() class": $vm.createObjectDoingSideEffectDefineOwnPropertyOverridingGetOwnPropertySlot(),
};

for (var name in doesNotIntercept)
    shouldBe($vm.classInterceptsOwnPropertyStores(doesNotIntercept[name]), false, name);

shouldBe($vm.classInterceptsOwnPropertyStores(1), false, "number");
shouldBe($vm.classInterceptsOwnPropertyStores("abc"), false, "string");
shouldBe($vm.classInterceptsOwnPropertyStores(undefined), false, "undefined");

var putObject = $vm.createObjectDoingSideEffectPutWithCorrectSlotStatus();
var defineObject = $vm.createObjectDoingSideEffectDefineOwnProperty();
shouldBe($vm.classInterceptsOwnPropertyStores(putObject), true, "put() class");
shouldBe($vm.classInterceptsOwnPropertyStores(defineObject), true, "defineOwnProperty() class");

// The answer is a constant of the class. A transition or a dictionary does not change it.
putObject.a = 0;
defineObject.a = 0;
shouldBe($vm.classInterceptsOwnPropertyStores(putObject), true, "put() class after a transition");
shouldBe($vm.classInterceptsOwnPropertyStores(defineObject), true, "defineOwnProperty() class after a transition");
shouldBe($vm.classInterceptsOwnPropertyStores($vm.toCacheableDictionary(putObject)), true, "put() class as a dictionary");
shouldBe($vm.classInterceptsOwnPropertyStores($vm.toUncacheableDictionary(defineObject)), true, "defineOwnProperty() class as a dictionary");
