"use strict";

// AllowChangesToImmutableProperties: while it is alive, JavaScript changes an object with immutable properties as it does any other.

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

const changes = [
    object => { object.existing = 2; },
    object => { object.fresh = 2; },
    object => { delete object.existing; },
    object => { Object.defineProperty(object, "existing", { get() { return 2; } }); },
    object => { Object.defineProperty(object, "fresh", { value: 2 }); },
    object => { Object.setPrototypeOf(object, null); },
    object => { object.accessor = 2; },
    object => { if (!Reflect.set(object, "existing", 2)) throw new TypeError; },
];

function observe(object) {
    return JSON.stringify([Object.getOwnPropertyDescriptors(object), Object.getPrototypeOf(object) === null, Reflect.get(object, "accessor")]);
}

for (let create of [() => ({ }), () => function () { }, () => [], () => new Map]) {
    function make() {
        let state = 1;
        return Object.defineProperty(Object.assign(create(), { existing: 1 }), "accessor", { get() { return state; }, set(value) { state = value; }, configurable: true });
    }
    for (let change of changes) {
        let expected = make();
        change(expected);

        // The same code in turn, so that nothing it does while it is allowed to is cached for when it is not.
        for (let i = 0; i < 200; ++i) {
            let object = $vm.makePropertiesImmutable(make());
            let before = observe(object);
            shouldThrow(() => change(object), TypeError, change);
            shouldBe(observe(object), before, change);
            $vm.allowChangesToImmutableProperties(() => change(object));
            shouldBe(observe(object), observe(expected), change);
            shouldBe($vm.hasImmutableProperties(object), true);
            shouldThrow(() => { object.another = 1; }, TypeError);
        }
    }
}

// Compiled code that has only ever been allowed to.
{
    function set(object) { object.existing = 2; }
    noInline(set);
    let object = $vm.makePropertiesImmutable({ existing: 1 });
    $vm.allowChangesToImmutableProperties(() => {
        for (let i = 0; i < testLoopCount; ++i)
            set(object);
    });
    shouldBe(object.existing, 2);
    object = $vm.makePropertiesImmutable({ existing: 1 });
    shouldThrow(() => set(object), TypeError);
    shouldBe(object.existing, 1);
}

// It ends when the function throws.
{
    let object = $vm.makePropertiesImmutable({ });
    shouldThrow(() => $vm.allowChangesToImmutableProperties(() => { throw new RangeError; }), RangeError);
    shouldThrow(() => { object.fresh = 1; }, TypeError);
}
