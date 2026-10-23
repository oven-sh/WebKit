//@ requireOptions("--useShadowRealm=1")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${String(expected)} but got ${String(actual)}`);
}

function shouldThrowTypeError(func, expectedMessage) {
    let error;
    let threw = false;
    try {
        func();
    } catch (e) {
        threw = true;
        error = e;
    }
    if (!threw)
        throw new Error("did not throw");
    if (!(error instanceof TypeError))
        throw new Error(`expected a TypeError from the incubating realm but got ${String(error)}`);
    shouldBe(error.message, expectedMessage);
}

let realm = new ShadowRealm();

// [source text of the thrown value, expected TypeError message]
const symbolCases = [
    [`Symbol("desc")`, "Symbol(desc)"],
    [`Symbol()`, "Symbol()"],
    [`Symbol.for("registered")`, "Symbol(registered)"],
    [`Symbol.iterator`, "Symbol(Symbol.iterator)"],
    [`Symbol.hasInstance`, "Symbol(Symbol.hasInstance)"],
    // An Error whose message is a Symbol cannot be described without ToString either. The caller
    // gets the bare TypeError.
    [`Object.assign(new Error("x"), { message: Symbol("m") })`, "Type error"],
    [`Object.assign(new RangeError("x"), { message: Symbol() })`, "Type error"],
];

// Other primitives and Error objects keep their description.
const otherCases = [
    [`"a string"`, "a string"],
    [`""`, "Type error"],
    [`42`, "42"],
    [`-0`, "0"],
    [`10n ** 30n`, "1000000000000000000000000000000"],
    [`undefined`, "undefined"],
    [`null`, "null"],
    [`new RangeError("boom")`, "boom"],
    [`new Error("")`, "Type error"],
];

// A JSFunction target takes remoteFunctionCallForJSFunction, any other callable takes remoteFunctionCallGeneric.
const targetKinds = [
    (body) => `() => { ${body} }`,
    (body) => `new Proxy(() => { ${body} }, {})`,
    (body) => `new Proxy(function () {}, { apply() { ${body} } })`,
    (body) => `(function () { ${body} }).bind(null)`,
];

// A callable passed into the shadow realm that throws: the shadow realm gets a TypeError from its
// own realm. Report what it saw as a primitive so that it can cross back.
let callAndDescribe = realm.evaluate(`(f) => {
    try {
        f();
    } catch (e) {
        return (e instanceof TypeError) + ":" + e.message;
    }
    return "did not throw";
}`);

for (let [thrown, message] of symbolCases.concat(otherCases)) {
    for (let makeTarget of targetKinds) {
        let source = makeTarget(`throw ${thrown};`);

        // Shadow realm function wrapped for the incubating realm.
        shouldThrowTypeError(realm.evaluate(source), message);

        // Incubating realm function wrapped for the shadow realm.
        shouldBe(callAndDescribe((0, eval)(source)), "true:" + message);
    }
}
// Only an Error instance has its message read on this path.
shouldThrowTypeError(realm.evaluate(`() => { throw { message: "not an Error instance" }; }`), "Type error");

// ShadowRealm.prototype.evaluate, when the evaluated script itself throws. The thrown value is
// described the same way, but this path has its own generic message.
const genericEvaluateMessage = "Error encountered during evaluation";
for (let [thrown, message] of symbolCases.concat(otherCases))
    shouldThrowTypeError(() => realm.evaluate(`throw ${thrown}`), message === "Type error" ? genericEvaluateMessage : message);
// evaluate() also reads the own "message" data property of a thrown object that is not an Error...
shouldThrowTypeError(() => realm.evaluate(`throw { message: "plain object" }`), "plain object");
shouldThrowTypeError(() => realm.evaluate(`throw { message: 42 }`), "42");
// ... but runs none of the thrown value's code to do so, and lets nothing else out in its place.
shouldThrowTypeError(() => realm.evaluate(`throw { get message() { throw new Error("getter ran") } }`), genericEvaluateMessage);
shouldThrowTypeError(() => realm.evaluate(`throw { message: { toString() { throw new Error("toString ran") } } }`), genericEvaluateMessage);
shouldThrowTypeError(() => realm.evaluate(`throw { message: { toString() { return "toString ran" } } }`), genericEvaluateMessage);
shouldThrowTypeError(() => realm.evaluate(`throw new Proxy(new Error("proxy"), { getOwnPropertyDescriptor() { throw new Error("trap ran") } })`), genericEvaluateMessage);

// Through the JITs.
let thrower = realm.evaluate(`(i) => { throw Symbol("s" + i); }`);
for (let i = 0; i < testLoopCount; ++i)
    shouldThrowTypeError(() => thrower(i), "Symbol(s" + i + ")");
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(callAndDescribe(() => { throw Symbol(i); }), "true:Symbol(" + i + ")");
