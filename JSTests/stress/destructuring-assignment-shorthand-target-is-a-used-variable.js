// A shorthand property of an object assignment pattern names a variable that the code uses, like every other
// assignment target. The parser recorded that only for a target with a default value (`{ v = 1 }`). For the others
// it relied on the parse of the same text as an object literal, and that parse does not always read the property:
//   - the head of a for-in, for-of or for-await-of statement is parsed as a pattern at once, and so is an object
//     pattern nested in such a head;
//   - the literal parse stops at the first property with a default value, so for `({ w = 1, v } = o)` it never
//     reads `v`. It stops at a second `__proto__` key too.
// A function whose only mention of the variable was such a target did not capture it. It wrote a global variable
// in sloppy code, and threw a ReferenceError in strict code. When a scope further out had a captured variable of the
// same name, it wrote that one, in strict code too. A catch parameter that had no other mention was dropped, with
// the same result and no function in between.
//
// destructuring-assignment-shorthand-target-is-a-used-variable-in-every-closure-kind.js has every spelling of the
// pattern in every kind of function.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${String(expected)} but got ${String(actual)}`);
}

function shouldThrow(func, errorType, what) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error(`${what}: expected a ${errorType.name} but got ${String(error)}`);
}

// A function that a bundler wraps around the statements of a module has these two shapes.
function forOfHead() {
    var i = "old";
    var f = function () { for ({ i } of [{ i: "new" }]) ; };
    f();
    return i;
}
shouldBe(forOfHead(), "new", "for-of head");
shouldBe("i" in globalThis, false, "for-of head");

function afterDefaultValue() {
    let width, height;
    function setOptions(options) { ({ width = 100, height } = options); }
    setOptions({ height: 5 });
    return `${width}/${height}`;
}
shouldBe(afterDefaultValue(), "100/5", "shorthand property after a default value");
shouldBe("height" in globalThis, false, "shorthand property after a default value");

// A captured variable of the same name further out is not the target.
function underCapturedVariable() {
    let shadowed = "outer";
    const read = () => shadowed;
    function inner() {
        let shadowed = "inner";
        (() => { for ({ shadowed } of [{ shadowed: "new" }]) ; })();
        return shadowed;
    }
    return `${inner()}/${read()}`;
}
shouldBe(underCapturedVariable(), "new/outer", "a variable under a captured variable of the same name");

function underCapturedParameterInStrictCode(shadowedParameter) {
    "use strict";
    const read = () => shadowedParameter;
    function inner(shadowedParameter) {
        let w;
        (() => { ({ w = 1, shadowedParameter } = { shadowedParameter: "new" }); })();
        return shadowedParameter;
    }
    return `${inner("inner")}/${read()}`;
}
shouldBe(underCapturedParameterInStrictCode("outer"), "new/outer", "a parameter under a captured parameter of the same name");

// The pattern is in a default value of a parameter of the function.
function inParameterDefaultValue() {
    let inDefaultValue = "init", w;
    ((p = ({ w = 1, inDefaultValue } = { inDefaultValue: "new" })) => p)();
    return inDefaultValue;
}
shouldBe(inParameterDefaultValue(), "new", "assignment in a parameter default value");
shouldBe("inDefaultValue" in globalThis, false, "assignment in a parameter default value");

// The head of a for-await-of statement.
let forAwaitResult;
async function forAwaitHead() {
    let awaited = "init";
    await (async () => { for await ({ awaited } of [{ awaited: "new" }]) ; })();
    return awaited;
}
forAwaitHead().then(value => { forAwaitResult = value; }, error => { forAwaitResult = error; });
drainMicrotasks();
shouldBe(forAwaitResult, "new", "for-await-of head");
shouldBe("awaited" in globalThis, false, "for-await-of head");

// A catch parameter with no other mention is still the target. No function is in between here. A read of the
// parameter would be another mention, so the checks are that no global variable appears and that nothing throws.
function catchParameter() {
    try {
        throw "init";
    } catch (caught) {
        for ({ caught } of [{ caught: "new" }]) ;
    }
}
catchParameter();
shouldBe("caught" in globalThis, false, "a catch parameter");

function catchParameterInStrictCode() {
    "use strict";
    let w;
    try {
        throw "init";
    } catch (caughtInStrictCode) {
        ({ w = 1, caughtInStrictCode } = { caughtInStrictCode: "new" });
    }
}
catchParameterInStrictCode();

// The target is the variable, so what the variable is decides what the assignment does.
shouldThrow(function () {
    const constant = "init";
    (() => { for ({ constant } of [{ constant: "new" }]) ; })();
}, TypeError, "a const target");
shouldBe("constant" in globalThis, false, "a const target");

shouldThrow(function () {
    (() => { for ({ uninitialized } of [{ uninitialized: "new" }]) ; })();
    let uninitialized;
}, ReferenceError, "a target in its temporal dead zone");
shouldBe("uninitialized" in globalThis, false, "a target in its temporal dead zone");

shouldThrow(function () {
    class Named {
        static assign() { (() => { for ({ Named } of [{ Named: "new" }]) ; })(); }
    }
    Named.assign();
}, TypeError, "a class name target");

const namedFunctionExpression = function ownName() {
    (() => { for ({ ownName } of [{ ownName: "new" }]) ; })();
    return typeof ownName;
};
shouldBe(namedFunctionExpression(), "function", "the name of a function expression as the target");
shouldBe("ownName" in globalThis, false, "the name of a function expression as the target");

shouldThrow(function ownNameInStrictCode() {
    "use strict";
    (() => { for ({ ownNameInStrictCode } of [{ ownNameInStrictCode: "new" }]) ; })();
}, TypeError, "the name of a function expression as the target in strict code");

// The write is a write to a closure variable in every tier.
function everyTier() {
    let tiered = 0;
    const assign = value => { for ({ tiered } of [{ tiered: value }]) ; };
    noInline(assign);
    for (let i = 1; i <= testLoopCount; ++i) {
        assign(i);
        if (tiered !== i)
            throw new Error(`every tier: expected ${i} but got ${tiered}`);
    }
}
everyTier();
shouldBe("tiered" in globalThis, false, "every tier");

// `arguments` as such a target is the arguments object of the function, and the function has to create it. These
// functions mention `arguments` nowhere else.
const globalArgumentsDescriptor = JSON.stringify(Object.getOwnPropertyDescriptor(globalThis, "arguments"));
function argumentsIsNotGlobal(what) {
    shouldBe(JSON.stringify(Object.getOwnPropertyDescriptor(globalThis, "arguments")), globalArgumentsDescriptor, what);
}

function argumentsInForOfHead() {
    for ({ arguments } of [{ arguments: "new" }]) ;
}
argumentsInForOfHead("old");
argumentsIsNotGlobal("arguments in a for-of head");

function argumentsWithDefaultValue() {
    for ({ arguments = "new" } of [{ }]) ;
}
argumentsWithDefaultValue("old");
argumentsIsNotGlobal("arguments with a default value");

function argumentsAfterDefaultValue() {
    let w;
    ({ w = 1, arguments } = { arguments: "new" });
}
argumentsAfterDefaultValue("old");
argumentsIsNotGlobal("arguments after a default value");

// The body of a generator or of an async function is a function of its own, with the arguments object of its wrapper.
function* argumentsInForOfHeadOfGenerator() {
    for ({ arguments } of [{ arguments: "new" }]) ;
}
argumentsInForOfHeadOfGenerator("old").next();
argumentsIsNotGlobal("arguments in a for-of head of a generator");

async function argumentsAfterDefaultValueInAsyncFunction() {
    let w;
    ({ w = 1, arguments } = { arguments: "new" });
}
argumentsAfterDefaultValueInAsyncFunction("old");
drainMicrotasks();
argumentsIsNotGlobal("arguments after a default value in an async function");

// In an arrow function it is the arguments object of the enclosing function.
function argumentsInForOfHeadOfArrowFunction() {
    (() => { for ({ arguments } of [{ arguments: "new" }]) ; })();
    return arguments;
}
shouldBe(argumentsInForOfHeadOfArrowFunction("old"), "new", "arguments in a for-of head of an arrow function");
argumentsIsNotGlobal("arguments in a for-of head of an arrow function");

function argumentsAfterDefaultValueInArrowFunction() {
    let w;
    (() => { ({ w = 1, arguments } = { arguments: "new" }); })();
    return arguments;
}
shouldBe(argumentsAfterDefaultValueInArrowFunction("old"), "new", "arguments after a default value in an arrow function");
argumentsIsNotGlobal("arguments after a default value in an arrow function");

function* argumentsInForOfHeadOfArrowFunctionInGenerator() {
    (() => { for ({ arguments } of [{ arguments: "new" }]) ; })();
    yield arguments;
}
shouldBe(argumentsInForOfHeadOfArrowFunctionInGenerator("old").next().value, "new", "arguments in a for-of head of an arrow function in a generator");
argumentsIsNotGlobal("arguments in a for-of head of an arrow function in a generator");

// A variable named `eval` is a variable like the others. The global function stays what it was.
const globalEval = globalThis.eval;
(function (eval) {
    (() => { for ({ eval } of [{ eval: "new" }]) ; })();
})("init");
const evalIsNotGlobal = globalThis.eval === globalEval;
globalThis.eval = globalEval;
shouldBe(evalIsNotGlobal, true, "a parameter named eval");

// Strict code still rejects these two names as targets.
for (const name of ["arguments", "eval"]) {
    shouldThrow(() => new Function(`"use strict"; for ({ ${name} } of [{ }]) ;`), SyntaxError, `${name} in a for-of head in strict code`);
    shouldThrow(() => new Function(`"use strict"; let w; ({ w = 1, ${name} } = { });`), SyntaxError, `${name} after a default value in strict code`);
}

// An object literal in the same places is no pattern, and its properties are no targets.
function objectLiteralInForOfHead() {
    let a = "a", b = "b";
    const log = [];
    (() => { for ({ a, b, set x(value) { log.push(a + b + value); } }.x of [1, 2]) ; })();
    return `${a}/${b}/${log}`;
}
shouldBe(objectLiteralInForOfHead(), "a/b/ab1,ab2", "an object literal in a for-of head");

function objectLiteralInArrayPattern() {
    let a = "a", b = "b";
    const log = [];
    (() => { [{ a, b, set c(value) { log.push(a + b + value); } }.c] = [1]; })();
    return `${a}/${b}/${log}`;
}
shouldBe(objectLiteralInArrayPattern(), "a/b/ab1", "an object literal in an array pattern");

function assignmentWithoutDefaultValue() {
    let plain = "init";
    (() => { ({ plain } = { plain: "new" }); })();
    return plain;
}
shouldBe(assignmentWithoutDefaultValue(), "new", "assignment without a default value");

// A method named `arguments` of an object literal in a for-of head is no use of the arguments object. The parser
// reads the name as a shorthand target first, and drops that parse at the `(`. If it counted as a use, the
// closures that `make` returns would keep `options` alive.
function make(options, list) {
    const host = { };
    const worker = () => { for ({ arguments() { } }.x of list) ; };
    return { worker, inner };
    function inner() { return host; }
}
noInline(make);

const references = [];
let current = make({ previous: null }, []);
for (let i = 0; i < 128; i++) {
    const options = { previous: current, payload: new Uint8Array(1024) };
    references.push(new WeakRef(options));
    current = make(options, []);
    current.worker();
}

// `w` is the target with a default value in the patterns above.
shouldBe("w" in globalThis, false, "the target with a default value");

// A WeakRef keeps its target alive until the end of the current job, so look from a later task.
setTimeout(() => {
    gc();
    gc();
    let alive = 0;
    for (const reference of references) {
        if (reference.deref())
            alive++;
    }
    if (alive > references.length / 4)
        throw new Error(`a method named arguments: ${alive} of ${references.length} options objects are kept alive`);
}, 0);
