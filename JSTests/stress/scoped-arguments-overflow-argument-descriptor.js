function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected: ${String(expected)}`);
}

function shouldThrow(func, errorConstructor)
{
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorConstructor))
        throw new Error(`bad error: ${String(error)}`);
}

// Nothing captures a parameter, so `arguments` is a DirectArguments object.
function direct(a, b)
{
    return arguments;
}

// A closure captures the parameters, so `arguments` is a ScopedArguments object. It keeps the arguments past its
// two parameters in a storage of its own.
function scoped(a, b)
{
    (function () { return a + b; })();
    return arguments;
}

function descriptorOf(args, index)
{
    let descriptor = Object.getOwnPropertyDescriptor(args, index);
    if (!descriptor)
        return "none";
    return `${String(descriptor.value)} ${descriptor.writable ? "W" : "-"}${descriptor.enumerable ? "E" : "-"}${descriptor.configurable ? "C" : "-"}`;
}

// Sorted: this test is not about the order of the keys.
function enumerated(args)
{
    let keys = [];
    for (let key in args)
        keys.push(key);
    return keys.sort().join();
}

// Index 0 has a parameter. Indices 2 and 3 are arguments past the parameters.
function testIndex(create, index)
{
    let value = index + 1;
    let otherKeys = ["0", "1", "2", "3"].filter(key => key !== String(index)).join();

    let args = create(1, 2, 3, 4);
    Object.defineProperty(args, index, { enumerable: false });
    shouldBe(descriptorOf(args, index), `${value} W-C`);
    shouldBe(Object.prototype.propertyIsEnumerable.call(args, index), false);
    shouldBe(enumerated(args), otherKeys);
    args[index] = 7;
    shouldBe(descriptorOf(args, index), "7 W-C");
    Object.defineProperty(args, index, { enumerable: true });
    shouldBe(descriptorOf(args, index), "7 WEC");
    shouldBe(enumerated(args), "0,1,2,3");

    args = create(1, 2, 3, 4);
    Object.defineProperty(args, index, { configurable: false });
    shouldBe(descriptorOf(args, index), `${value} WE-`);
    shouldBe(delete args[index], false);
    shouldBe(Reflect.deleteProperty(args, index), false);
    shouldBe(index in args, true);
    shouldBe(args[index], value);
    shouldThrow(() => Object.defineProperty(args, index, { configurable: true }), TypeError);
    shouldThrow(() => Object.defineProperty(args, index, { enumerable: false }), TypeError);
    shouldThrow(() => Object.defineProperty(args, index, { get() { } }), TypeError);
    shouldBe(Reflect.defineProperty(args, index, { configurable: true }), false);
    shouldBe(Reflect.defineProperty(args, index, { enumerable: false }), false);
    shouldBe(Reflect.defineProperty(args, index, { get() { } }), false);
    shouldBe(descriptorOf(args, index), `${value} WE-`);
    args[index] = 7;
    shouldBe(descriptorOf(args, index), "7 WE-");
    Object.defineProperty(args, index, { writable: false });
    shouldBe(descriptorOf(args, index), "7 -E-");
    args[index] = 8;
    shouldBe(args[index], 7);
    shouldThrow(() => Object.defineProperty(args, index, { value: 8 }), TypeError);

    args = create(1, 2, 3, 4);
    Object.defineProperty(args, index, { value: 9, enumerable: false, configurable: false });
    shouldBe(descriptorOf(args, index), "9 W--");
    shouldBe(delete args[index], false);
    shouldBe(args[index], 9);

    args = create(1, 2, 3, 4);
    shouldBe(delete args[index], true);
    shouldBe(index in args, false);
    shouldBe(descriptorOf(args, index), "none");
    args[index] = 7;
    shouldBe(descriptorOf(args, index), "7 WEC");
    shouldBe(args.length, 4);

    // A define that leaves the index configurable, a store, then a delete: the index is gone, and the value that
    // the define saw does not come back.
    args = create(1, 2, 3, 4);
    Object.defineProperty(args, index, { enumerable: true });
    args[index] = 7;
    shouldBe(delete args[index], true);
    shouldBe(index in args, false);
    shouldBe(args[index], undefined);
    shouldBe(descriptorOf(args, index), "none");
    shouldBe(enumerated(args), otherKeys);
    args[index] = 8;
    shouldBe(descriptorOf(args, index), "8 WEC");
}

// Array.prototype.pop deletes the last index.
function testPop(create)
{
    let args = create(1, 2, 3, 4);
    Object.defineProperty(args, 3, { value: "defined" });
    args[3] = "stored";
    shouldBe(Array.prototype.pop.call(args), "stored");
    shouldBe(args.length, 3);
    shouldBe(3 in args, false);
    shouldBe(args[3], undefined);
}

function testIntegrityLevel(create)
{
    let sealed = create(1, 2, 3, 4);
    Object.seal(sealed);
    shouldBe(Object.isSealed(sealed), true);
    shouldBe(Object.isFrozen(sealed), false);
    for (let index = 0; index < 4; ++index) {
        shouldBe(descriptorOf(sealed, index), `${index + 1} WE-`);
        shouldBe(delete sealed[index], false);
        shouldBe(Reflect.defineProperty(sealed, index, { configurable: true }), false);
        sealed[index] = -index;
        shouldBe(sealed[index], -index);
    }
    shouldBe(sealed.length, 4);

    let frozen = create(1, 2, 3, 4);
    Object.freeze(frozen);
    shouldBe(Object.isSealed(frozen), true);
    shouldBe(Object.isFrozen(frozen), true);
    for (let index = 0; index < 4; ++index) {
        shouldBe(descriptorOf(frozen, index), `${index + 1} -E-`);
        shouldBe(delete frozen[index], false);
        frozen[index] = -index;
        shouldBe(frozen[index], index + 1);
    }
    shouldBe(frozen.length, 4);
}

// Fewer arguments than parameters: index 1 has a parameter and no argument, so it is an ordinary property.
function testFewerArguments(create)
{
    let args = create(1);
    shouldBe(descriptorOf(args, 1), "none");
    Object.defineProperty(args, 1, { value: 2, configurable: false });
    shouldBe(descriptorOf(args, 1), "2 ---");
    shouldBe(delete args[1], false);
    shouldThrow(() => Object.defineProperty(args, 1, { configurable: true }), TypeError);
    Object.defineProperty(args, 0, { enumerable: false });
    shouldBe(descriptorOf(args, 0), "1 W-C");
    shouldBe(args.length, 1);
    Object.seal(args);
    shouldBe(Object.isSealed(args), true);
    shouldBe(args.length, 1);

    let none = create();
    Object.defineProperty(none, 0, { value: 1, enumerable: true, configurable: false });
    shouldBe(descriptorOf(none, 0), "1 -E-");
    shouldBe(delete none[0], false);
    shouldBe(none.length, 0);
}

for (let create of [direct, scoped]) {
    for (let index of [0, 2, 3])
        testIndex(create, index);
    testPop(create);
    testIntegrityLevel(create);
    testFewerArguments(create);
}

// A reader that the JIT compiles sees an argument past the parameters with a changed descriptor.
function read(args, index)
{
    return args[index];
}
noInline(read);

let changed = scoped(1, 2, 3, 4);
Object.defineProperty(changed, 3, { enumerable: false, configurable: false });
let unchanged = scoped(1, 2, 3, 4);
for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(read(unchanged, 3), 4);
    shouldBe(read(changed, 3), 4);
    shouldBe(read(changed, 0), 1);
}
changed[3] = 5;
shouldBe(read(changed, 3), 5);
shouldBe(delete changed[3], false);
shouldBe(read(changed, 3), 5);
