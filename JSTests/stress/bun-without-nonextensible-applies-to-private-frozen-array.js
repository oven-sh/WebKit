//@ requireOptions("--useNonExtensibleAppliesToPrivate=0")
// Bun: without the rule of WebKit/WebKit@b0be2f45a9 a frozen array takes a private field and the brand of private methods, and its elements stay read-only.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

class ReturnsArgument {
    constructor(object) { return object; }
}

class FieldStamper extends ReturnsArgument {
    #field = 1;
    static has(object) { return #field in object; }
    static get(object) { return object.#field; }
}

class MethodStamper extends ReturnsArgument {
    #method() { return 2; }
    static has(object) { return #method in object; }
    static call(object) { return object.#method(); }
}

function outcome(func) {
    try {
        func();
    } catch (error) {
        return error.constructor.name;
    }
    return "none";
}

const makers = [
    () => Object.freeze([1, 2, 3]),
    () => Object.freeze([1.5, 2.5]),
    () => Object.freeze(["a", {}]),
    () => Object.freeze([]),
    () => Object.freeze([1, , 3]),
];

let hot = Math.max(5, Math.ceil(testLoopCount / 5));
for (let i = 0; i < hot; i++) {
    for (let maker = 0; maker < makers.length; maker++) {
        for (const fieldFirst of [true, false]) {
            const array = makers[maker]();
            const length = array.length;
            const first = array[0];
            shouldBe(outcome(() => fieldFirst ? new FieldStamper(array) : new MethodStamper(array)), "none", maker);
            shouldBe(outcome(() => fieldFirst ? new MethodStamper(array) : new FieldStamper(array)), "none", maker);
            shouldBe(FieldStamper.has(array), true, maker);
            shouldBe(FieldStamper.get(array), 1, maker);
            shouldBe(MethodStamper.has(array), true, maker);
            shouldBe(MethodStamper.call(array), 2, maker);
            shouldBe(Object.isFrozen(array), true, maker);
            shouldBe(array.length, length, maker);
            shouldBe(outcome(() => { "use strict"; array[0] = 9; }), "TypeError", maker);
            shouldBe(outcome(() => { "use strict"; array.push(9); }), "TypeError", maker);
            shouldBe(Reflect.set(array, 0, 9), false, maker);
            shouldBe(array[0], first, maker);
            shouldBe(outcome(() => new FieldStamper(array)), "TypeError", maker);
            shouldBe(outcome(() => new MethodStamper(array)), "TypeError", maker);
        }
    }
}
