// ClassStaticBlockBody: it is a Syntax Error if ContainsArguments of ClassStaticBlockStatementList is true.
// ContainsArguments looks into arrow functions and stops at other functions. The parser used to check
// `arguments` only in the static block itself, not in an arrow function in it, and not as a shorthand property.

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


const argumentsMessage = "Cannot use 'arguments' as an identifier in static block.";

// `arguments` in an arrow function in a static block.
for (const script of [
    "(class { static { () => arguments; } })",
    "(class { static { () => { arguments; }; } })",
    "(class { static { () => arguments.length; } })",
    "(class { static { () => { arguments.length; }; } })",
    "(class { static { () => { return arguments[0]; }; } })",
    "(class { static { (c = arguments) => 1; } })",
    "(class { static { (x = () => arguments) => 1; } })",
    "(class { static { () => () => arguments; } })",
    "(class { static { () => { { () => { arguments; }; } }; } })",
    "(class { static { async () => arguments; } })",
    "(class { static { async () => { await arguments; }; } })",
    "(class { static { () => ({ [arguments]: 1 }); } })",
    "(class { static { () => ({ arguments }); } })",
    "(class { static { () => ({ a: 1, arguments, }); } })",
    "(class { static { () => [{ arguments }]; } })",
    "(class { static { () => { var arguments; }; } })",
    "(class { static { () => { let arguments; }; } })",
    "(class { static { () => { class C { [arguments]() {} }; }; } })",
    "(class { static { () => { class C { static { () => arguments; } }; }; } })",
    "(class { static { if (true) { () => arguments; } } })",
    "(class { static { for (const x of [() => arguments]) {} } })",
    "(class { static { label: () => arguments; } })",
    "(class { static { (() => arguments)(); } })",
    // Still rejected in the static block itself.
    "(class { static { arguments; } })",
    "(class { static { arguments.length; } })",
    "(class { static { { arguments; } } })",
    "(class { static { ({ [arguments]: 1 }); } })",
    "(class { static { ({ arguments }); } })",
]) {
    shouldThrowSyntaxError(script, argumentsMessage);
    shouldThrowSyntaxError(`function f() { ${script} }`, argumentsMessage);
    shouldThrowSyntaxError(`() => { ${script} }`, argumentsMessage);
    shouldThrowSyntaxError(`(class { static { ${script} } })`, argumentsMessage);
}

// In a pattern with a default value the expression pass fails first, with the message it gives for `({ c = arguments })` in the static block itself.
shouldThrowSyntaxError("(class { static { ({ c = arguments }) => 1; } })", "Unexpected token '='. Expected a ':' following the property name 'c'.");

// `arguments` in a class field initializer in an arrow function in a static block. The static block check comes first.
shouldThrowSyntaxError("(class { static { () => { class C { x = arguments; }; }; } })", argumentsMessage);
shouldThrowSyntaxError("(class { static { () => { class C { x = () => arguments; }; }; } })", argumentsMessage);
shouldThrowSyntaxError("(class { x = () => { class C { static { () => arguments; } }; }; })", argumentsMessage);

// A shorthand property is an IdentifierReference, so the class field initializer checks apply to it too.
shouldThrowSyntaxError("(class { x = ({ arguments }); })", "Cannot reference 'arguments' in class field initializer.");
shouldThrowSyntaxError("(class { x = () => ({ arguments }); })", "Cannot reference 'arguments' in class field initializer.");
shouldThrowSyntaxError("(class { static x = ({ arguments }); })", "Cannot reference 'arguments' in class field initializer.");
{
    let error;
    try {
        new (class { x = eval("({ arguments })"); })();
    } catch (e) {
        error = e;
    }
    shouldBe(error instanceof SyntaxError, true);
    shouldBe(error.message, "arguments is not valid in this context.");
}

// A function that is not an arrow function has its own `arguments`.
for (const script of [
    "(class { static { function f() { return arguments; } } })",
    "(class { static { function f(a = arguments) {} } })",
    "(class { static { (function () { arguments; }); } })",
    "(class { static { (function (a = arguments) {}); } })",
    "(class { static { (function* () { arguments; }); } })",
    "(class { static { (async function () { arguments; }); } })",
    "(class { static { ({ m() { arguments; } }); } })",
    "(class { static { ({ get m() { return arguments; } }); } })",
    "(class { static { (class { m() { arguments; } }); } })",
    "(class { static { (class { static m() { arguments; } }); } })",
    "(class { static { (class { constructor() { arguments; } }); } })",
    "(class { static { () => { function f() { arguments; } }; } })",
    "(class { static { () => function () { arguments; }; } })",
    "(class { static { () => function (a = arguments) {}; } })",
    "(class { static { () => { (function () { () => arguments; }); }; } })",
    "(class { static { () => ({ m() { arguments; } }); } })",
    "(class { static { () => class { m() { () => arguments; } }; } })",
    "(class { static { (a = function () { arguments; }) => 1; } })",
    "(class { static { () => this.arguments; } })",
    "(class { static { () => ({ arguments: 1 }); } })",
    "(class { static { () => ({ arguments: 1 }).arguments; } })",
    "(class { static { () => ({ arguments() {} }); } })",
    "(class { static { function f() { ({ arguments }); } } })",
    "(class { static { () => function () { ({ arguments }); }; } })",
    "(class { static { () => eval('1'); } })",
    "(class { static { () => new.target; } })",
    "(class { static { () => { return 1; }; } })",
]) {
    shouldNotThrowSyntaxError(script);
    shouldNotThrowSyntaxError(`function f() { ${script} }`);
    shouldNotThrowSyntaxError(`() => { ${script} }`);
}

// The arrow function is not rejected because it is in a class, and the class is not rejected because it is in an arrow function.
shouldNotThrowSyntaxError("(class { m() { () => arguments; } })");
shouldNotThrowSyntaxError("(class { static m() { () => arguments; } })");
shouldNotThrowSyntaxError("() => { (class { static { function f() { arguments; } } }); }");
shouldNotThrowSyntaxError("function f() { () => { (class { static { () => { function g() { arguments; } }; } }); }; }");

// The arrow function runs with the `arguments` of the function around it, so a valid form behaves as before.
{
    let seen;
    function outer() {
        class C {
            static {
                (function () { seen = (() => arguments)(); })(1, 2);
            }
        }
    }
    outer();
    shouldBe(seen.length, 2);
    shouldBe(seen[1], 2);
}

