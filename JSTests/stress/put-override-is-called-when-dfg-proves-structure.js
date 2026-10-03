// A store to an object whose class overrides put() has to call put(), also when the DFG has proved the structure of
// the object. PutByStatus::computeFor(StructureSet) has no PutPropertySlot to ask, so it has to look at the class.
// The put() and defineOwnProperty() of ObjectDoingSideEffectPutWithCorrectSlotStatus convert the value to a string.
// A store that calls neither leaves a value that is not a string.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + typeof expected + " " + expected + " but got " + typeof actual + " " + actual);
}

function create() {
    return $vm.createObjectDoingSideEffectPutWithCorrectSlotStatus();
}

// The base is a constant, so the DFG knows its structure. The load from JIT code makes the structure watch x for
// replacement, and the next store fires that watchpoint set. After that the store looks like a plain replace.
var constantBase = create();
constantBase.x = 0;

function loadFromConstantBase() {
    return constantBase.x;
}
noInline(loadFromConstantBase);

function storeById(value) {
    constantBase.x = value;
}
noInline(storeById);

var key = "x";
function storeByVal(value) {
    constantBase[key] = value;
}
noInline(storeByVal);

for (var i = 0; i < testLoopCount; ++i) {
    storeById(i);
    shouldBe(loadFromConstantBase(), String(i), "storeById " + i);
    storeByVal(-i);
    shouldBe(loadFromConstantBase(), String(-i), "storeByVal " + i);
}

// The load proves the structure of the base for the store that follows it.
function loadThenStore(object, value) {
    var previous = object.x;
    object.x = value;
    return previous;
}
noInline(loadThenStore);

var base = create();
base.x = 0;
for (var i = 0; i < testLoopCount; ++i) {
    loadThenStore(base, i);
    shouldBe(base.x, String(i), "loadThenStore " + i);
}

// The store adds a property, so it is a structure transition. It needs no watchpoint set.
function addProperty(value) {
    var object = create();
    object.a = 0;
    shouldBe(object.a, "0", "addProperty: a");
    object.x = value;
    return object;
}
noInline(addProperty);

for (var i = 0; i < testLoopCount; ++i)
    shouldBe(addProperty(i).x, String(i), "addProperty " + i);

// A class field is a direct store. It does not call put(). It calls defineOwnProperty() when the class overrides it.
function ReturnsItsArgument(object) {
    return object;
}

var fieldValue = 0;
class DefinesField extends ReturnsItsArgument {
    x = (this.x, fieldValue); // The load proves the structure of this for the definition of x.
}

var fieldBase = create();
fieldBase.x = 0;
for (var i = 0; i < testLoopCount; ++i) {
    fieldValue = i;
    new DefinesField(fieldBase);
    shouldBe(fieldBase.x, String(i), "DefinesField " + i);
}

// Two structures that hold x at two offsets. The FTL can emit a MultiPutByOffset for them. The DFG keeps the
// PutById, and then it must not assume that the store leaves the structure of other objects alone: put() calls
// toString() on the value.
function storeToEither(object, other, value) {
    var a = object.a; // One offset for both structures, so this proves the structure set of object.
    var p = other.p; // Proves the structure of other.
    object.x = value;
    return other.p;
}
noInline(storeToEither);

function loadX(object) {
    return object.x;
}
noInline(loadX);

function replaceP(object) {
    delete object.p;
    object.q = "q";
}
// Fire the transition watchpoint of the structure of { p: 1 }, so that only a structure check proves it.
replaceP({ p: 1 });

var first = create();
first.a = 0;
first.x = 0;
var second = create();
second.a = 0;
second.y = 0;
second.x = 0;

for (var i = 0; i < testLoopCount; ++i) {
    var object = i & 1 ? first : second;
    shouldBe(storeToEither(object, { p: 1 }, i), 1, "storeToEither " + i);
    shouldBe(loadX(object), String(i), "storeToEither: x " + i);
}

var other = { p: 1 };
var calls = 0;
var value = {
    toString() {
        calls++;
        replaceP(other);
        return "string";
    }
};
shouldBe(storeToEither(first, other, value), undefined, "storeToEither: p after put() removed it");
shouldBe(calls, 1, "storeToEither: calls of toString()");
shouldBe(first.x, "string", "storeToEither: x");
