function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": bad value: " + actual + " expected: " + expected);
}

// A prototype that answers an indexed read inside its own class has no indexed storage to show for
// it: a typed array, an arguments object, a global proxy. An array with a hole over such a prototype
// still has to read the hole from it, on the fast paths as everywhere else.

var otherGlobal = createGlobalObject();
globalThis[1] = "g";
otherGlobal[1] = "h";

var sloppyDirectArguments = new Function("return arguments;");
var sloppyScopedArguments = new Function("a", "b", "c", "(function () { return a + b + c; })(); return arguments;");

// [name, the prototype, what it answers for index 1, whether its structure says MayHaveIndexedAccessors]
var prototypes = [
    ["an ordinary object", () => ({ 1: "o" }), "o", false],
    ["a Uint8Array", () => new Uint8Array([7, 8, 9, 10]), 8, false],
    ["a Float64Array", () => new Float64Array([7.5, 8.5, 9.5, 10.5]), 8.5, false],
    ["a DirectArguments", () => sloppyDirectArguments(7, 8, 9, 10), 8, false],
    ["a ScopedArguments", () => sloppyScopedArguments(7, 8, 9), 8, false],
    ["the global object", () => globalThis, "g", false],
    ["the global object of another realm", () => otherGlobal, "h", false],
    ["an object over the global object", () => Object.create(globalThis), "g", false],
    ["an object over a Uint8Array", () => Object.create(new Uint8Array([7, 8, 9, 10])), 8, false],
    ["a Proxy", () => new Proxy({ 1: "p" }, {}), "p", true],
];

// The allocation profile of an array literal follows what the earlier arrays of that literal became,
// so each array comes from a literal of its own, in a source of its own.
var literals = 0;
function literal(source) {
    return new Function("return " + source + "; // " + literals++)();
}

// Each makes [x, <hole>, y, z] in one indexing shape.
var shapes = [
    ["ArrayWithInt32", () => literal("[0, , 2, 3]")],
    ["ArrayWithDouble", () => literal("[0.5, , 2.5, 3.5]")],
    ["ArrayWithContiguous", () => literal('["a", , "c", "d"]')],
    ["ArrayWithArrayStorage", () => { var array = literal("[0, , 2, 3]"); ensureArrayStorage(array); return array; }],
];

// With --alwaysHaveABadTime every array is an ArrayWithSlowPutArrayStorage from the start.
var hasShapes = !$vm.isHavingABadTime();

function holeyArray(shapeName, makeArray, makePrototype, mayHaveIndexedAccessors) {
    var array = Object.setPrototypeOf(makeArray(), makePrototype());
    // Only a prototype that says MayHaveIndexedAccessors takes the array out of its shape.
    if (hasShapes)
        shouldBe($vm.indexingMode(array), mayHaveIndexedAccessors ? "ArrayWithSlowPutArrayStorage" : shapeName, "the shape of the array");
    return array;
}

// The read itself, once it is compiled. One function for each shape, so that each has its own profile.
for (var [shapeName, makeArray] of shapes) {
    var read = new Function("array", "return array[1];");
    noInline(read);
    for (var [prototypeName, makePrototype, expected, mayHaveIndexedAccessors] of prototypes) {
        var array = holeyArray(shapeName, makeArray, makePrototype, mayHaveIndexedAccessors);
        for (var i = 0; i < testLoopCount; ++i)
            shouldBe(read(array), expected, "array[1] of an " + shapeName + " over " + prototypeName);
    }
}

function ownOrHole(array, index) {
    return Object.hasOwn(array, index) ? array[index] : "<hole>";
}

// Each returns what the operation read for the hole at index 1. None of them asks for a species
// constructor: that one would come from the prototype under test.
var operations = [
    ["at", (a) => Array.prototype.at.call(a, 1)],
    ["join", (a, expected) => Array.prototype.join.call(a, "|").split("|")[1] === String(expected) ? expected : "<empty>"],
    ["toReversed", (a) => Array.prototype.toReversed.call(a)[2]],
    ["with", (a) => Array.prototype.with.call(a, 0, 9)[1]],
    ["reverse", (a) => ownOrHole(Array.prototype.reverse.call(a), 2)],
    ["shift", (a) => (Array.prototype.shift.call(a), ownOrHole(a, 0))],
    ["unshift", (a) => (Array.prototype.unshift.call(a, 7), ownOrHole(a, 2))],
    ["sort", (a) => Object.keys(Array.prototype.sort.call(a, () => 0)).length === 4 ? ownOrHole(a, 1) : "<hole>"],
    ["toSorted", (a) => Array.prototype.toSorted.call(a, () => 0)[1]],
    ["copyWithin", (a) => ownOrHole(Array.prototype.copyWithin.call(a, 0, 1, 2), 0)],
    ["includes", (a, expected) => Array.prototype.includes.call(a, expected) ? expected : "<absent>"],
    ["indexOf", (a, expected) => Array.prototype.indexOf.call(a, expected) === 1 ? expected : "<absent>"],
    ["lastIndexOf", (a, expected) => Array.prototype.lastIndexOf.call(a, expected) === 1 ? expected : "<absent>"],
];

// These make their result with ArraySpeciesCreate, so they are only for a prototype whose
// "constructor" is Object.
var speciesOperations = [
    ["slice", (a) => ownOrHole(Array.prototype.slice.call(a), 1)],
    ["slice from the hole", (a) => ownOrHole(Array.prototype.slice.call(a, 1, 3), 0)],
    ["splice", (a) => ownOrHole(Array.prototype.splice.call(a, 0, 3), 1)],
    ["splice in place", (a) => (Array.prototype.splice.call(a, 0, 1), ownOrHole(a, 0))],
    ["concat", (a) => (a[Symbol.isConcatSpreadable] = true, ownOrHole(Array.prototype.concat.call([], a), 1))],
    ["concat as this", (a) => (a[Symbol.isConcatSpreadable] = true, ownOrHole(Array.prototype.concat.call(a, [5]), 1))],
    ["flat", (a) => Array.prototype.flat.call(a)[1]],
    ["map", (a) => ownOrHole(Array.prototype.map.call(a, (x) => x), 1)],
    ["filter", (a) => Array.prototype.filter.call(a, () => true)[1]],
];

for (var [prototypeName, makePrototype, expected, mayHaveIndexedAccessors] of prototypes) {
    var constructor = makePrototype().constructor;
    var all = constructor === Object || constructor === otherGlobal.Object ? operations.concat(speciesOperations) : operations;
    for (var [shapeName, makeArray] of shapes) {
        for (var [name, operation] of all) {
            var array = holeyArray(shapeName, makeArray, makePrototype, mayHaveIndexedAccessors);
            shouldBe(operation(array, expected), expected, name + " of an " + shapeName + " over " + prototypeName);
        }
    }
}

// A compiled multi-argument unshift keeps the arguments of the call in the DFG's scratch buffer. One
// shape for each of the two operations that do (Int32 and Contiguous share one, Double has its own).
// A typed array in the chain drops a store to an index beyond its length, so this uses the
// prototypes that take the store: an arguments object, the global object, an ordinary object.
for (var [shapeName, makeArray] of [shapes[0], shapes[1]]) {
    var unshiftThree = new Function("array", "return Array.prototype.unshift.call(array, 7, 8, 9);");
    noInline(unshiftThree);
    for (var [prototypeName, makePrototype, expected] of [prototypes[0], prototypes[3], prototypes[5]]) {
        for (var i = 0; i < testLoopCount; ++i) {
            var array = Object.setPrototypeOf(makeArray(), makePrototype());
            shouldBe(unshiftThree(array), 7, "the length after a compiled unshift of three");
            shouldBe(ownOrHole(array, 4), expected, "a compiled unshift of three on an " + shapeName + " over " + prototypeName);
            shouldBe(array[0] + "," + array[1] + "," + array[2], "7,8,9", "the three elements a compiled unshift stored");
        }
    }
}

// An array of an original array structure, once Array.prototype itself is over such an object. In a
// realm of its own, since it ends the sane prototype chain of that realm's arrays.
var insaneRealm = createGlobalObject();
shouldBe(insaneRealm.eval(`
    Object.setPrototypeOf(Array.prototype, new Uint8Array([7, 8, 9, 10]));
    [[0, , 2, 3], [0.5, , 2.5, 3.5], ["a", , "c", "d"]].map((array) => array.join("|").split("|")[1] + " " + array.toReversed()[2] + " " + array.slice(1, 3)[0]).join();
`), "8 8 8,8 8 8,8 8 8", "arrays of a realm whose Array.prototype is over a Uint8Array");
