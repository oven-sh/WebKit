// A delete of a property whose attributes are not 0 (non-enumerable, read-only, accessor) has to take the deletion
// transition that an earlier delete of it made, as a delete of an ordinary property does. The lookup asked for the
// transition with the attributes of the property, and the transition is stored without them. So each such delete made
// a Structure of its own.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": bad value: " + actual + " expected: " + expected);
}

function structureID(object) {
    let list = $vm.getStructureTransitionList(object);
    return list[list.length - 5];
}

function define(object, name, descriptor) {
    return Object.defineProperty(object, name, { configurable: true, ...descriptor });
}

function strictArguments() { "use strict"; return arguments; }
function sloppyArguments(a, b) { return arguments; }

let symbol = Symbol("a");

// Objects made the same way have to end on one Structure. A maker with two outcomes takes them in turn.
let makers = {
    nonEnumerable() { let o = define({}, "a", { value: 1, writable: true }); delete o.a; return o; },
    readOnly() { let o = define({}, "a", { value: 1, enumerable: true }); delete o.a; return o; },
    getter() { let o = { get a() { return 1; } }; delete o.a; return o; },
    outOfLine() { let o = define({ p0: 0, p1: 1, p2: 2, p3: 3, p4: 4, p5: 5, p6: 6, p7: 7 }, "a", { value: 1 }); delete o.a; return o; },
    symbolByValue() { let o = define({}, symbol, { value: 1 }); delete o[symbol]; return o; },
    reflect() { let o = define({}, "a", { value: 1 }); Reflect.deleteProperty(o, "a"); return o; },
    attributeChangeThenDelete() { let o = { a: 1 }; define(o, "a", { enumerable: false }); delete o.a; return o; },
    deleteThenAdd() { let o = define({}, "a", { value: 1 }); delete o.a; o.b = 1; return o; },
    deleteThenAddAgain() { let o = define({}, "a", { value: 1 }); delete o.a; o.a = 1; return o; },
    twoDeletes() { let o = define(define({}, "a", { value: 1 }), "b", { value: 2 }); delete o.a; delete o.b; return o; },
    twoDeletionsFromOneSource(i) { let o = define(define({}, "a", { value: 1 }), "b", { value: 2 }); if (i & 1) delete o.a; else delete o.b; return o; },
    additionAndDeletionFromOneSource(i) { let o = define({}, "a", { value: 1 }); if (i & 1) delete o.a; else o.b = 1; return o; },
    errorStack() { let e = new Error("message"); e.stack; delete e.stack; return e; },
    errorStackNeverRead() { let e = new Error("message"); delete e.stack; return e; },
    errorLineAndColumnThenAdd() { let e = new Error("message"); delete e.line; delete e.column; e.code = 1; return e; },
    functionName() { let f = function () { }; delete f.name; return f; },
    strictArgumentsLength() { let a = strictArguments(1, 2); delete a.length; return a; },
    sloppyArgumentsLength() { let a = sloppyArguments(1, 2); delete a.length; return a; },
};

for (let [name, make] of Object.entries(makers)) {
    let objects = [make(0), make(1)];
    for (let i = 2; i < 10; ++i) {
        let object = make(i);
        shouldBe(structureID(object), structureID(objects[i & 1]), name);
        objects.push(object);
    }
}

// An object that was a dictionary keeps its transitions out of the table. The delete still has to work.
{
    let o = define({ x: 1 }, "a", { value: 1 });
    $vm.toCacheableDictionary(o);
    $vm.flattenDictionaryObject(o);
    define(o, "b", { value: 2 });
    shouldBe(delete o.b, true, "was a dictionary");
    shouldBe(delete o.a, true, "was a dictionary");
    shouldBe(JSON.stringify(Object.getOwnPropertyNames(o)), `["x"]`, "was a dictionary");
}

// A collection drops the property tables. The next object takes the kept transitions, and its table is built again
// through the deletion transition.
{
    let make = () => { let o = define({ x: 1 }, "a", { value: 1 }); define(o, "y", { value: 2, enumerable: true }); delete o.a; o.z = 3; return o; };
    let first = make();
    fullGC();
    let second = make();
    shouldBe(structureID(second), structureID(first), "after a collection");
    shouldBe(JSON.stringify(Object.getOwnPropertyNames(second)), `["x","y","z"]`, "names after a collection");
    shouldBe(JSON.stringify(Object.getOwnPropertyDescriptor(second, "y")), `{"value":2,"writable":false,"enumerable":true,"configurable":true}`, "descriptor after a collection");
}
