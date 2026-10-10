//@ runDefault("--useDollarVM=1", "--compileMainScriptAheadOfTime=1")
//@ runDefault("--useDollarVM=1", "--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("--useDollarVM=1", "--compileMainScriptAheadOfTime=1", "--verifyGC=1", "--scribbleFreeCells=1", "--useZombieMode=1", "--slowPathAllocsBetweenGCs=50")
//@ runDefault("--useDollarVM=1")
//@ runDefault("--useDollarVM=1", "--useJIT=0")
//@ runDefault("--useDollarVM=1", "--useJIT=0", "--validateExceptionChecks=1")
//@ runDefault("--useDollarVM=1", "--compileMainScriptAheadOfTime=1", "--validateExceptionChecks=1")

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}

const message = "Attempting to change the shape of an object whose shape is immutable.";
const structureOf = object => /Structure (0x[0-9a-f]+)/.exec(describe(object))[1];
const ordinaryCopy = (source, prototype = Object.prototype) => Object.defineProperties(Object.create(prototype), Object.getOwnPropertyDescriptors(source));
const levels = [
    ["ordinary", ordinaryCopy],
    ["stable fields", objectWithStableFields],
    ["immutable shape", objectWithImmutableShape],
];
const otherPrototype = { inherited: true };
const prototypes = [Object.prototype, otherPrototype, null];
const symbol = Symbol("added");
const getter = function () { return "got"; };
const setter = function () { };

function stateOf(object) {
    const keys = Reflect.ownKeys(object);
    return JSON.stringify([
        structureOf(object),
        keys.map(String),
        keys.map(key => { const d = Object.getOwnPropertyDescriptor(object, key); return [typeof d.get, typeof d.set, String(d.value), d.writable, d.enumerable, d.configurable]; }),
        Object.isExtensible(object),
        prototypes.indexOf(Object.getPrototypeOf(object)),
    ]);
}

class Returns { constructor(object) { return object; } }
class StampsPrivateField extends Returns { #field = 1; }
class StampsBrand extends Returns { #method() { } }
class StampsPublicField extends Returns { added = 1; }

function addSloppy(object) { object.added = 1; }
function addStrict(object) { "use strict"; object.added = 1; }
function addByKeySloppy(object, key) { object[key] = 1; }
function addByKeyStrict(object, key) { "use strict"; object[key] = 1; }
function addRun(object) { "use strict"; object.first = 1; object.second = 2; object.third = 3; }
function deleteSloppy(object) { return delete object.a; }
function deleteStrict(object) { "use strict"; return delete object.a; }
function deleteByKeySloppy(object, key) { return delete object[key]; }
function deleteByKeyStrict(object, key) { "use strict"; return delete object[key]; }
function setProtoSloppy(object, prototype) { object.__proto__ = prototype; }
function setProtoStrict(object, prototype) { "use strict"; object.__proto__ = prototype; }

const roads = [
    ["a new name, sloppy", 2, addSloppy],
    ["a new name, strict", 2, addStrict],
    ["a new name by key, sloppy", 2, object => addByKeySloppy(object, "added")],
    ["a new name by key, strict", 2, object => addByKeyStrict(object, "added")],
    ["a new symbol, sloppy", 2, object => addByKeySloppy(object, symbol)],
    ["a new symbol, strict", 2, object => addByKeyStrict(object, symbol)],
    ["a run of new names", 2, addRun],
    ["Object.defineProperty of a new name", 2, object => Object.defineProperty(object, "added", { value: 1, writable: true, enumerable: true, configurable: true })],
    ["Object.defineProperty of a new accessor", 2, object => Object.defineProperty(object, "added", { get: getter, configurable: true })],
    ["Object.defineProperties of a new name", 2, object => Object.defineProperties(object, { added: { value: 1 } })],
    ["Reflect.defineProperty of a new name", 2, object => Reflect.defineProperty(object, "added", { value: 1 })],
    ["Reflect.set of a new name", 2, object => Reflect.set(object, "added", 1)],
    ["Reflect.set with the object as the receiver", 2, object => Reflect.set({}, "added", 1, object)],
    ["Object.assign of a new name", 2, object => Object.assign(object, { added: 1 })],
    ["Object.assign of two sources", 2, object => Object.assign(object, { a: 1 }, { added: 1 })],
    ["Object.assign of three sources", 2, object => Object.assign(object, { a: 1 }, { b: "two" }, { added: 1 })],
    ["__defineGetter__ of a new name", 2, object => object.__defineGetter__("added", getter)],
    ["__defineSetter__ of a new name", 2, object => object.__defineSetter__("added", setter)],
    ["Error.captureStackTrace", 2, object => Error.captureStackTrace(object)],
    ["a private field", 2, object => new StampsPrivateField(object)],
    ["a brand", 2, object => new StampsBrand(object)],
    ["a public field", 2, object => new StampsPublicField(object)],

    ["delete, sloppy", 1, deleteSloppy],
    ["delete, strict", 1, deleteStrict],
    ["delete by key, sloppy", 1, object => deleteByKeySloppy(object, "b")],
    ["delete by key, strict", 1, object => deleteByKeyStrict(object, "c")],
    ["Reflect.deleteProperty", 1, object => Reflect.deleteProperty(object, "a")],

    ["read-only", 1, object => Object.defineProperty(object, "a", { writable: false })],
    ["read-only with a value", 1, object => Object.defineProperty(object, "a", { value: 9, writable: false })],
    ["read-only by Reflect.defineProperty", 1, object => Reflect.defineProperty(object, "b", { writable: false })],
    ["read-only by Object.defineProperties", 1, object => Object.defineProperties(object, { c: { writable: false } })],
    ["an accessor", 1, object => Object.defineProperty(object, "a", { get: getter })],
    ["an accessor by __defineGetter__", 1, object => object.__defineGetter__("b", getter)],
    ["an accessor by __defineSetter__", 1, object => object.__defineSetter__("c", setter)],
    ["Object.freeze", 1, object => Object.freeze(object)],
    ["not enumerable", 2, object => Object.defineProperty(object, "a", { enumerable: false })],
    ["not configurable", 2, object => Object.defineProperty(object, "a", { configurable: false })],
    ["Object.seal", 2, object => Object.seal(object)],
    ["Object.preventExtensions", 2, object => Object.preventExtensions(object)],
    ["Reflect.preventExtensions", 2, object => Reflect.preventExtensions(object)],

    ["Object.setPrototypeOf", 2, object => Object.setPrototypeOf(object, otherPrototype)],
    ["Object.setPrototypeOf of null", 2, object => Object.setPrototypeOf(object, null)],
    ["Reflect.setPrototypeOf", 2, object => Reflect.setPrototypeOf(object, otherPrototype)],
    ["__proto__, sloppy", 2, object => setProtoSloppy(object, otherPrototype)],
    ["__proto__, strict", 2, object => setProtoStrict(object, otherPrototype)],

    ["an index, sloppy", 2, object => addByKeySloppy(object, 0)],
    ["an index, strict", 2, object => addByKeyStrict(object, 0)],
    ["a far index, sloppy", 2, object => addByKeySloppy(object, 1000000000)],
    ["a far index, strict", 2, object => addByKeyStrict(object, 1000000000)],
    ["Object.defineProperty of an index", 2, object => Object.defineProperty(object, 0, { value: 1, writable: true, enumerable: true, configurable: true })],
    ["Object.defineProperty of a read-only index", 2, object => Object.defineProperty(object, 0, { value: 1 })],
    ["Object.defineProperty of an accessor at an index", 2, object => Object.defineProperty(object, 0, { get: getter })],
    ["Reflect.set of an index", 2, object => Reflect.set(object, 0, 1)],
    ["Object.assign of an array", 2, object => Object.assign(object, [1])],
    ["Array.prototype.push", 2, object => Array.prototype.push.call(object, 1)],
];

const harmless = [
    ["a store", object => { object.a = 5; check(object.a, 5, "the stored value"); }],
    ["a store by key", object => { addByKeyStrict(object, "b"); check(object.b, 1, "the stored value"); }],
    ["Object.defineProperty of a value", object => { Object.defineProperty(object, "a", { value: 7 }); check(object.a, 7, "the defined value"); }],
    ["Object.defineProperty of what is there", object => Object.defineProperty(object, "a", { value: 1, writable: true, enumerable: true, configurable: true })],
    ["Reflect.set", object => check(Reflect.set(object, "a", 8), true, "the answer of Reflect.set")],
    ["Object.assign of its own names", object => { Object.assign(object, { a: 2, c: 3 }); check(object.a + object.c, 5, "the assigned values"); }],
    ["delete of a name that is not there, sloppy", object => check(deleteByKeySloppy(object, "absent"), true, "the answer of delete")],
    ["delete of a name that is not there, strict", object => check(deleteByKeyStrict(object, "absent"), true, "the answer of delete")],
    ["delete of an index that is not there", object => check(deleteByKeyStrict(object, 0), true, "the answer of delete")],
    ["Reflect.deleteProperty of a name that is not there", object => check(Reflect.deleteProperty(object, "absent"), true, "the answer of Reflect.deleteProperty")],
    ["the prototype it has", object => { Object.setPrototypeOf(object, Object.prototype); check(Reflect.setPrototypeOf(object, Object.prototype), true, "the answer of Reflect.setPrototypeOf"); setProtoStrict(object, Object.prototype); }],
    ["the prototype of another by Object.create", object => { const heir = Object.create(object); check(heir.a, object.a, "the inherited value"); heir.added = 1; check("added" in object, false, "the name added to the heir"); }],
    ["the prototype of another by Object.setPrototypeOf", object => check(Object.setPrototypeOf({}, object).b, "two", "the inherited value")],
    ["the prototype of a constructor", object => { function Made() { } Made.prototype = object; check(new Made().b, "two", "the inherited value"); }],
    ["reading", object => { check(JSON.stringify(object), JSON.stringify({ a: 1, b: "two", c: null }), "JSON"); check(Object.keys(object).join(), "a,b,c", "keys"); let names = ""; for (const name in object) names += name; check(names, "abc", "for-in"); }],
    ["a key of a WeakMap", object => check(new WeakMap([[object, 1]]).get(object), 1, "the value in the WeakMap")],
];

const repetitions = 40;
for (let level = 0; level < levels.length; ++level) {
    const [levelName, make] = levels[level];
    for (const [name, refusedFrom, road] of roads) {
        for (let i = 0; i < repetitions; ++i) {
            const what = name + " of an object with " + levelName;
            const object = make({ a: 1, b: "two", c: null });
            const isChecked = !i || i == repetitions - 1;
            const before = isChecked ? stateOf(object) : null;
            let thrown = null;
            try {
                road(object);
            } catch (error) {
                thrown = error;
            }
            if (level < refusedFrom) {
                check(thrown, null, what + " throws");
                if (isChecked)
                    check(stateOf(object) !== before, true, what + " changes it");
                check(hasStableFields(object), level >= 1, what + " keeps the bit");
                if (level) {
                    for (const [field, value] of [["a", 1], ["b", "two"], ["c", null]]) {
                        const d = Object.getOwnPropertyDescriptor(object, field);
                        check(d.value, value, what + ": the value of " + field);
                        check(d.writable, true, what + ": " + field + " is writable");
                    }
                }
                continue;
            }
            check(thrown instanceof TypeError, true, what + " throws a TypeError, not " + thrown);
            check(thrown.message, message, what + ": the message");
            if (isChecked)
                check(stateOf(object), before, what + " leaves it as it was");
            check(hasStableFields(object), true, what + " keeps the first bit");
            check(hasImmutableShape(object), level == 2, what + " keeps the second bit");
        }
    }
    for (const [name, road] of harmless) {
        for (let i = 0; i < repetitions; ++i) {
            const what = name + " of an object with " + levelName;
            const object = make({ a: 1, b: "two", c: null });
            const before = structureOf(object);
            road(object);
            if (level == 2)
                check(structureOf(object), before, what + ": the Structure");
            check(hasStableFields(object), level >= 1, what + " keeps the first bit");
            check(hasImmutableShape(object), level == 2, what + " keeps the second bit");
        }
    }
}

for (let level = 1; level < levels.length; ++level) {
    const [levelName, make] = levels[level];
    for (let i = 0; i < repetitions; ++i) {
        for (const source of [{ a: 1 }, { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6 }, { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8 }]) {
            const object = make(source);
            const { ...rest } = object;
            for (const [how, copy] of [["Object.assign", Object.assign({}, object)], ["a spread", { ...object }], ["a rest pattern", rest], ["Object.assign of two", Object.assign({}, object, object)]]) {
                const what = "a copy by " + how + " of an object with " + levelName;
                check(hasStableFields(copy), false, what + " has the first bit");
                check(hasImmutableShape(copy), false, what + " has the second bit");
                check(JSON.stringify(copy), JSON.stringify(source), what);
                check(delete copy.a, true, what + ": delete");
                copy.added = 1;
                Object.freeze(copy);
                check(Object.isFrozen(copy), true, what + ": freeze");
            }
            check(JSON.stringify(object), JSON.stringify(source), "the source");
        }
    }
}

for (let i = 0; i < repetitions; ++i) {
    const object = objectWithImmutableShape({ a: 1, b: 2, c: 3 });
    let thrown = null;
    try {
        Object.assign(object, { a: 10, added: 1, c: 30 });
    } catch (error) {
        thrown = error;
    }
    check(thrown instanceof TypeError && thrown.message, message, "Object.assign stops");
    check(JSON.stringify(object), JSON.stringify({ a: 10, b: 2, c: 3 }), "Object.assign stops at the first name that is refused");
}

for (let i = 0; i < repetitions; ++i) {
    const object = objectWithStableFields({ a: 1, b: 2 });
    object[0] = "element";
    object.tail = 3;
    let thrown = null;
    try {
        Object.freeze(object);
    } catch (error) {
        thrown = error;
    }
    check(thrown instanceof TypeError && thrown.message, message, "Object.freeze of an object with stable fields and an element");
    object.a = 4;
    check(object.a + object.b, 6, "the fields after the refused freeze");
    check(hasStableFields(object), true, "the bit after the refused freeze");
}

if (aotSetFamily({ familiesAreThere: 1 }, 2000) === true) {
    let family = 1;
    const noDeparture = [
        ["a new name", false, object => { object.added = 1; }],
        ["a brand", false, object => new StampsBrand(object)],
        ["a private field", false, object => new StampsPrivateField(object)],
        ["a long tail", true, object => { for (let i = 0; i < 200; ++i) object["tail" + i] = i; }],
        ["delete of a tail name", true, object => { object.added = 1; object.more = 2; delete object.added; }],
        ["not enumerable", true, object => Object.defineProperty(object, "a", { enumerable: false })],
        ["not configurable", true, object => Object.defineProperty(object, "a", { configurable: false })],
        ["Object.seal", true, object => Object.seal(object)],
        ["Object.preventExtensions", true, object => Object.preventExtensions(object)],
        ["an index", true, object => { object[0] = 1; }],
        ["a far index", true, object => { object[1000000000] = 1; }],
        ["the prototype of another", true, object => Object.create(object)],
        ["a new prototype", true, object => Object.setPrototypeOf(object, otherPrototype)],
        ["a dictionary", true, object => $vm.toCacheableDictionary(object)],
        ["an uncacheable dictionary", true, object => $vm.toUncacheableDictionary(object)],
    ];
    for (const [name, isDepartureOfOrdinary, road] of noDeparture) {
        const stable = objectWithStableFields({ a: 1, b: 2 });
        const stableFamily = family++;
        check(aotSetFamily(stable, stableFamily), true, "a family is given to an object with stable fields");
        road(stable);
        check(aotFamilyOf(stable), stableFamily, name + ": the family of an object with stable fields");
        check(aotHasDepartedFamily(stableFamily) || false, false, name + ": the family with stable fields has departed");
        check(hasStableFields(stable), true, name + ": the bit");
        check(stable.a + stable.b, 3, name + ": the fields");

        const ordinaryFamily = family++;
        const ordinary = {};
        ordinary.a = 1;
        ordinary["b" + ordinaryFamily] = 2;
        check(aotSetFamily(ordinary, ordinaryFamily), true, "a family is given to an ordinary object");
        road(ordinary);
        check(aotFamilyOf(ordinary), isDepartureOfOrdinary ? 0 : ordinaryFamily, name + ": the family of an ordinary object");
        const hasDeparted = aotHasDepartedFamily(ordinaryFamily);
        if (hasDeparted !== null)
            check(hasDeparted, isDepartureOfOrdinary, name + ": the family of an ordinary object has departed");
    }
    for (const [name, refusedFrom, road] of roads) {
        if (refusedFrom != 1)
            continue;
        const stable = objectWithStableFields({ a: 1, b: "two", c: null });
        const stableFamily = family++;
        check(aotSetFamily(stable, stableFamily), true, "a family is given to an object with stable fields");
        let thrown = null;
        try {
            road(stable);
        } catch (error) {
            thrown = error;
        }
        check(thrown instanceof TypeError && thrown.message, message, name + " of a member of a family");
        check(aotFamilyOf(stable), stableFamily, name + ": the family after the refusal");
        check(aotHasDepartedFamily(stableFamily) || false, false, name + ": the family has departed by a refused road");
    }
}

for (let i = 0; i < repetitions; ++i) {
    let received = null;
    const withSetter = Object.defineProperty({}, 0, { set(value) { received = value; }, configurable: true });
    const object = objectWithImmutableShape({ a: 1 }, withSetter);
    const before = structureOf(object);
    addByKeyStrict(object, 0);
    check(received, 1, "the value given to the setter at an index of the prototype");
    received = null;
    addByKeySloppy(object, 0);
    check(received, 1, "the value given to the setter at an index of the prototype");
    check(structureOf(object), before, "the Structure after the setter at an index of the prototype");

    const heir = objectWithImmutableShape({ a: 1 }, Object.defineProperty({}, 0, { value: "fixed" }));
    addByKeySloppy(heir, 0);
    let thrown = null;
    try {
        addByKeyStrict(heir, 0);
    } catch (error) {
        thrown = error;
    }
    check(thrown instanceof TypeError && thrown.message !== message, true, "a store over a read-only index of the prototype is refused by the language");
    check(heir[0], "fixed", "the read-only index of the prototype");
    thrown = null;
    try {
        addByKeySloppy(heir, 1);
    } catch (error) {
        thrown = error;
    }
    check(thrown instanceof TypeError && thrown.message, message, "another index");
}
