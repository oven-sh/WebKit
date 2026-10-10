//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1", "--useAOTOperationCounters=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1", "--useAOTOperationCounters=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1", "--useAOTOperationCounters=1", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}

let nextFamily = 1000;
function fresh() {
    const object = {};
    object["only" + nextFamily] = 0;
    object.a = 1;
    object.b = 2;
    object.c = 3;
    return object;
}
function member(what) {
    const object = fresh();
    check(aotSetFamily(object, nextFamily), true, what + ": joins");
    check(aotFamilyOf(object), nextFamily, what + ": is a member");
    check(aotHasDepartedFamily(nextFamily), false, what + ": nobody has left yet");
    return [object, nextFamily++];
}
function stamp(object) { return object; }
class StampsField extends stamp { #field = 1; static has(object) { return #field in object; } }
class StampsBrand extends stamp { #method() { } static has(object) { return #method in object; } }
function addsExtra(object) { object.extra = 1; }
function replaces(object, value) { object.b = value; }
noInline(addsExtra);
noInline(replaces);

const leaves = {
    "delete of a name of the family": [o => delete o.b, "deletion"],
    "delete of an extra": [o => { o.extra = 1; delete o.extra; }, "deletion"],
    "Reflect.deleteProperty": [o => Reflect.deleteProperty(o, "a"), "deletion"],
    "read-only": [o => Object.defineProperty(o, "a", { writable: false }), "attributes"],
    "not enumerable": [o => Object.defineProperty(o, "a", { enumerable: false }), "attributes"],
    "not configurable": [o => Object.defineProperty(o, "a", { configurable: false }), "attributes"],
    "a getter": [o => Object.defineProperty(o, "a", { get() { return 7; } }), "attributes"],
    "__defineGetter__": [o => o.__defineGetter__("b", () => 7), "attributes"],
    "__defineSetter__": [o => o.__defineSetter__("c", () => { }), "attributes"],
    "defineProperties": [o => Object.defineProperties(o, { c: { writable: false } }), "attributes"],
    "an extra turned into a getter": [o => { o.extra = 1; Object.defineProperty(o, "extra", { get() { return 7; } }); }, "attributes"],
    "freeze": [o => Object.freeze(o), "freeze"],
    "seal": [o => Object.seal(o), "seal"],
    "preventExtensions": [o => Object.preventExtensions(o), "prevent-extensions"],
    "Reflect.preventExtensions": [o => Reflect.preventExtensions(o), "prevent-extensions"],
    "many additions": [o => { for (let i = 0; i < 400; ++i) o["p" + i] = i; }, "dictionary"],
    "a cacheable dictionary": [o => $vm.toCacheableDictionary(o), "dictionary"],
    "an uncacheable dictionary": [o => $vm.toUncacheableDictionary(o), "dictionary"],
    "a dictionary that is flattened": [o => { $vm.toCacheableDictionary(o); $vm.flattenDictionaryObject(o); }, "dictionary"],
    "Object.create": [o => Object.create(o), "prototype-of-another"],
    "setPrototypeOf of another": [o => Object.setPrototypeOf({ }, o), "prototype-of-another"],
    "__proto__ in a literal": [o => ({ __proto__: o }), "prototype-of-another"],
    "__proto__ of another assigned": [o => { ({ }).__proto__ = o; }, "prototype-of-another"],
    "prototype of a constructor": [o => { function F() { } F.prototype = o; new F; }, "prototype-of-another"],
    "prototype of new.target": [o => { function F() { } F.prototype = o; Reflect.construct(Object, [], F); }, "prototype-of-another"],
    "setPrototypeOf": [o => Object.setPrototypeOf(o, { }), "change-of-prototype"],
    "__proto__ assigned": [o => { o.__proto__ = null; }, "change-of-prototype"],
    "Reflect.setPrototypeOf": [o => Reflect.setPrototypeOf(o, Array.prototype), "change-of-prototype"],
    "an element": [o => { o[0] = 1; }, "indexed-storage"],
    "a far element": [o => { o[100000000] = 1; }, "indexed-storage"],
    "an element with a getter": [o => Object.defineProperty(o, "0", { get() { return 7; } }), "indexed-storage"],
};
for (const what in leaves) {
    const [road, name] = leaves[what];
    const [object, family] = member(what);
    const before = aotOperationCount("Family::departed:" + name);
    road(object);
    check(aotHasDepartedFamily(family), true, what + ": sets the byte");
    check(aotFamilyOf(object), 0, what + ": is no member any more");
    check(aotOperationCount("Family::departed:" + name) > before, true, what + ": is counted as " + name);
}

const stays = {
    "a replacing store": o => { replaces(o, 5); check(o.b, 5, "stored"); },
    "an addition": o => addsExtra(o),
    "an addition by defineProperty": o => Object.defineProperty(o, "extra", { value: 1, writable: true, enumerable: true, configurable: true }),
    "an added getter": o => Object.defineProperty(o, "extra", { get() { return 7; } }),
    "an added read-only property": o => Object.defineProperty(o, "extra", { value: 1 }),
    "an added symbol": o => { o[Symbol.iterator] = 1; },
    "a private field": o => { new StampsField(o); check(StampsField.has(o), true, "stamped"); },
    "a private method": o => { new StampsBrand(o); check(StampsBrand.has(o), true, "branded"); },
    "defineProperty that changes the value only": o => Object.defineProperty(o, "a", { value: 5 }),
    "defineProperty with the same attributes": o => Object.defineProperty(o, "a", { value: 5, writable: true, enumerable: true, configurable: true }),
    "Object.assign": o => Object.assign(o, { a: 9, extra: 1 }),
    "being spread": o => ({ ...o }),
    "being enumerated": o => { for (const name in o) o[name]; Object.keys(o); Object.entries(o); JSON.stringify(o); },
    "being described": o => { Object.getOwnPropertyDescriptors(o); Object.hasOwn(o, "a"); "a" in o; o.hasOwnProperty("zz"); },
    "being the target of a proxy": o => { const p = new Proxy(o, { }); p.a; p.b = 4; },
    "collections": o => { edenGC(); fullGC(); },
    "a failed delete of nothing": o => delete o.nothing,
};
for (const what in stays) {
    const [object, family] = member(what);
    stays[what](object);
    check(aotHasDepartedFamily(family), false, what + ": leaves the byte alone");
    check(aotFamilyOf(object), family, what + ": is still a member");
}

{
    const members = [];
    const family = nextFamily;
    for (let i = 0; i < 200; ++i)
        members.push(fresh());
    check(aotSetFamily(members[0], family), true, "two hundred of one structure: joins");
    ++nextFamily;
    for (const object of members) {
        check(aotFamilyOf(object), family, "two hundred of one structure: all are members");
        addsExtra(object);
        replaces(object, 7);
        check(aotFamilyOf(object), family, "an addition cached in machine code hands the number on");
    }
    check(aotHasDepartedFamily(family), false, "additions in machine code leave the byte alone");
    const before = aotOperationCount("Family::departed:deletion");
    for (const object of members)
        delete object.a;
    check(aotOperationCount("Family::departed:deletion") - before, members.length, "every leaver passes the hook, also those that take a cached transition");
    check(aotHasDepartedFamily(family), true, "two hundred of one structure: the byte is set");
}

{
    const members = [];
    const family = nextFamily;
    for (let i = 0; i < 200; ++i)
        members.push(fresh());
    check(aotSetFamily(members[0], family), true, "brands: joins");
    ++nextFamily;
    for (const object of members) {
        new StampsBrand(object);
        check(aotFamilyOf(object), family, "a brand, cached in machine code or not, hands the number on");
    }
    check(aotHasDepartedFamily(family), false, "brands leave the byte alone");
    delete members[members.length - 1].a;
    check(aotHasDepartedFamily(family), true, "a branded member that leaves sets the byte");
}

const refused = {
    "an array": () => [1, 2],
    "a function": () => function () { },
    "a dictionary": () => { const o = fresh(); $vm.toCacheableDictionary(o); return o; },
    "a prototype": () => { const o = fresh(); Object.create(o); return o; },
    "an object with a getter": () => { const o = fresh(); Object.defineProperty(o, "a", { get() { return 1; } }); return o; },
    "a frozen object": () => Object.freeze(fresh()),
    "a structure that something has left already": () => { const o = fresh(), other = fresh(); other.extra = 1; return o; },
    "a structure that something has left by a delete": () => { const o = fresh(), other = fresh(); delete other.a; return o; },
    "properties out of line": () => { const o = fresh(); for (let i = 0; i < 20; ++i) o["q" + i] = i; return o; },
};
for (const what in refused) {
    const object = refused[what]();
    check(aotSetFamily(object, nextFamily), false, what + ": is refused");
    check(aotFamilyOf(object), 0, what + ": has no number");
    ++nextFamily;
}

{
    const [object, family] = member("another family");
    check(aotSetFamily(object, family), true, "the same number again is fine");
    check(aotSetFamily(object, family + 1), false, "another number is refused");
    check(aotFamilyOf(object), family, "the number stays");
}
