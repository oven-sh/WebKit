// JSON.parse stores what the reviver returns with CreateDataProperty, which is defineOwnProperty(). The walk writes the
// slot of an existing property instead. It has to call defineOwnProperty() when the class of the holder overrides it,
// and the reviver can put any object in the tree. The defineOwnProperty() of
// ObjectDoingSideEffectPutWithCorrectSlotStatus converts the value to a string.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + typeof expected + " " + expected + " but got " + typeof actual + " " + actual);
}

var object = $vm.createObjectDoingSideEffectPutWithCorrectSlotStatus();
object.a = 0;

var revived = 0;
function reviver(name, value) {
    if (name === "first") {
        // The walk reads "second" after this call, so it walks object next.
        this.second = object;
        return value;
    }
    if (this === object && name === "a")
        return revived;
    return value;
}

for (var i = 0; i < testLoopCount; ++i) {
    revived = i;
    var result = JSON.parse('{"first":1,"second":{}}', reviver);
    shouldBe(result.second, object, "second " + i);
    shouldBe(object.a, String(i), "a " + i);
}
