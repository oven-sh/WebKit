// FieldDefinition: it is a Syntax Error if ContainsArguments of Initializer is true, or if Initializer Contains SuperCall.
// Both rules look into an arrow function and stop at any other function. The parser used to stop only at the body of
// such a function, so `arguments` and `super()` in its parameters were a SyntaxError.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected: ${String(expected)}`);
}

function shouldThrowSyntaxError(script, message) {
    let error;
    try {
        (0, eval)(script);
    } catch (e) {
        error = e;
    }
    if (!(error instanceof SyntaxError))
        throw new Error(`Expected SyntaxError for: ${script}` + (error ? `, but got ${error}` : ", but nothing was thrown"));
    if (error.message !== message)
        throw new Error(`Expected "${message}" for: ${script}, but got "${error.message}"`);
}

function shouldNotThrowSyntaxError(script) {
    try {
        (0, eval)(script);
    } catch (e) {
        if (e instanceof SyntaxError)
            throw new Error(`Unexpected SyntaxError for: ${script}: ${e.message}`);
        throw e;
    }
}

// [text before the expression in the field initializer, text after it]
const fields = [
    ["(class { f = (", "); })"],
    ["(class { static f = (", "); })"],
    ["(class { #f = (", "); })"],
    ["(class { static #f = (", "); })"],
    ["(class { ['f'] = (", "); })"],
    ["(class extends Object { f = (", "); })"],
    // A class field initializer in another one.
    ["(class { g = class { f = (", "); }; })"],
    // The expression is in an arrow function in the initializer.
    ["(class { f = () => (", "); })"],
    ["(class { f = (a = (", ")) => a; })"],
    ["(class { f = async () => { await (", "); }; })"],
];
// Only for valid code: `arguments` in an initializer in a class static block gets the error of the static block.
const fieldInStaticBlock = ["(class { static { class C { f = (", "); } } })"];

// `arguments` in the parameters of a function that is not an arrow function.
const functionsWithOwnArguments = [
    "function (p = arguments) { }",
    "function (p = arguments.length) { }",
    "function (p = arguments[0]) { }",
    "function (p = () => arguments) { }",
    "function (p = () => () => arguments.length) { }",
    "function ({ p = arguments }) { }",
    "function ([p = arguments]) { }",
    "function ({ [arguments.length]: p }) { }",
    "function (...[p = arguments]) { }",
    "function (a, b = a, c = arguments.length) { }",
    "function (p = `${arguments.length}`) { }",
    "function (p = { [arguments.length]: 1 }) { }",
    "function (p = typeof arguments) { }",
    "function named(p = arguments) { }",
    "function* (p = arguments) { }",
    "async function (p = arguments) { }",
    "async function* (p = arguments) { }",
    "{ m(p = arguments) { } }",
    "{ *m(p = arguments) { } }",
    "{ async m(p = arguments) { } }",
    "{ async *m(p = arguments) { } }",
    "{ set m(p = arguments) { } }",
    "{ set m({ p = arguments }) { } }",
    "{ ['m'](p = arguments) { } }",
    "class { constructor(p = arguments) { } }",
    "class extends Object { constructor(p = arguments) { super(); } }",
    "class { m(p = arguments) { } }",
    "class { static m(p = arguments) { } }",
    "class { #m(p = arguments) { } }",
    "class { static #m(p = arguments) { } }",
    "class { *m(p = arguments) { } }",
    "class { async m(p = arguments) { } }",
    "class { set m(p = arguments) { } }",
    "class { static set m(p = arguments) { } }",
    // A function in the parameters of another function.
    "function (p = function (q = arguments) { }) { }",
    "function (p = { m(q = arguments) { } }) { }",
    "function (p = () => function (q = arguments) { }) { }",
    // The parameters belong to the function, and so does a class heritage or a computed key in them.
    "function (p = class extends arguments { }) { }",
    "function (p = class { [arguments]() { } }) { }",
    "function (p = class { [arguments] = 1; }) { }",
    "function (p = class { static [arguments.length] = 1; }) { }",
    // The body was valid before too.
    "function () { arguments; }",
    "function (p = arguments) { arguments; () => arguments; }",
];

for (const [before, after] of [...fields, fieldInStaticBlock]) {
    for (const expression of functionsWithOwnArguments)
        shouldNotThrowSyntaxError(before + expression + after);
}

// `super()` in the parameters of the constructor of a derived class.
for (const [before, after] of [...fields, fieldInStaticBlock]) {
    for (const expression of [
        "class extends Object { constructor(p = super()) { } }",
        "class extends Object { constructor(p = () => super()) { p(); } }",
        "class extends Object { constructor(p = super(), q = () => super()) { } }",
        "class extends Object { constructor({ p = super() }) { } }",
        "class extends Object { constructor(p = class extends super() { }) { } }",
        "class extends Object { constructor(p = class { [super()]() { } }) { } }",
        "class extends Object { constructor(p = { [super()]: 1 }) { } }",
        "class extends Object { constructor(p = super()) { } static m(q = arguments) { } }",
    ])
        shouldNotThrowSyntaxError(before + expression + after);
}

// Still a SyntaxError: the initializer itself, an arrow function, a computed key and a class heritage are part of the initializer.
const argumentsMessage = "Unexpected identifier 'arguments'. Cannot reference 'arguments' in class field initializer.";
const superCallMessage = "Unexpected token '('. super call is not valid in class field initializer context.";
const superMessage = "super is not valid in this context.";

for (const [before, after] of fields) {
    for (const expression of [
        "arguments",
        "arguments.length",
        "() => arguments",
        "(p = arguments) => p",
        "(p = arguments.length) => p",
        "([p = arguments]) => p",
        "(p = () => arguments) => p",
        "async (p = arguments) => p",
        "async () => arguments",
        "{ [arguments]() { } }",
        "{ [arguments.length]: function (p) { } }",
        "{ get [arguments]() { return 1; } }",
        "class { [arguments]() { } }",
        "class { static [arguments.length](p) { } }",
        "class extends arguments { }",
        "class extends (() => arguments) { }",
        // A class field initializer in the parameters is an initializer again.
        "function (p = class { g = arguments; }) { }",
        "function (p = class { g = () => arguments; }) { }",
        "function (p = class { static g = arguments.length; }) { }",
        "function (p = class { g = (q = arguments) => q; }) { }",
        "{ m(p = class { g = arguments; }) { } }",
        "function () { class C { g = arguments; } }",
    ])
        shouldThrowSyntaxError(before + expression + after, argumentsMessage);

    shouldThrowSyntaxError(before + "function (p = class { static { arguments; } }) { }" + after, "Cannot use 'arguments' as an identifier in static block.");
    // The parser reads "({ p = arguments })" as an expression first, and that fails before it gets to `arguments`.
    shouldThrowSyntaxError(before + "({ p = arguments }) => p" + after, "Unexpected token '='. Expected a ':' following the property name 'p'.");
}

for (const [before, after] of [
    ["(class extends Object { f = (", "); })"],
    ["(class extends Object { static f = (", "); })"],
    ["(class extends Object { constructor() { super(); } f = (", "); })"],
    ["(class extends Object { constructor(p = class extends Object { f = (", "); }) { super(); } })"],
]) {
    for (const expression of [
        "super()",
        "() => super()",
        "(p = super()) => p",
        "{ [super()]: 1 }",
        "class extends super() { }",
        "class { [super()]() { } }",
        "class extends Object { constructor(p = class { g = super(); }) { } }",
        "class extends Object { constructor(p = class { g = () => super(); }) { } }",
    ])
        shouldThrowSyntaxError(before + expression + after, superCallMessage);

    // Only the constructor of a derived class can call super().
    for (const expression of [
        "function (p = super()) { }",
        "function (p = () => super()) { }",
        "{ m(p = super()) { } }",
        "{ set m(p = super()) { } }",
        "class { constructor(p = super()) { } }",
        "class extends Object { m(p = super()) { } }",
        "class extends Object { static m(p = super()) { } }",
        "class extends Object { set m(p = super()) { } }",
        "class extends Object { constructor(p = function (q = super()) { }) { } }",
        "class extends Object { constructor(p = function () { super(); }) { } }",
    ])
        shouldThrowSyntaxError(before + expression + after, superMessage);
}

// `yield` and `await` in these parameters are what they are without the class field.
shouldThrowSyntaxError("(function* () { class C { f = function (p = yield) { }; } })", "Unexpected keyword 'yield'. Cannot use yield expression out of generator.");
shouldThrowSyntaxError("(function* () { class C { f = function* (p = yield) { }; } })", "Unexpected keyword 'yield'. Cannot use yield expression within parameters.");
shouldThrowSyntaxError("(function* () { class C { f = { *m(p = yield) { } }; } })", "Unexpected keyword 'yield'. Cannot use yield expression within parameters.");
shouldThrowSyntaxError("(function* () { class C { f = (p = yield) => p; } })", "Unexpected keyword 'yield'. Cannot use yield expression inside class field initializer expression.");
shouldThrowSyntaxError("(async function () { class C { f = async function (p = await 1) { }; } })", "Cannot use 'await' within a parameter default expression.");
shouldNotThrowSyntaxError("(async function () { class C { f = function (p = await) { }; } })");

// The values.
class A {
    length = function (p = arguments.length) { return p; };
    object = function (p = arguments) { return p === arguments; };
    arrow = function (p = () => arguments.length) { return p(); };
    pattern = function ({ p = arguments.length }, q) { return p; };
    rest = function (...[p = arguments.length]) { return p; };
    method = { m(p = arguments.length) { return p; } };
    setter = { set m(p = arguments.length) { this.value = p; } };
    static staticField = function (p = arguments[1]) { return p; };
    #privateField = function (p = arguments.length) { return p; };
    callPrivateField() { return this.#privateField(undefined, 1, 2, 3); }
    generator = function* (p = arguments.length) { yield p; };
    asyncFunction = async function (p = arguments.length) { return p; };
    inArrow = () => function (p = arguments.length) { return p; };
    nested = new (class { constructor(p = arguments.length) { this.p = p; } })(undefined, 1);
    nestedFunction = function (p = function (q = arguments.length) { return q; }) { return p(undefined, 1, 2) + arguments.length; };
    newTarget = function (p = new.target, q = () => new.target) { return [p, q()]; };
    evalInParameters = function (p = eval("arguments.length"), q = () => eval("arguments.length")) { return p + q(); };
    evalInInitializer = eval("(function (p = arguments.length) { return p; })");
}

let asyncResult;
for (let i = 0; i < testLoopCount; ++i) {
    const a = new A();
    shouldBe(a.length(), 0);
    shouldBe(a.length(undefined, 2), 2);
    shouldBe(a.length(7, 2), 7);
    shouldBe(a.object(), true);
    shouldBe(a.arrow(undefined, 1, 2), 3);
    shouldBe(a.pattern({}, 1), 2);
    shouldBe(a.pattern({ p: 5 }), 5);
    shouldBe(a.rest(), 0);
    shouldBe(a.rest(undefined, 1), 2);
    shouldBe(a.method.m(undefined, 1, 2, 3), 4);
    a.setter.m = undefined;
    shouldBe(a.setter.value, 1);
    shouldBe(A.staticField(undefined, "second"), "second");
    shouldBe(a.callPrivateField(), 4);
    shouldBe(a.generator(undefined, 1).next().value, 2);
    shouldBe(a.inArrow()(undefined, 1, 2), 3);
    shouldBe(a.nested.p, 2);
    shouldBe(a.nestedFunction(undefined, 1), 5);
    shouldBe(a.newTarget()[0], undefined);
    shouldBe(a.newTarget()[1], undefined);
    shouldBe(new a.newTarget()[0], a.newTarget);
    shouldBe(new a.newTarget()[1], a.newTarget);
    shouldBe(a.evalInParameters(undefined, undefined, 1), 6);
    shouldBe(a.evalInInitializer(undefined, 1, 2), 3);
    if (!i)
        a.asyncFunction(undefined, 1, 2).then(value => { asyncResult = value; });
}
drainMicrotasks();
shouldBe(asyncResult, 3);

class Base {
    constructor(value) { this.value = value; }
}
class B {
    Derived = class extends Base {
        constructor(p = super("parameters"), q = this.value) {
            shouldBe(p, this);
            this.q = q;
        }
    };
    DerivedWithArrow = class extends Base {
        constructor(p = () => super("arrow")) {
            shouldBe(p(), this);
        }
    };
    static Derived = class extends Base {
        constructor(p = arguments.length, q = super(p)) { }
    };
}

for (let i = 0; i < testLoopCount; ++i) {
    const b = new B();
    const derived = new b.Derived();
    shouldBe(derived.value, "parameters");
    shouldBe(derived.q, "parameters");
    shouldBe(new b.DerivedWithArrow().value, "arrow");
    shouldBe(new B.Derived(undefined, undefined, 2).value, 3);
}
