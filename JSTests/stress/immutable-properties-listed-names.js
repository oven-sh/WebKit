// The form of the lock with a list of names. A listed name is fixed whether the property is present or absent, and every other name
// stays as it was: the object is still extensible. The list can take in every numeric key (array indices and the other canonical
// numeric strings) and the prototype. Objects that inherit from such an object assign as before.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function outcome(func) {
    try {
        return String(func());
    } catch (e) {
        return e.constructor.name;
    }
}

function strictStore(object, key, value) { "use strict"; object[key] = value; return "stored"; }
function sloppyStore(object, key, value) { object[key] = value; return "stored"; }
function strictStoreKept(object, value) { "use strict"; object.kept = value; return "stored"; }
function strictStoreFree(object, value) { "use strict"; object.free = value; return "stored"; }
function strictStoreAbsent(object, value) { "use strict"; object.absent = value; return "stored"; }
function strictDelete(object, key) { "use strict"; return delete object[key]; }
function sloppyDelete(object, key) { return delete object[key]; }
function read(object, key) { return object[key]; }

function make() {
    let object = { kept: 1, free: 2 };
    Object.defineProperty(object, "getter", { get() { return 3; }, set(value) { throw new Error("the setter ran"); }, configurable: true });
    return object;
}

let symbol = Symbol("listed"), otherSymbol = Symbol("other");

// 1. Names, present and absent.
{
    let object = $vm.makePropertiesImmutable(make(), ["kept", "getter", "absent", symbol]);
    shouldBe($vm.hasImmutableProperties(object), false);
    shouldBe(Object.isExtensible(object), true);
    shouldBe(Object.getOwnPropertyDescriptor(object, "kept").writable, true);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(outcome(() => strictStoreKept(object, i + 2)), "TypeError");
        shouldBe(outcome(() => strictStoreAbsent(object, i)), "TypeError");
        shouldBe(outcome(() => strictStoreFree(object, i)), "stored");
        shouldBe(object.kept, 1);
        shouldBe(object.free, i);
        shouldBe("absent" in object, false);
    }
    for (let key of ["kept", "getter", "absent", symbol]) {
        let before = JSON.stringify(Object.getOwnPropertyDescriptor(object, key));
        shouldBe(outcome(() => strictStore(object, key, 9)), "TypeError", String(key));
        shouldBe(outcome(() => sloppyStore(object, key, 9)), "stored", String(key));
        shouldBe(Reflect.set(object, key, 9), false, String(key));
        shouldBe(outcome(() => Object.defineProperty(object, key, { value: 9, configurable: true })), "TypeError", String(key));
        shouldBe(Reflect.defineProperty(object, key, { get() { return 9; }, configurable: true }), false, String(key));
        shouldBe(JSON.stringify(Object.getOwnPropertyDescriptor(object, key)), before, String(key));
    }
    for (let key of ["kept", "getter"]) {
        shouldBe(outcome(() => strictDelete(object, key)), "TypeError", key);
        shouldBe(sloppyDelete(object, key), false, key);
        shouldBe(Reflect.deleteProperty(object, key), false, key);
        shouldBe(Object.hasOwn(object, key), true, key);
    }
    for (let key of ["absent", symbol]) {
        shouldBe(strictDelete(object, key), true, String(key));
        shouldBe(Reflect.deleteProperty(object, key), true, String(key));
    }
    // A definition that changes nothing is accepted.
    shouldBe(Reflect.defineProperty(object, "kept", { value: 1 }), true);
    shouldBe(Reflect.defineProperty(object, "kept", { value: 1, writable: true, enumerable: true, configurable: true }), true);
    shouldBe(Reflect.defineProperty(object, "kept", { writable: false }), false);

    for (let key of ["added", otherSymbol, "free"]) {
        shouldBe(strictStore(object, key, 7), "stored", String(key));
        shouldBe(object[key], 7, String(key));
        shouldBe(strictDelete(object, key), true, String(key));
        shouldBe(Reflect.defineProperty(object, key, { get() { return 8; }, configurable: true }), true, String(key));
        shouldBe(object[key], 8, String(key));
    }
    // The list holds after the object got other Structures.
    shouldBe(outcome(() => strictStoreKept(object, 5)), "TypeError");
    shouldBe(outcome(() => strictStoreKept(object, object.kept)), "stored");
    shouldBe(outcome(() => strictStoreAbsent(object, 5)), "TypeError");
    shouldBe(object[0] = 1, 1);
    shouldBe(object[0], 1);
    shouldBe(Reflect.setPrototypeOf(object, null), true);
}

// 2. Numeric keys.
let numericKeys = [0, 5, "7", 4294967294, "4294967295", -1, "-1", 1.5, "1.5", "-0", "NaN", "Infinity", "-Infinity", "1e+21"];
let otherKeys = ["01", "1.0", "+1", "1e3", "0x1", " 1", ""];
{
    let object = $vm.makePropertiesImmutable(make(), [], true);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(outcome(() => strictStore(object, i % 8, i)), "TypeError");
        shouldBe(sloppyStore(object, i % 8, i), "stored");
        shouldBe(read(object, i % 8), undefined);
    }
    for (let key of numericKeys) {
        shouldBe(outcome(() => strictStore(object, key, 9)), "TypeError", String(key));
        shouldBe(sloppyStore(object, key, 9), "stored", String(key));
        shouldBe(Reflect.set(object, key, 9), false, String(key));
        shouldBe(outcome(() => Object.defineProperty(object, key, { value: 9 })), "TypeError", String(key));
        shouldBe(Reflect.defineProperty(object, key, { get() { return 9; } }), false, String(key));
        shouldBe(strictDelete(object, key), true, String(key));
        shouldBe(key in object, false, String(key));
        shouldBe(read(object, key), undefined, String(key));
    }
    shouldBe($vm.indexingMode(object), $vm.indexingMode(make()));
    for (let key of [...otherKeys, "kept", "free", "added"]) {
        shouldBe(strictStore(object, key, 7), "stored", JSON.stringify(key));
        shouldBe(object[key], 7, JSON.stringify(key));
    }
    shouldBe(Object.keys(object).join(), "kept,free," + otherKeys.join() + ",added");

    // An inheritor takes its own elements, and an array that inherits finds nothing in its holes.
    let child = Object.create(object);
    for (let i = 0; i < testLoopCount; ++i) {
        strictStore(child, i % 8, i);
        shouldBe(child[i % 8], i);
    }
    shouldBe(strictStore(child, "1.5", 1), "stored");
    let array = [1, , 3];
    Object.setPrototypeOf(array, object);
    for (let i = 0; i < testLoopCount; ++i)
        shouldBe(read(array, 1), undefined);
    shouldBe(strictStore(array, 1, 2), "stored");
    shouldBe(array[1], 2);

    // Every tier stores into indexed storage in place, so an object that has some cannot take this.
    for (let withElements of [[1], { 0: 1 }, [], Object.assign(make(), { 3: 1 })])
        shouldBe(outcome(() => $vm.makePropertiesImmutable(withElements, [], true)), "TypeError");
}

// 3. The prototype.
{
    let parent = { inherited: 1 };
    let object = $vm.makePropertiesImmutable(Object.create(parent), ["x"], false, true);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(outcome(() => Object.setPrototypeOf(object, {})), "TypeError");
        shouldBe(Reflect.setPrototypeOf(object, null), false);
        shouldBe(object.inherited, 1);
    }
    shouldBe(outcome(() => { "use strict"; object.__proto__ = {}; }), "TypeError");
    shouldBe(outcome(() => { object.__proto__ = {}; }), "TypeError");
    shouldBe(Object.getPrototypeOf(object), parent);
    shouldBe(Object.setPrototypeOf(object, parent), object);
    shouldBe(Reflect.setPrototypeOf(object, parent), true);
    shouldBe(Object.isExtensible(object), true);
    shouldBe(strictStore(object, "y", 1), "stored");
    shouldBe(strictStore(object, 0, 1), "stored");
    shouldBe(Reflect.setPrototypeOf(object, null), false);
    parent.inherited = 2;
    shouldBe(object.inherited, 2);
}

// 4. A second call adds to the first.
{
    let object = $vm.makePropertiesImmutable(make(), ["kept"]);
    shouldBe(strictStore(object, "free", 5), "stored");
    $vm.makePropertiesImmutable(object, ["free"], true);
    $vm.makePropertiesImmutable(object, ["kept"]);
    $vm.makePropertiesImmutable(object, [], false, true);
    shouldBe(outcome(() => strictStore(object, "kept", 9)), "TypeError");
    shouldBe(outcome(() => strictStore(object, "free", 9)), "TypeError");
    shouldBe(outcome(() => strictStore(object, 0, 9)), "TypeError");
    shouldBe(Reflect.setPrototypeOf(object, null), false);
    shouldBe(strictStore(object, "added", 9), "stored");
    shouldBe(object.free, 5);
}

// 5. With preventExtensions no key can be added, and a member that is not listed can still be assigned.
{
    let object = Object.preventExtensions($vm.makePropertiesImmutable(make(), ["kept", "getter"], true, true));
    shouldBe(Object.isExtensible(object), false);
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(outcome(() => strictStoreAbsent(object, i)), "TypeError");
        shouldBe(outcome(() => strictStoreKept(object, i + 2)), "TypeError");
        shouldBe(strictStoreFree(object, i), "stored");
        shouldBe(object.free, i);
    }
    for (let key of ["added", otherSymbol, ...numericKeys, ...otherKeys]) {
        shouldBe(outcome(() => strictStore(object, key, 9)), "TypeError", String(key));
        shouldBe(sloppyStore(object, key, 9), "stored", String(key));
        shouldBe(key in object, false, String(key));
    }
    shouldBe(Object.isFrozen(object), false);
    shouldBe(Object.getOwnPropertyDescriptor(object, "kept").writable, true);
}

// 6. The whole-object form after the listed form.
{
    let object = $vm.makePropertiesImmutable($vm.makePropertiesImmutable(make(), ["kept"], true), undefined);
    shouldBe($vm.hasImmutableProperties(object), true);
    shouldBe(outcome(() => strictStore(object, "free", 9)), "TypeError");
    shouldBe(outcome(() => strictStore(object, "added", 9)), "TypeError");
    shouldBe(Object.isExtensible(object), false);
}

// 7. Every other string. A string is fixed, present or absent, unless it is among the open strings. A symbol is fixed only when it is listed.
{
    let object = $vm.makePropertiesImmutable(make(), [symbol], true, true, ["free", "opened"]);
    shouldBe($vm.hasImmutableProperties(object), false);
    shouldBe(Object.isExtensible(object), true);
    for (let key of ["kept", "getter"]) {
        shouldBe(outcome(() => strictStore(object, key, 9)), "TypeError", key);
        shouldBe(outcome(() => sloppyStore(object, key, 9)), "stored", key);
        shouldBe(Reflect.set(object, key, 9), false, key);
        shouldBe(Reflect.defineProperty(object, key, { value: 9 }), false, key);
        shouldBe(outcome(() => strictDelete(object, key)), "TypeError", key);
        shouldBe(sloppyDelete(object, key), false, key);
    }
    shouldBe(object.kept, 1);
    shouldBe(object.getter, 3);
    for (let key of ["absent", "anything", "", "0", "1.5", symbol]) {
        shouldBe(outcome(() => strictStore(object, key, 9)), "TypeError", String(key));
        shouldBe(Reflect.defineProperty(object, key, { value: 9 }), false, String(key));
        shouldBe(strictDelete(object, key), true, String(key));
        shouldBe(key in object, false, String(key));
    }
    for (let key of ["free", "opened", otherSymbol]) {
        shouldBe(outcome(() => strictStore(object, key, 9)), "stored", String(key));
        shouldBe(object[key], 9, String(key));
        shouldBe(strictDelete(object, key), true, String(key));
        shouldBe(key in object, false, String(key));
        shouldBe(Reflect.defineProperty(object, key, { value: 8, writable: true, configurable: true }), true, String(key));
        shouldBe(object[key], 8, String(key));
    }

    let heir = Object.create(object);
    shouldBe(outcome(() => strictStore(heir, "kept", 9)), "stored");
    shouldBe(outcome(() => strictStore(heir, "anything", 9)), "stored");
    shouldBe(heir.kept, 9);
    shouldBe(object.kept, 1);
}

// 8. A later list closes an open string, and no later list opens a string again.
{
    let object = $vm.makePropertiesImmutable(make(), [], false, false, ["free", "opened"]);
    $vm.makePropertiesImmutable(object, ["free"]);
    shouldBe(outcome(() => strictStore(object, "free", 9)), "TypeError");
    shouldBe(outcome(() => strictStore(object, "opened", 9)), "stored");
    $vm.makePropertiesImmutable(object, [], false, false, ["free", "kept", "opened"]);
    shouldBe(outcome(() => strictStore(object, "free", 9)), "TypeError");
    shouldBe(outcome(() => strictStore(object, "kept", 9)), "TypeError");
    shouldBe(outcome(() => strictStore(object, "opened", 8)), "stored");
    $vm.makePropertiesImmutable(object, [], false, false, []);
    shouldBe(outcome(() => strictStore(object, "opened", 7)), "TypeError");
    shouldBe(object.opened, 8);
}

// 9. An array without element storage, which only Array.prototype is, stays empty under a list that takes in its length and every numeric key.
{
    let array = $vm.makePropertiesImmutable(createGlobalObject().Array.prototype, ["length"], true, true);
    shouldBe(outcome(() => strictStore(array, "length", 1)), "TypeError");
    shouldBe(outcome(() => array.push(1)), "TypeError");
    shouldBe(outcome(() => array.pop()), "TypeError");
    shouldBe(outcome(() => array.unshift(1)), "TypeError");
    shouldBe(outcome(() => strictStore(array, 0, 1)), "TypeError");
    shouldBe(Reflect.defineProperty(array, "length", { value: 1 }), false);
    shouldBe(Reflect.defineProperty(array, "length", { value: 0 }), true);
    shouldBe(array.length, 0);
    shouldBe(outcome(() => strictStore(array, "added", 1)), "stored");
}
