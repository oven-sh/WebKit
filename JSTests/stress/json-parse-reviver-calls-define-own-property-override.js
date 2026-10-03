// JSON.parse stores what the reviver returns with CreateDataProperty, which is defineOwnProperty(). The walk writes the
// slot of an existing property instead. It has to call defineOwnProperty() when the class of the holder overrides it,
// and the reviver can put any object in the tree. The defineOwnProperty() of ObjectDoingSideEffectDefineOwnProperty
// converts the value to a string.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + typeof expected + " " + expected + " but got " + typeof actual + " " + actual);
}

function create() {
    var object = $vm.createObjectDoingSideEffectDefineOwnProperty();
    object.a = 0;
    return object;
}

var holders = [create(), $vm.toCacheableDictionary(create()), $vm.toUncacheableDictionary(create())];

var holder;
var revived = 0;
function reviver(name, value) {
    if (name === "first") {
        // The walk reads "second" after this call, so it walks holder next.
        this.second = holder;
        return value;
    }
    if (this === holder && name === "a")
        return revived;
    return value;
}

for (var i = 0; i < testLoopCount; ++i) {
    revived = i;
    for (var j = 0; j < holders.length; ++j) {
        holder = holders[j];
        var result = JSON.parse('{"first":1,"second":{}}', reviver);
        shouldBe(result.second, holder, "second " + i + " of holder " + j);
        shouldBe(holder.a, String(i), "a " + i + " of holder " + j);
    }
}
