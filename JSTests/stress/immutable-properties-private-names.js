// Private names and an object with immutable properties: a private field or method cannot be added to it; a private field it already has can
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

let hot = Math.max(5, Math.ceil(testLoopCount / 5));

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

// Adding one fails, hot or cold.
{
    for (let i = 0; i < hot; i++) {
        new FieldStamper({});
        new MethodStamper({});
        new PublicFieldStamper({});
    }
    let immutable = $vm.makePropertiesImmutable({ a: 1 });
    for (let i = 0; i < 50; i++) {
        shouldThrow(() => new FieldStamper(immutable), TypeError);
        shouldThrow(() => new MethodStamper(immutable), TypeError);
        shouldThrow(() => new PublicFieldStamper(immutable), TypeError);
    }
    shouldBe(FieldStamper.has(immutable), false);
    shouldBe(MethodStamper.has(immutable), false);
    shouldBe("publicField" in immutable, false);
}

// What was added before keeps working, and the field can still be written.
{
    let object = {};
    new FieldStamper(object);
    new MethodStamper(object);
    for (let i = 0; i < hot; i++)
        FieldStamper.set(object, i);
    $vm.makePropertiesImmutable(object);
    shouldBe(FieldStamper.has(object), true);
    shouldBe(FieldStamper.get(object), hot - 1);
    for (let i = 0; i < hot; i++)
        FieldStamper.set(object, -i);
    shouldBe(FieldStamper.get(object), -(hot - 1));
    shouldBe(MethodStamper.call(object), 2);
    shouldThrow(() => new FieldStamper(object), TypeError);
    shouldBe(Reflect.ownKeys(object).length, 0);
}

// An instance of a class with private state, made immutable after construction.
{
    class Counter {
        #count = 0;
        increment() { return ++this.#count; }
    }
    let counter = $vm.makePropertiesImmutable(new Counter());
    shouldBe(counter.increment(), 1);
    shouldBe(counter.increment(), 2);
    shouldThrow(() => { "use strict"; counter.extra = 1; }, TypeError);
}

// A public field is CreateDataPropertyOrThrow, which is [[DefineOwnProperty]]: through a constructor that returns its argument it
// succeeds when it changes nothing, whatever cell or number representation the equal value arrives in, and throws otherwise. The
// same holds for an index key, for a copy-on-write array and for the others, and it agrees with Object.defineProperty().
{
    function outcome(func) {
        try {
            func();
            return "ok";
        } catch (e) {
            return e.constructor.name;
        }
    }
    let suffix = "b", ten = 10n, quarter = 0.25;
    let make = () => ({ text: "ab", number: 1, half: 2.5, big: 10n ** 30n, zero: 0, nan: NaN, symbol: Symbol.for("s"), object: Object.prototype });
    let withoutHoles = () => { let a = []; for (let i = 0; i < 4; ++i) a.push(i + 0.5); return a; };
    let holey = () => { let a = withoutHoles(); a[9] = 9.5; return a; };
    let target;
    class Base { constructor() { return target; } }
    let unchanged = {
        ropeString: class extends Base { text = "a" + suffix; }, joinedString: class extends Base { text = ["a", "b"].join(""); }, computedNumber: class extends Base { number = quarter * 4; },
        computedDouble: class extends Base { half = quarter * 10; }, anotherBigIntCell: class extends Base { big = ten ** 30n; }, nan: class extends Base { nan = 0 / 0; }, symbol: class extends Base { symbol = Symbol.for("s"); },
        object: class extends Base { object = Object.prototype; }, several: class extends Base { text = "a" + suffix; number = 1; nan = NaN; },
    };
    let changed = {
        minusZero: class extends Base { zero = -0; }, longerString: class extends Base { text = "a" + suffix + "c"; }, anotherBigInt: class extends Base { big = ten ** 30n + 1n; }, anotherNumber: class extends Base { number = 2; },
        newName: class extends Base { added = 1; }, anotherObject: class extends Base { object = Array.prototype; }, secondOfTwo: class extends Base { number = 1; text = "x"; },
    };
    for (let name in unchanged) {
        target = $vm.makePropertiesImmutable(make());
        let before = JSON.stringify(Object.getOwnPropertyDescriptors(target), (k, v) => typeof v === "bigint" ? String(v) : v);
        for (let i = 0; i < hot; ++i)
            shouldBe(outcome(() => new unchanged[name]), "ok", name);
        shouldBe(JSON.stringify(Object.getOwnPropertyDescriptors(target), (k, v) => typeof v === "bigint" ? String(v) : v), before, name);
    }
    for (let name in changed) {
        target = $vm.makePropertiesImmutable(make());
        let before = JSON.stringify(Object.getOwnPropertyDescriptors(target), (k, v) => typeof v === "bigint" ? String(v) : v);
        for (let i = 0; i < hot; ++i)
            shouldBe(outcome(() => new changed[name]), "TypeError", name);
        shouldBe(JSON.stringify(Object.getOwnPropertyDescriptors(target), (k, v) => typeof v === "bigint" ? String(v) : v), before, name);
    }
    // The same answers from Object.defineProperty().
    target = $vm.makePropertiesImmutable(make());
    shouldBe(outcome(() => Object.defineProperty(target, "text", { value: "a" + suffix })), "ok");
    shouldBe(outcome(() => Object.defineProperty(target, "big", { value: ten ** 30n })), "ok");
    shouldBe(outcome(() => Object.defineProperty(target, "zero", { value: -0 })), "TypeError");
    // Index keys.
    for (let makeArray of [withoutHoles, holey]) {
        class SameElement extends Base { 1 = quarter * 6; }
        class OtherElement extends Base { 1 = 7; }
        class NewElement extends Base { 20 = 7; }
        target = $vm.makePropertiesImmutable(makeArray());
        let before = JSON.stringify(target);
        for (let i = 0; i < hot; ++i) {
            shouldBe(outcome(() => new SameElement), "ok");
            shouldBe(outcome(() => new OtherElement), "TypeError");
            shouldBe(outcome(() => new NewElement), "TypeError");
        }
        shouldBe(JSON.stringify(target), before);
    }
    // A function, whose name is materialized lazily.
    function named() { }
    target = $vm.makePropertiesImmutable(named);
    shouldBe(outcome(() => Object.defineProperty(target, "name", { value: "na" + "med" })), "ok");
    shouldBe(outcome(() => Object.defineProperty(target, "name", { value: "other" })), "TypeError");
    shouldBe(named.name, "named");
}
