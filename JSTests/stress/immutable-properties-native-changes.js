"use strict";

// Under AllowLazyMaterializationOfImmutableProperties, native code defines and deletes the properties of an object with immutable
// properties and sets its prototype, with the result each has for any other object.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function shouldThrow(func, errorType, message) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error((message ? message + ": " : "") + "expected " + errorType.name + " but got " + String(error));
}

const symbol = Symbol("symbol");
const otherPrototype = { inherited: 1 };
function getter() { return 2; }

// [what native code does, the same for an object that is not immutable]
const changes = [
    [object => $vm.definePropertyAllowingLazyMaterialization(object, "existing", { value: 2 }), object => Object.defineProperty(object, "existing", { value: 2 })],
    [object => $vm.definePropertyAllowingLazyMaterialization(object, "existing", { get: getter }), object => Object.defineProperty(object, "existing", { get: getter })],
    [object => $vm.definePropertyAllowingLazyMaterialization(object, "existing", { enumerable: false }), object => Object.defineProperty(object, "existing", { enumerable: false })],
    [object => $vm.definePropertyAllowingLazyMaterialization(object, "fresh", { value: 2, writable: true }), object => Object.defineProperty(object, "fresh", { value: 2, writable: true })],
    [object => $vm.definePropertyAllowingLazyMaterialization(object, "fresh", { get: getter, configurable: true }), object => Object.defineProperty(object, "fresh", { get: getter, configurable: true })],
    [object => $vm.definePropertyAllowingLazyMaterialization(object, symbol, { value: 2 }), object => Object.defineProperty(object, symbol, { value: 2 })],
    [object => $vm.deletePropertyAllowingLazyMaterialization(object, "existing"), object => delete object.existing],
    [object => $vm.deletePropertyAllowingLazyMaterialization(object, "absent"), object => delete object.absent],
    [object => $vm.setPrototypeAllowingLazyMaterialization(object, null), object => Object.setPrototypeOf(object, null)],
    [object => $vm.setPrototypeAllowingLazyMaterialization(object, otherPrototype), object => Object.setPrototypeOf(object, otherPrototype)],
];

function observe(object) {
    let descriptors = Reflect.ownKeys(object).map(key => [String(key), JSON.stringify(Reflect.getOwnPropertyDescriptor(object, key), (name, value) => typeof value === "function" ? "function " + value.name : value)]);
    let prototype = Object.getPrototypeOf(object);
    return JSON.stringify([descriptors, prototype === null ? "null" : prototype === otherPrototype ? "other" : "original"]);
}

for (let create of [() => ({ }), () => function () { }, () => new Map, () => Object.create(null)]) {
    let make = () => Object.assign(create(), { existing: 1 });
    for (let [changeNatively, change] of changes) {
        let expected = make();
        change(expected);

        let object = $vm.makePropertiesImmutable(make());
        let before = observe(object);
        if (observe(expected) !== before)
            shouldThrow(() => change(object), TypeError, change);
        shouldBe(observe(object), before, change);
        changeNatively(object);
        shouldBe(observe(object), observe(expected), changeNatively);

        shouldBe($vm.hasImmutableProperties(object), true, changeNatively);
        shouldBe(Object.isExtensible(object), false, changeNatively);
        let after = observe(object);
        shouldThrow(() => { object.another = 1; }, TypeError, changeNatively);
        shouldThrow(() => { Object.defineProperty(object, "existing", { value: 3 }); }, TypeError, changeNatively);
        shouldThrow(() => { Object.setPrototypeOf(object, { }); }, TypeError, changeNatively);
        shouldBe(observe(object), after, changeNatively);
    }
}

// Nor is an object with elements, which stay as they are afterwards.
for (let create of [() => [], () => [1, 2, 3], () => [1.5], () => ["a"], () => ({ 0: 1 }), () => Object.assign([1], { existing: 1 })]) {
    let object = $vm.makePropertiesImmutable(create());
    let before = observe(object);
    shouldThrow(() => $vm.definePropertyAllowingLazyMaterialization(object, "fresh", { value: 2 }), TypeError);
    shouldThrow(() => $vm.definePropertyAllowingLazyMaterialization(object, "fresh", { get: getter }), TypeError);
    shouldThrow(() => $vm.definePropertyAllowingLazyMaterialization(object, symbol, { value: 2 }), TypeError);
    if (Array.isArray(object))
        shouldThrow(() => $vm.definePropertyAllowingLazyMaterialization(object, "length", { value: object.length + 1 }), TypeError);
    shouldBe($vm.deletePropertyAllowingLazyMaterialization(object, "existing"), !Object.hasOwn(object, "existing"));
    $vm.setPrototypeAllowingLazyMaterialization(object, Object.getPrototypeOf(object));
    shouldThrow(() => { object[0] = 99; }, TypeError);
    shouldThrow(() => { object[5] = 99; }, TypeError);
    if (Array.isArray(object))
        shouldThrow(() => { object.push(5); }, TypeError);
    shouldBe(observe(object), before);
}

// An index is not for native code to change either.
for (let create of [() => ({ }), () => [1], () => ({ 0: 1 }), () => function () { }]) {
    let object = $vm.makePropertiesImmutable(create());
    let before = observe(object);
    for (let index of [0, 1, 100000]) {
        shouldThrow(() => $vm.definePropertyAllowingLazyMaterialization(object, index, { value: 2 }), TypeError, index);
        shouldThrow(() => $vm.definePropertyAllowingLazyMaterialization(object, index, { get: getter }), TypeError, index);
        shouldBe($vm.deletePropertyAllowingLazyMaterialization(object, index), !Object.hasOwn(object, index), index);
    }
    shouldBe(observe(object), before);
}

// What is refused for any object is refused.
{
    let object = $vm.makePropertiesImmutable(Object.defineProperty({ }, "fixed", { value: 1 }));
    shouldThrow(() => $vm.definePropertyAllowingLazyMaterialization(object, "fixed", { value: 2 }), TypeError);
    shouldBe($vm.deletePropertyAllowingLazyMaterialization(object, "fixed"), false);
    shouldBe(object.fixed, 1);
    let child = $vm.makePropertiesImmutable(Object.create(object));
    shouldThrow(() => $vm.setPrototypeAllowingLazyMaterialization(object, child), TypeError);
}

// Compiled code that has read the object sees each change.
{
    function read(object) { return object.existing; }
    noInline(read);
    function has(object) { return "existing" in object; }
    noInline(has);

    let grandparent = $vm.makePropertiesImmutable({ });
    let parent = $vm.makePropertiesImmutable(Object.assign(Object.create(grandparent), { existing: 1 }));
    let child = Object.create(parent);
    function check(value) {
        for (let object of [parent, child]) {
            for (let i = 0; i < testLoopCount; ++i) {
                shouldBe(read(object), value);
                shouldBe(has(object), value !== undefined);
            }
        }
    }
    check(1);
    $vm.definePropertyAllowingLazyMaterialization(parent, "existing", { value: 2 });
    check(2);
    $vm.definePropertyAllowingLazyMaterialization(parent, "existing", { get() { return 3; } });
    check(3);
    $vm.deletePropertyAllowingLazyMaterialization(parent, "existing");
    check(undefined);
    $vm.definePropertyAllowingLazyMaterialization(grandparent, "existing", { value: 4 });
    check(4);
    $vm.setPrototypeAllowingLazyMaterialization(parent, { existing: 5 });
    check(5);
    $vm.definePropertyAllowingLazyMaterialization(parent, "existing", { value: 6 });
    check(6);
}
