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
function throwsTypeError(f, what) {
    try {
        f();
    } catch (error) {
        check(error instanceof TypeError, true, what + " throws a TypeError, not " + error);
        return;
    }
    throw new Error(what + " did not throw");
}
const structureOf = object => /Structure (0x[0-9a-f]+)/.exec(describe(object))[1];
const ordinaryCopy = (source, prototype = Object.prototype) => Object.defineProperties(Object.create(prototype), Object.getOwnPropertyDescriptors(source));
const levels = [
    ["ordinary", ordinaryCopy, false, false],
    ["stable fields", objectWithStableFields, true, false],
    ["immutable shape", objectWithImmutableShape, true, true],
];

function reflectionOf(object) {
    const inOrder = [];
    for (const name in object)
        inOrder.push(name);
    const names = Object.getOwnPropertyNames(object);
    return JSON.stringify({
        keys: Object.keys(object),
        names,
        symbols: Object.getOwnPropertySymbols(object).length,
        attributes: names.map(name => { const d = Object.getOwnPropertyDescriptor(object, name); return [name, "value" in d, d.writable, d.enumerable, d.configurable]; }),
        isExtensible: Object.isExtensible(object),
        reflectIsExtensible: Reflect.isExtensible(object),
        isSealed: Object.isSealed(object),
        isFrozen: Object.isFrozen(object),
        json: JSON.stringify(object),
        spread: { ...object },
        entries: Object.entries(object),
        inOrder,
        has: names.map(name => name in object && Object.hasOwn(object, name) && Object.prototype.propertyIsEnumerable.call(object, name)),
        absent: "notThere" in object,
        tag: Object.prototype.toString.call(object),
        type: typeof object,
        isArray: Array.isArray(object),
    });
}

const inner = { a: 1 };
const list = [1, 2];
const many = count => Object.fromEntries(Array.from({ length: count }, (_, i) => ["n" + i, i]));
const prototypeWithMethod = { sum() { return this.x + this.y; }, inherited: "from the prototype" };
const sources = [
    ["no field", { }],
    ["one field", { x: 1 }],
    ["values of every kind", { x: 1, y: 2.5, z: "text", w: null, v: undefined, u: inner, t: list, s: true, r: -0, q: NaN, p: 2 ** 40 }],
    ["names in an order that is not alphabetical", { zebra: 1, apple: 2, mango: 3 }],
    ["names that the engine knows", { constructor: 1, toString: 2, valueOf: 3, length: 4, name: 5, prototype: 6 }],
    ["the most fields there can be", many(62)],
];

for (const [what, source] of sources) {
    for (const prototype of [Object.prototype, null, prototypeWithMethod]) {
        const expected = reflectionOf(ordinaryCopy(source, prototype));
        for (const [level, make, isStable, isImmutable] of levels) {
            const object = make(source, prototype);
            const where = what + ", " + level + ", prototype " + (prototype === null ? "null" : prototype === Object.prototype ? "Object.prototype" : "its own");
            check(hasStableFields(object), isStable, where + ": hasStableFields");
            check(hasImmutableShape(object), isImmutable, where + ": hasImmutableShape");
            check(Object.getPrototypeOf(object), prototype, where + ": the prototype");
            check(reflectionOf(object), expected, where + ": reflection answers as for an ordinary object");
            for (const name of Object.keys(source))
                check(object[name], source[name], where + ": the value of " + name);
        }
    }
}
check(Object.getPrototypeOf(objectWithStableFields({ x: 1 })) === Object.prototype && Object.getPrototypeOf(objectWithImmutableShape({ x: 1 })) === Object.prototype, true, "the prototype is Object.prototype when none is given");
check(hasStableFields(1) || hasImmutableShape("text") || hasStableFields(null) || hasImmutableShape(undefined), false, "what is no object has neither");

for (const [level, make] of levels.slice(1)) {
    throwsTypeError(() => make(many(63)), level + ": one field more than there can be");
    throwsTypeError(() => make({ x: 1, 0: 2 }), level + ": an index as a name");
    throwsTypeError(() => make(1), level + ": no object");
    throwsTypeError(() => make({ }, 1), level + ": a prototype that is neither an object nor null");
    const skipped = make(Object.defineProperty({ x: 1, [Symbol.iterator]: 2 }, "hidden", { value: 3, enumerable: false }));
    check(Reflect.ownKeys(skipped).join(), "x", level + ": symbols and what is not enumerable are not copied");
}

function storesAndReads(object, i) {
    object.x = i;
    object.y = i + 0.5;
    object.z = inner;
    return object.x + object.y;
}
noInline(storesAndReads);
for (const [level, make] of levels.slice(1)) {
    const object = make({ x: 0, y: 0, z: null }, prototypeWithMethod);
    const structure = structureOf(object);
    for (let i = 0; i < 20000; ++i)
        check(storesAndReads(object, i), 2 * i + 0.5, level + ": stores and reads");
    check(object.sum(), 2 * 19999 + 0.5, level + ": a method of the prototype");
    check(object.inherited, "from the prototype", level + ": a property of the prototype");
    check(structureOf(object), structure, level + ": stores to fields do not change the Structure");
    check(object.z, inner, level + ": a cell in a field");
}

{
    const kept = [];
    for (let i = 0; i < 2000; ++i)
        kept.push((i & 1 ? objectWithStableFields : objectWithImmutableShape)({ index: i, text: "t" + i, child: { i }, half: i / 2 }));
    fullGC();
    for (let i = 0; i < kept.length; ++i)
        check(kept[i].index + kept[i].text + kept[i].child.i + kept[i].half, i + "t" + i + i + i / 2, "after a collection");
}

{
    let resolved;
    const thenable = objectWithImmutableShape({ then(resolve) { resolve("by its own then"); } });
    Promise.resolve(thenable).then(value => { resolved = value; });
    drainMicrotasks();
    check(resolved, "by its own then", "a field named then makes a thenable");
    let plain;
    const notThenable = objectWithImmutableShape({ x: 1 });
    Promise.resolve(notThenable).then(value => { plain = value; });
    drainMicrotasks();
    check(plain, notThenable, "without one it is none");
}

{
    const object = objectWithImmutableShape({ x: 1, y: 2 });
    const structure = structureOf(object);
    const heir = Object.create(object);
    check(heir.x + heir.y, 3, "an heir reads the fields");
    function Made() { }
    Made.prototype = object;
    check(new Made().y, 2, "and so does an instance of a function with it as prototype");
    check(structureOf(object), structure, "an immutable shape is born able to be a prototype: no transition");
    check(hasImmutableShape(object), true, "and stays what it is");
    check(hasStableFields(heir) || hasImmutableShape(heir), false, "an heir has neither");
}

for (const [level, make] of levels.slice(1)) {
    const object = make({ x: 1, y: 2.5, z: inner });
    const { x, ...rest } = object;
    const copies = [
        ["a spread", { ...object }],
        ["a spread behind another property", { first: 0, ...object }],
        ["a rest", rest],
        ["Object.assign into an empty object", Object.assign({ }, object)],
        ["a round trip through JSON", JSON.parse(JSON.stringify(object))],
        ["Object.fromEntries of its entries", Object.fromEntries(Object.entries(object))],
    ];
    for (const [what, copy] of copies) {
        check(hasStableFields(copy) || hasImmutableShape(copy), false, level + ": " + what + " is an ordinary object");
        copy.extra = 1;
        delete copy.y;
        Object.freeze(copy);
        check(Object.isFrozen(copy) && copy.extra === 1 && !("y" in copy), true, level + ": " + what + " can be extended, lose a property and be frozen");
    }
    check(hasStableFields(object) && object.x === 1 && object.y === 2.5 && object.z === inner, true, level + ": and the original is what it was");
}

function markedInPlace(source, prototype = { }) {
    const object = Object.create(prototype);
    for (const name of Object.keys(source))
        object[name] = source[name];
    check(makeFieldsStable(object), true, "the fields of an ordinary object are made stable in place");
    return object;
}
{
    const prototype = { };
    const bear = () => { const object = Object.create(prototype); object.x = 1; object.y = 2; return object; };
    const first = bear();
    const second = bear();
    check(structureOf(first), structureOf(second), "two objects born the same way have one Structure");
    check(hasStableFields(first) || hasStableFields(second), false, "neither has stable fields yet");
    const structure = structureOf(first);
    check(makeFieldsStable(first), true, "the fields of one are made stable");
    check(structureOf(first), structure, "in place: no transition");
    check(hasStableFields(first) && hasStableFields(second) && hasStableFields(bear()), true, "so those of all that have, or get, that Structure are");
    check(hasImmutableShape(first), false, "the shape is not immutable");
    check(makeFieldsStable(first), false, "not twice");
    check(hasStableFields(Object.create(prototype)), false, "an object on the way to that Structure has none");
    const dictionary = { };
    for (let i = 0; i < 300; ++i)
        dictionary["n" + i] = i;
    check(makeFieldsStable(dictionary), false, "not of a dictionary, nor with properties out of line");
    check(makeFieldsStable([1]) || makeFieldsStable(function () { }) || makeFieldsStable(1) || makeFieldsStable(null) || makeFieldsStable(), false, "only of a plain object");
}

class Returns { constructor(object) { return object; } }
class StampsField extends Returns { #stamp = 1; static has(object) { return #stamp in object; } }
class StampsBrand extends Returns { #method() { } static has(object) { return #method in object; } }
const otherPrototype = { other: true };
const harmlessRoads = [
    ["a name is added", o => { o.tail = 1; }],
    ["a name is added by defineProperty", o => { Object.defineProperty(o, "tail", { value: 1, writable: true, enumerable: true, configurable: true }); }],
    ["an accessor is added behind", o => { Object.defineProperty(o, "tail", { get() { return 7; }, enumerable: true, configurable: true }); }],
    ["a name is added and deleted", o => { o.tail = 1; delete o.tail; }],
    ["a name is added and made read-only", o => { o.tail = 1; Object.defineProperty(o, "tail", { writable: false }); }],
    ["Object.assign adds names", o => { Object.assign(o, { tail: 1, more: 2 }); }],
    ["a long tail", o => { for (let i = 0; i < 300; ++i) o["tail" + i] = i; }],
    ["a long tail, nearly all of it deleted, flattened", o => { for (let i = 0; i < 300; ++i) o["tail" + i] = i; for (let i = 0; i < 299; ++i) delete o["tail" + i]; $vm.flattenDictionaryObject(o); }],
    ["Object.seal", o => { Object.seal(o); }],
    ["Object.preventExtensions", o => { Object.preventExtensions(o); }],
    ["Reflect.preventExtensions", o => { check(Reflect.preventExtensions(o), true, "Reflect.preventExtensions answers"); }],
    ["an index", o => { o[0] = "zero"; o[5] = "five"; }],
    ["a large index", o => { o[100000] = "far"; }],
    ["it becomes a prototype", o => { Object.create(o); }],
    ["a new prototype", o => { Object.setPrototypeOf(o, otherPrototype); }],
    ["a new prototype by __proto__", o => { o.__proto__ = otherPrototype; }],
    ["a private field", o => { new StampsField(o); check(StampsField.has(o), true, "the field is there"); }],
    ["a private brand", o => { new StampsBrand(o); check(StampsBrand.has(o), true, "the brand is there"); }],
    ["a symbol", o => { o[Symbol.for("tail")] = 1; }],
    ["a field is defined again as it is", o => { Object.defineProperty(o, "x", { value: 10, writable: true, enumerable: true, configurable: true }); o.x = 1; }],
];
for (const [road, go, how, make] of harmlessRoads.flatMap(([what, go]) => [[what + ", born so", go, "born", objectWithStableFields], [what + ", made so in place, with room to spare in the object", go, "in place", markedInPlace]])) {
    const source = { x: 1, y: 2.5, z: inner };
    const ordinary = ordinaryCopy(source, how === "born" ? Object.prototype : { });
    const stable = make(source);
    go(ordinary);
    go(stable);
    check(reflectionOf(stable), reflectionOf(ordinary), road + ": as an ordinary object that went the same road");
    check(Object.getPrototypeOf(stable) === otherPrototype, Object.getPrototypeOf(ordinary) === otherPrototype, road + ": the prototype");
    check(hasStableFields(stable), true, road + ": the fields are still stable");
    check(hasImmutableShape(stable), false, road + ": the shape never was immutable");
    check(stable.x === 1 && stable.y === 2.5 && stable.z === inner, true, road + ": the born fields hold their values");
    if (!Object.isFrozen(stable)) {
        check(storesAndReads(stable, 4), 8.5, road + ": and are stored to and read as before");
        check(stable.x === 4 && stable.y === 4.5 && stable.z === inner, true, road + ": with the values stored");
    }
}
