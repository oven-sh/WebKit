// Private names and a locked object: a private field or method cannot be added to it; a private field it already has can
// still be written, since that value is the object's private state, as an internal slot is.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function shouldThrow(func, errorType) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error("expected " + errorType.name + " but got " + String(error));
}

class ReturnsArgument {
    constructor(object) { return object; }
}

class FieldStamper extends ReturnsArgument {
    #field = 1;
    static has(object) { return #field in object; }
    static get(object) { return object.#field; }
    static set(object, value) { object.#field = value; }
}

class MethodStamper extends ReturnsArgument {
    #method() { return 2; }
    static has(object) { return #method in object; }
    static call(object) { return object.#method(); }
}

class PublicFieldStamper extends ReturnsArgument {
    publicField = 3;
}

// Adding to a locked object fails, hot or cold.
{
    for (let i = 0; i < 2000; i++) {
        new FieldStamper({});
        new MethodStamper({});
        new PublicFieldStamper({});
    }
    let locked = $vm.lockObject({ a: 1 });
    for (let i = 0; i < 50; i++) {
        shouldThrow(() => new FieldStamper(locked), TypeError);
        shouldThrow(() => new MethodStamper(locked), TypeError);
        shouldThrow(() => new PublicFieldStamper(locked), TypeError);
    }
    shouldBe(FieldStamper.has(locked), false);
    shouldBe(MethodStamper.has(locked), false);
    shouldBe("publicField" in locked, false);
}

// What was added before the lock keeps working, and the field can still be written.
{
    let object = {};
    new FieldStamper(object);
    new MethodStamper(object);
    for (let i = 0; i < 2000; i++)
        FieldStamper.set(object, i);
    $vm.lockObject(object);
    shouldBe(FieldStamper.has(object), true);
    shouldBe(FieldStamper.get(object), 1999);
    for (let i = 0; i < 2000; i++)
        FieldStamper.set(object, -i);
    shouldBe(FieldStamper.get(object), -1999);
    shouldBe(MethodStamper.call(object), 2);
    shouldThrow(() => new FieldStamper(object), TypeError);
    shouldBe(Reflect.ownKeys(object).length, 0);
}

// An instance of a class with private state, locked after construction.
{
    class Counter {
        #count = 0;
        increment() { return ++this.#count; }
    }
    let counter = $vm.lockObject(new Counter());
    shouldBe(counter.increment(), 1);
    shouldBe(counter.increment(), 2);
    shouldThrow(() => { "use strict"; counter.extra = 1; }, TypeError);
}
