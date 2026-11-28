// Object.assign() stores with [[Set]]: an element that is already on the target keeps its attributes.

function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function shouldThrow(run, errorType)
{
    let thrown = null;
    try {
        run();
    } catch (error) {
        thrown = error;
    }
    if (!(thrown instanceof errorType))
        throw new Error(`expected a ${errorType.name} but got ${thrown}`);
}

function shouldBeHidden(object, key, value, configurable)
{
    const descriptor = Object.getOwnPropertyDescriptor(object, key);
    shouldBe(descriptor.value, value);
    shouldBe(descriptor.writable, true);
    shouldBe(descriptor.enumerable, false);
    shouldBe(descriptor.configurable, configurable);
}

function withHidden(key, configurable)
{
    return Object.defineProperty({ a: 1 }, key, { value: 9, writable: true, configurable });
}

function assign(target, ...sources)
{
    return Object.assign(target, ...sources);
}
noInline(assign);

for (let i = 0; i < testLoopCount; ++i) {
    let target = assign(withHidden(2, true), { 2: 7, b: 3 });
    shouldBeHidden(target, 2, 7, true);
    shouldBe(Object.keys(target).join(), "a,b");

    shouldBeHidden(assign(withHidden(2, false), { 2: 7 }), 2, 7, false);
    shouldBeHidden(assign(withHidden(4294967294, true), { 4294967294: 7 }), 4294967294, 7, true);
    shouldBeHidden(assign(withHidden(2, true), { b: 3 }, { 2: 7 }), 2, 7, true);
    shouldBeHidden(Object.assign(withHidden(2, true), { 2: 7 }), 2, 7, true);

    // An element that the target did not have is created as usual.
    target = assign(withHidden(2, true), { 1: 5 });
    shouldBe(Object.keys(target).join(), "1,a");
    shouldBeHidden(target, 2, 9, true);
}

{
    let stored;
    const target = Object.defineProperty({}, 2, { set(value) { stored = value; }, configurable: true });
    Object.assign(target, { 2: 7 });
    shouldBe(stored, 7);
    shouldBe(Object.getOwnPropertyDescriptor(target, 2).get, undefined);
}

shouldThrow(() => Object.assign(Object.defineProperty({}, 2, { value: 9, configurable: true }), { 2: 7 }), TypeError);
