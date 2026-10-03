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

var object = $vm.createObjectDoingSideEffectPutWithCorrectSlotStatus();
object.a = 0;
object.b = 0;

// The same loop on an object without a put() override still stores the value as it is.
var plain = { a: 0, b: 0 };

for (var i = 0; i < testLoopCount; ++i) {
    storeToEachProperty(object, i);
    shouldBe(object.a, String(i), "a " + i);
    shouldBe(object.b, String(i), "b " + i);

    storeToEachProperty(plain, i);
    shouldBe(plain.a, i, "plain a " + i);
    shouldBe(plain.b, i, "plain b " + i);
}
