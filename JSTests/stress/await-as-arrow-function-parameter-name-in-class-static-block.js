// ClassStaticBlockStatementList is parsed with [+Await]. ArrowParameters[?Await] take that, so `await` is not a
// BindingIdentifier in the parameters of an arrow function in a static block. The body of such an arrow function
// is [~Await], so `await` is an identifier there. The parser used to reject only the forms that its expression pass
// sees, `(await) => 1` and `([await]) => 1`, and accepted the name in a pattern with a default or a property, and
// as a rest parameter.

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

// The arrow function is first parsed in isArrowFunctionParameters(), in a scope that is not the static block, and the
// SourceProviderCache keeps that parse. The item says that the parameters were not parsed with [+Await], so
// parseFunctionInfo() parses the arrow function again when the static block is known.
for (const [script, message] of [
    ["(class { static { [a = (b = ({ await }) => 1)] = []; } })", awaitParameterMessage],
    ["(class { static { [a = (b = (...await) => 1)] = []; } })", awaitParameterMessage],
    ["(class { static { [a = (b = ([{ await }]) => 1)] = []; } })", awaitParameterMessage],
    ["(class { static { ({ x: (a = ({ await }) => 1) => 1 }); } })", "Cannot use 'await' as a shorthand property name in a static block."],
    ["(class { static { ({ x = 1 }, a = ({ await }) => 1) => a; } })", "Cannot use 'await' as a shorthand property name in a static block."],
]) {
    shouldThrowSyntaxError(script, message);
    shouldThrowSyntaxError(`function f() { ${script} }`, message);
    shouldThrowSyntaxError(`() => { ${script} }`, message);
}

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
