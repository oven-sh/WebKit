// `__proto__: value` in an object literal sets the [[Prototype]] of the object itself
// (PropertyDefinitionEvaluation, isProtoSetter). It is not a [[Set]], so it never reaches an
// accessor named `__proto__` on Object.prototype. The JSON-shaped fast paths of eval and of a
// program (JSONP) have to agree with the bytecode of an object literal.

function shouldBe(actual, expected, message)
{
    if (actual !== expected)
        throw new Error(message + ": expected " + String(expected) + " but got " + String(actual));
}

let accessorCalls = 0;

const evaluators = {
    "indirect eval": text => (0, eval)("(" + text + ")"),
    "direct eval": text => eval("(" + text + ")"),
    "program, var": text => {
        loadString("var jsonpResult = " + text);
        return globalThis.jsonpResult;
    },
    "program, assignment": text => {
        loadString("jsonpResult = " + text);
        return globalThis.jsonpResult;
    },
};

const ownKeys = object => Reflect.ownKeys(object).join();

const literals = [
    ['{"__proto__": null}', (o, m) => {
        shouldBe(Object.getPrototypeOf(o), null, m);
        shouldBe(ownKeys(o), "", m);
    }],
    ['{__proto__: null}', (o, m) => {
        shouldBe(Object.getPrototypeOf(o), null, m);
        shouldBe(ownKeys(o), "", m);
    }],
    ['{"__proto__": {"inherited": 1}}', (o, m) => {
        shouldBe(Object.getPrototypeOf(Object.getPrototypeOf(o)), Object.prototype, m);
        shouldBe(o.inherited, 1, m);
        shouldBe(ownKeys(o), "", m);
    }],
    ['{"__proto__": [1, 2]}', (o, m) => {
        shouldBe(Array.isArray(Object.getPrototypeOf(o)), true, m);
        shouldBe(ownKeys(o), "", m);
    }],
    ['{"__proto__": 1}', (o, m) => {
        shouldBe(Object.getPrototypeOf(o), Object.prototype, m);
        shouldBe(ownKeys(o), "", m);
    }],
    ['{"__proto__": "text"}', (o, m) => {
        shouldBe(Object.getPrototypeOf(o), Object.prototype, m);
        shouldBe(ownKeys(o), "", m);
    }],
    ['{"a": 1, "__proto__": null, "b": 2}', (o, m) => {
        shouldBe(Object.getPrototypeOf(o), null, m);
        shouldBe(ownKeys(o), "a,b", m);
    }],
    ['{"a": 1, "__proto__": {"inherited": 1}, "b": 2}', (o, m) => {
        shouldBe(o.inherited, 1, m);
        shouldBe(ownKeys(o), "a,b", m);
    }],
    ['{"outer": {"__proto__": null, "b": 1}}', (o, m) => {
        shouldBe(Object.getPrototypeOf(o), Object.prototype, m);
        shouldBe(Object.getPrototypeOf(o.outer), null, m);
        shouldBe(ownKeys(o.outer), "b", m);
    }],
    ['[{"__proto__": null}, {"__proto__": {"inherited": 1}}]', (o, m) => {
        shouldBe(Object.getPrototypeOf(o[0]), null, m);
        shouldBe(o[1].inherited, 1, m);
    }],
];

function check(state)
{
    for (const [evaluatorName, evaluate] of Object.entries(evaluators)) {
        for (const [text, verify] of literals)
            verify(evaluate(text), state + ", " + evaluatorName + ", " + text);
    }

    // JSON.parse defines an own property, whatever Object.prototype has.
    const parsed = JSON.parse('{"__proto__": null}');
    shouldBe(Object.getPrototypeOf(parsed), Object.prototype, state + ", JSON.parse");
    shouldBe(ownKeys(parsed), "__proto__", state + ", JSON.parse");

    shouldBe(accessorCalls, 0, state + ", calls of the accessor");
}

check("original accessor");

Object.defineProperty(Object.prototype, "__proto__", {
    get() { accessorCalls++; return undefined; },
    set(value) { accessorCalls++; },
    configurable: true,
});
check("replaced accessor");

Object.defineProperty(Object.prototype, "__proto__", {
    get() { throw new Error("the getter of Object.prototype.__proto__ was called"); },
    set(value) { throw new Error("the setter of Object.prototype.__proto__ was called"); },
    configurable: true,
});
check("throwing accessor");

delete Object.prototype.__proto__;
check("deleted accessor");
