// ClassStaticBlockBody: it is a Syntax Error if ContainsArguments of ClassStaticBlockStatementList is true.
// ContainsArguments looks into arrow functions and stops at other functions. The parser used to check
// `arguments` only in the static block itself, not in an arrow function in it.
//
// ClassStaticBlockStatementList is parsed with [+Await]. ArrowParameters[?Await] take that, so `await`
// is not a BindingIdentifier in the parameters of an arrow function in a static block. The body of such
// an arrow function is [~Await], so `await` is an identifier there. The parser used to reject only the
// forms that its expression pass sees, `(await) => 1` and `([await]) => 1`, and accepted the names in a
// pattern with a default or a property, and a rest parameter.

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

// `await` as the name of a parameter of an arrow function in a static block.
const awaitParameterMessage = "Cannot use 'await' as a parameter name in a static block.";
const awaitReferenceMessage = "The 'await' keyword is disallowed in the IdentifierReference position within static block.";

for (const script of [
    "(class { static { ({ await }) => 1; } })",
    "(class { static { ({ await = 1 }) => 1; } })",
    "(class { static { ({ a: { await } }) => 1; } })",
    "(class { static { ([{ await }]) => 1; } })",
    "(class { static { (...await) => 1; } })",
    "(class { static { (a, ...await) => 1; } })",
    "(class { static { (a, { await }) => 1; } })",
    "(class { static { { ({ await }) => 1; } } })",
    "(class { static { if (true) ({ await }) => 1; } })",
]) {
    shouldThrowSyntaxError(script, awaitParameterMessage);
    shouldThrowSyntaxError(`function f() { ${script} }`, awaitParameterMessage);
    shouldThrowSyntaxError(`async function f() { ${script} }`, awaitParameterMessage);
    shouldThrowSyntaxError(`(class { static { ${script} } })`, awaitParameterMessage);
}

// An async arrow function reserves `await` itself.
shouldThrowSyntaxError("(class { static { async ({ await }) => 1; } })", "Cannot use 'await' as a parameter name in an async function.");

// Forms that the expression pass of the parser already rejected, with the message of that pass.
for (const script of [
    "(class { static { (await) => 1; } })",
    "(class { static { ([await]) => 1; } })",
    "(class { static { (a = await) => 1; } })",
    "(class { static { ({ a: await }) => 1; } })",
    "(class { static { ({ ...await }) => 1; } })",
    "(class { static { ([, await]) => 1; } })",
    "(class { static { ([...await]) => 1; } })",
    "(class { static { async (await) => 1; } })",
    "(class { static { async (...await) => 1; } })",
])
    shouldThrowSyntaxError(script, awaitReferenceMessage);
shouldThrowSyntaxError("(class { static { await => 1; } })", "Unexpected identifier 'await'. Cannot use 'await' within static block.");

// The rest parameter message names the reason, as the other parameter messages do.
shouldThrowSyntaxError("async function f(...await) {}", "Cannot use 'await' as a parameter name in an async function.");
shouldThrowSyntaxError("async function f() { (...await) => 1; }", "Cannot use 'await' as a parameter name in an async function.");
shouldThrowSyntaxError("async function f() { ({ await }) => 1; }", "Cannot use 'await' as a parameter name in an async function.");

// `await` is an identifier in the body of an arrow function in a static block, and in the parameters of an
// arrow function nested in that body, because ConciseBody is [~Await].
for (const script of [
    "(class { static { () => await; } })",
    "(class { static { () => { await; }; } })",
    "(class { static { () => { var await; }; } })",
    "(class { static { () => { let await; }; } })",
    "(class { static { () => ({ await }); } })",
    "(class { static { () => { class await {} }; } })",
    "(class { static { () => { function await() {} }; } })",
    "(class { static { () => { try {} catch (await) {} }; } })",
    "(class { static { () => { try {} catch ({ await }) {} }; } })",
    "(class { static { () => { await: 1; }; } })",
    "(class { static { () => (await) => 1; } })",
    "(class { static { () => ({ await }) => 1; } })",
    "(class { static { () => (...await) => 1; } })",
    "(class { static { () => { ({ await }) => 1; }; } })",
    "(class { static { () => function (await) {}; } })",
    "(class { static { () => function ({ await }) {}; } })",
    "(class { static { (a = function ({ await }) {}) => 1; } })",
    "(class { static { (a = function (...await) {}) => 1; } })",
    "(class { static { (a = { await: 1 }) => 1; } })",
    "(class { static { (a = { await: 1 }.await) => 1; } })",
    "(class { static { function f({ await }) {} } })",
    "(class { static { function f(...await) {} } })",
]) {
    shouldNotThrowSyntaxError(script);
    shouldNotThrowSyntaxError(`function f() { ${script} }`);
    shouldNotThrowSyntaxError(`() => { ${script} }`);
}

{
    let result;
    class C {
        static {
            result = (() => { var await = "body"; return ((await) => await)(await); })();
        }
    }
    shouldBe(result, "body");
}

// An async function or a static block in the parameters does not change the reason for the function around it.
shouldThrowSyntaxError("async function f(a = (class { static { ({ await }) => 1; } })) {}", "Cannot use 'await' as a shorthand property name in a static block.");
shouldThrowSyntaxError("(class { static { (a = async function ({ await }) {}) => 1; } })", "Cannot use 'await' as a parameter name in an async function.");
shouldThrowSyntaxError("(class { static { () => { async function f({ await }) {} }; } })", "Cannot use 'await' as a parameter name in an async function.");
