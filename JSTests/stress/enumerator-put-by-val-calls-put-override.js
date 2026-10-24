// A for-in loop that stores to a property of the object it enumerates (op_enumerator_put_by_val in OwnStructureMode)
// writes the slot of the property. It has to call put() instead when the class of the object overrides it.
// The put() of ObjectDoingSideEffectPutWithCorrectSlotStatus converts the value to a string.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + typeof expected + " " + expected + " but got " + typeof actual + " " + actual);
}

function storeToEachProperty(object, value) {
    for (var name in object)
        object[name] = value;
}
noInline(storeToEachProperty);

function create() {
    var object = $vm.createObjectDoingSideEffectPutWithCorrectSlotStatus();
    object.a = 0;
    object.b = 0;
    return object;
}

var object = create();
var dictionary = $vm.toCacheableDictionary(create());

// Names that are array indices take the same route: the loop calls put() with the name.
var indexed = create();
indexed[0] = 0;
indexed[1] = 0;

// The same loop on an object without a put() override still stores the value as it is.
var plain = { a: 0, b: 0 };

for (var i = 0; i < testLoopCount; ++i) {
    storeToEachProperty(object, i);
    shouldBe(object.a, String(i), "a " + i);
    shouldBe(object.b, String(i), "b " + i);

    storeToEachProperty(dictionary, i);
    shouldBe(dictionary.a, String(i), "dictionary a " + i);
    shouldBe(dictionary.b, String(i), "dictionary b " + i);

    storeToEachProperty(indexed, i);
    shouldBe(indexed[0], String(i), "indexed 0 " + i);
    shouldBe(indexed[1], String(i), "indexed 1 " + i);
    shouldBe(indexed.a, String(i), "indexed a " + i);

    storeToEachProperty(plain, i);
    shouldBe(plain.a, i, "plain a " + i);
    shouldBe(plain.b, i, "plain b " + i);
}

// The same loop in a function that only sees plain objects until the DFG and the FTL compiled it. Its profile is
// OwnStructureMode then, and the compiled loop stores by offset. An object of the class has to leave that code.
function storeToEachPropertyLate(object, value) {
    for (var name in object)
        object[name] = value;
}
noInline(storeToEachPropertyLate);

for (var i = 0; i < testLoopCount; ++i) {
    storeToEachPropertyLate(plain, i);
    shouldBe(plain.a, i, "plain a before " + i);
    shouldBe(plain.b, i, "plain b before " + i);
}

var lateObject = create();
var lateDictionary = $vm.toCacheableDictionary(create());

for (var i = 0; i < testLoopCount; ++i) {
    storeToEachPropertyLate(lateObject, i);
    shouldBe(lateObject.a, String(i), "late a " + i);
    shouldBe(lateObject.b, String(i), "late b " + i);

    storeToEachPropertyLate(lateDictionary, i);
    shouldBe(lateDictionary.a, String(i), "late dictionary a " + i);
    shouldBe(lateDictionary.b, String(i), "late dictionary b " + i);

    storeToEachPropertyLate(plain, i);
    shouldBe(plain.a, i, "late plain a " + i);
    shouldBe(plain.b, i, "late plain b " + i);
}
