load("./resources/objects-with-deleted-properties.js", "caller relative");

function shouldBe(actual, expected, label) {
    if (actual !== expected)
        throw new Error(`${label}: expected ${String(expected)} but got ${String(actual)}`);
}

function shouldThrowTypeError(operation, label) {
    let error;
    try {
        operation();
    } catch (caught) {
        error = caught;
    }
    if (!(error instanceof TypeError))
        throw new Error(`${label}: expected a TypeError but got ${String(error)}`);
}

function describeAttributes(object) {
    return Reflect.ownKeys(object).map(key => {
        const { writable, enumerable, configurable } = Object.getOwnPropertyDescriptor(object, key);
        return `${String(key)}: ${writable} ${enumerable} ${configurable}`;
    }).join("; ");
}

function testAbsentKey(immutable, key, label) {
    immutable[key] = 1;
    shouldBe(Object.hasOwn(immutable, key), false, `${label}: own property after a sloppy store`);
    shouldThrowTypeError(() => { "use strict"; immutable[key] = 1; }, `${label}: strict store`);
    shouldBe(Reflect.defineProperty(immutable, key, { value: 1 }), false, `${label}: Reflect.defineProperty`);
}

function testOwnKey(immutable, key, value, label) {
    if ("value" in Object.getOwnPropertyDescriptor(immutable, key)) {
        immutable[key] = "other";
        shouldBe(immutable[key], value, `${label}: value after a sloppy store`);
        shouldThrowTypeError(() => { "use strict"; immutable[key] = "other"; }, `${label}: strict store`);
    }
    shouldBe(delete immutable[key], false, `${label}: sloppy delete`);
    shouldThrowTypeError(() => { "use strict"; delete immutable[key]; }, `${label}: strict delete`);
    shouldBe(Object.hasOwn(immutable, key), true, `${label}: own property`);
    shouldBe(immutable[key], value, `${label}: value`);
}

function testInheritor(immutable, keys, label) {
    const inheritor = Object.create(immutable);
    for (const key of keys) {
        const descriptor = Object.getOwnPropertyDescriptor(immutable, key);
        if (typeof key !== "string" || !("value" in descriptor))
            continue;
        inheritor[key] = "own";
        shouldBe(Object.hasOwn(inheritor, key), descriptor.writable, `${label}: inheritor has an own ${key}`);
    }
    inheritor.a = "own";
    shouldBe(inheritor.a, "own", `${label}: inheritor.a`);
}

function testCopy(immutable, label) {
    const copy = { ...immutable };
    shouldBe($vm.hasImmutableProperties(copy), false, `${label}: hasImmutableProperties(copy)`);
    shouldBe(Object.keys(copy).join(), Object.keys(immutable).join(), `${label}: keys of the copy`);
}

for (const [name, create] of Object.entries({ ...plainObjectCreators, ...functionAndArrayCreators })) {
    const object = create();
    const attributes = describeAttributes(object);
    const keys = Reflect.ownKeys(object);
    const values = keys.map(key => object[key]);

    shouldBe($vm.makePropertiesImmutable(object), object, `${name}: makePropertiesImmutable(object)`);
    shouldBe($vm.hasImmutableProperties(object), true, `${name}: hasImmutableProperties(object)`);
    shouldBe(Object.isExtensible(object), false, `${name}: Object.isExtensible(object)`);
    shouldBe(describeAttributes(object), attributes, `${name}: attributes`);

    for (const key of possiblyAbsentKeys) {
        if (!Object.hasOwn(object, key))
            testAbsentKey(object, key, `${name}, absent ${key}`);
    }
    keys.forEach((key, index) => testOwnKey(object, key, values[index], `${name}, ${String(key)}`));
    testInheritor(object, keys, name);
    testCopy(object, name);
    shouldBe(describeAttributes(object), attributes, `${name}: attributes after every operation`);
}

function deleteA(object) {
    delete object.a;
}
noInline(deleteA);

function storeB(object, value) {
    object.b = value;
}
noInline(storeB);

function addA(object) {
    object.a = 1;
}
noInline(addA);

for (let i = 0; i < testLoopCount; ++i) {
    const object = { a: 1, b: 2 };
    deleteA(object);
    storeB(object, i);
    if (i % 2)
        addA(object);
}

const immutable = { a: 1, b: 2 };
deleteA(immutable);
storeB(immutable, "initial");
$vm.makePropertiesImmutable(immutable);
for (let i = 0; i < testLoopCount; ++i) {
    storeB(immutable, i);
    addA(immutable);
    deleteA(immutable);
}
shouldBe(immutable.b, "initial", "immutable.b after stores from optimized code");
shouldBe("a" in immutable, false, "immutable.a after stores from optimized code");
shouldBe(Reflect.ownKeys(immutable).length, 1, "key count after stores from optimized code");
