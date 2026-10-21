//@ runDefault
//@ runDefault("--useSourceProviderCache=false")
// The parser reads `async (v) => v` as a call of `async` before it sees the `=>`. The names that the arguments read
// stayed recorded as used variables of the scope around the arrow function. A function further out that declares a
// variable of one of those names then kept it in a scope object, for an arrow function that never reads it: a
// parameter is the arrow function's own binding. `(v) => v` never did this.
//
// Each text here is compiled once, so the configurations that compile early or collect all the time add nothing.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected} but got ${actual}`);
}

// "anonymous#A1b2c3:[0x1->0x2, LLIntFunctionCall, 23 (NeverInline)]": the number is the size of the bytecode.
function bytecodeSize(func) {
    const description = $vm.codeBlockFor(func);
    const match = /, (\d+)(?: \(\w+\))*\]$/.exec(description);
    if (!match)
        throw new Error(`no bytecode size in ${description}`);
    return Number(match[1]);
}

// The size of the bytecode of a function with this body. `skip` keeps the body from running.
let sourcePrefix = "";
function sizeOfFunction(body, parameter = "unused") {
    const func = new Function("skip", parameter, `${sourcePrefix}if (skip) return; ${body}`);
    func(true);
    return bytecodeSize(func);
}

// What declares the variable: the size of the bytecode of a function that does, with `code` after the declaration.
const owners = {
    "var": (name, code) => sizeOfFunction(`var ${name} = 1; ${code}`),
    "let": (name, code) => sizeOfFunction(`let ${name} = 1; ${code}`),
    "const": (name, code) => sizeOfFunction(`const ${name} = 1; ${code}`),
    "parameter": (name, code) => sizeOfFunction(code, name),
    "function declaration": (name, code) => sizeOfFunction(`function ${name}() { } ${code}`),
    "for-let loop": (name, code) => sizeOfFunction(`for (let ${name} = 0; ${name} < 1; ${name}++) { ${code} }`),
    "catch parameter": (name, code) => sizeOfFunction(`try { } catch (${name}) { ${code} }`),
};

// What holds the expression, between the declaration and it.
const holders = {
    "function": code => `function inner() { return ${code}; }`,
    "arrow function": code => `var inner = () => ${code};`,
    "method": code => `var inner = { method() { return ${code}; } };`,
    "class field": code => `class Inner { field = ${code}; }`,
    "class static block": code => `class Inner { static { ${code}; } }`,
};
const statements = code => `function inner() { ${code} }`;

// Whether the function that declares `name` keeps it in a scope object when `code` is in something it holds. That
// costs the function a create_lexical_environment and a put_to_scope, so the size of its bytecode says which it is.
function captures(name, code, { owner = owners["var"], holder = holders["function"] } = { }) {
    const size = code => owner(name, holder(code));
    const [notCaptured, captured, actual] = [size("0"), size(name), size(code)];
    if (notCaptured === captured)
        throw new Error(`the size of the bytecode does not say whether ${name} is captured`);
    if (actual !== notCaptured && actual !== captured)
        throw new Error(`${code}: ${actual} bytes of bytecode, expected ${notCaptured} (${name} is not captured) or ${captured} (it is)`);
    return actual === captured;
}

// Every place in the parameters that holds a binding.
const parameters = ["(v)", "(u, v)", "(v = 1)", "(...v)", "([v])", "([u, ...v])", "({ v })", "({ v = 1 })", "({ u: v })", "({ u: { w: [v] } })", "({ u = 1, v })", "({ ...v })"];
for (const [kind, holder] of Object.entries(holders)) {
    for (const parameter of parameters) {
        for (const arrow of [`async ${parameter} => v`, `${parameter} => v`])
            shouldBe(captures("v", arrow, { holder }), false, `${arrow} in a ${kind}`);
    }
}

// A source that is not Latin-1 has a parser of its own.
sourcePrefix = "/* \u2603 */ ";
for (const parameter of parameters)
    shouldBe(captures("v", `async ${parameter} => v`), false, `async ${parameter} => v in a source that is not Latin-1`);
for (const call of ["async(v)", "async.each(v)"])
    shouldBe(captures("v", call), true, `${call} in a source that is not Latin-1`);
sourcePrefix = "";

// Every kind of variable.
for (const [kind, owner] of Object.entries(owners)) {
    for (const arrow of ["async (v) => v", "(v) => v"])
        shouldBe(captures("v", arrow, { owner }), false, `${arrow} under a ${kind}`);
}

// Places where the text of the arrow function is parsed more than once, or not as the start of an expression.
for (const place of [
    code => `(${code});`,
    code => `for (${code}; ;) break;`,
    code => `for ([${code}]; ;) break;`,
    code => `[u = ${code}] = [];`,
    code => `({ u = ${code} } = { });`,
    code => `((u = ${code}) => u);`,
    code => `function u(w = ${code}) { }`,
    code => `class U extends (${code}) { }`,
]) {
    const holder = code => statements(place(code));
    shouldBe(captures("v", "async (v) => v", { holder }), false, place("async (v) => v"));
}

// What the arrow function reads from outside is still captured, and nothing else is.
for (const prefix of ["async ", ""]) {
    for (const parameter of ["(v = d)", "({ [d]: v })", "(v = () => d)", "({ v = d })", "(v, u = v + d)"]) {
        const arrow = `${prefix}${parameter} => v`;
        for (const [kind, holder] of Object.entries(holders)) {
            shouldBe(captures("d", arrow, { holder }), true, `d for ${arrow} in a ${kind}`);
            shouldBe(captures("v", arrow, { holder }), false, `v for ${arrow} in a ${kind}`);
        }
    }
    for (const body of ["v + d", "{ return d; }", "() => d"]) {
        shouldBe(captures("d", `${prefix}(v) => ${body}`), true, `d for ${prefix}(v) => ${body}`);
        shouldBe(captures("v", `${prefix}(v) => ${body}`), false, `v for ${prefix}(v) => ${body}`);
    }
}

// A use of the same name next to the arrow function is still a use.
for (const code of ["[v, async (v) => v]", "[async (v) => v, v]", "[v, async ({ v }) => v]", "[v, (v) => v]"]) {
    for (const [kind, holder] of Object.entries(holders))
        shouldBe(captures("v", code, { holder }), true, `${code} in a ${kind}`);
}

// `async` is also a name. A call of it uses its arguments, whatever holds the call and wherever `async` is in it.
for (const call of ["async(v)", "async (v)", "async(u, v)", "async(...v)", "async({ v })", "async(v = 1)", "async(() => v)", "async(async (u) => v)", "async\n(v)", "async?.(v)", "new async(v)"]) {
    for (const [kind, holder] of Object.entries(holders))
        shouldBe(captures("v", call, { holder }), true, `${call} in a ${kind}`);
}
for (const call of ["async(v)(0)", "async(0)(v)", "async.each(v)", "async.each(0)(v)", "async[0](v)", "async?.each(v)", "async`${0}`(v)", "new async.Queue(v)", "async(v).x = 0", "async(v) ? 0 : 1"])
    shouldBe(captures("v", call), true, call);
for (const statement of ["class U extends async(v) { }", "[async(v).x] = [0];", "for ([async(v).x] of []) ;", "for (async(v).x of []) ;", "({ u: async(v).x } = { });"])
    shouldBe(captures("v", statement, { holder: statements }), true, statement);

// Two more things that a name in the parameters did to the code around the arrow function.
shouldBe(sizeOfFunction("return () => async (arguments) => 0;"), sizeOfFunction("return () => (arguments) => 0;"), "an arguments object for a parameter named arguments");
shouldBe(sizeOfFunction("try { } catch (v) { (async (v) => v); }"), sizeOfFunction("try { } catch (v) { ((v) => v); }"), "a catch parameter that nothing reads");

// The values are what they were.
function results(promises) {
    const values = [];
    Promise.all(promises).then(resolved => { values.push(...resolved); }, error => { values.push(`rejected: ${error}`); });
    drainMicrotasks();
    return values.join();
}

function parameterShadowsOuterVariable() {
    var v = "outer";
    function inner() { return [async (v) => v, async ({ v }) => v, async (...v) => v[0], async (u, v = u + "!") => v]; }
    const [identifier, pattern, rest, defaultValue] = inner();
    return results([identifier("a"), pattern({ v: "b" }), rest("c"), defaultValue("d"), v]);
}
shouldBe(parameterShadowsOuterVariable(), "a,b,c,d!,outer", "parameters");

function useNextToParameterOfTheSameName(v) {
    function inner() { var before = v; var sibling = async (v) => v; var after = v; return `${before},${after}`; }
    return inner();
}
shouldBe(useNextToParameterOfTheSameName("outer"), "outer,outer", "a use next to the arrow function");

function defaultValueReadsOuterVariable() {
    var d = "outer", key = "k";
    function inner() { return [async (v = d) => v, async ({ [key]: v }) => v, async (v = () => d) => v()]; }
    const [defaultValue, computedKey, closure] = inner();
    d = "changed";
    return results([defaultValue(), computedKey({ k: "keyed" }), closure()]);
}
shouldBe(defaultValueReadsOuterVariable(), "changed,keyed,changed", "default values");

function callOfFunctionNamedAsync() {
    var v = "outer", u = "other";
    function async(...values) { return values.length === 1 ? values[0] : values.join("+"); }
    async.each = async;
    function inner() { return [async(v), async (v), async(u, v), async(() => v)(), async.each(v), async?.(v), new async(v) instanceof async]; }
    return inner().join();
}
shouldBe(callOfFunctionNamedAsync(), "outer,outer,other+outer,outer,outer,outer,true", "calls of a function named async");

// The body of a generator gets the arguments object of the generator only when it names `arguments`. A parameter
// of that name counted as naming it, so a direct eval in the body could read the object. The arrow function makes
// no difference now: the eval gives what it gives without it.
function argumentsInDirectEval(sibling) {
    try {
        const generator = new Function(`return function* () { ${sibling} return eval("arguments.length"); };`)();
        return String(generator(1, 2, 3).next().value);
    } catch (error) {
        return error.constructor.name;
    }
}
for (const sibling of ["var sibling = async (arguments) => 0;", "var sibling = async ({ arguments }) => 0;", "var sibling = async (...arguments) => 0;", "var sibling = (arguments) => 0;"])
    shouldBe(argumentsInDirectEval(sibling), argumentsInDirectEval(""), sibling);

// Every link after `async` is still not the start of an arrow function.
for (const source of ["async.each(v) => v", "async(u)(v) => v", "async?.(v) => v", "async[0](v) => v", "async`${0}`(v) => v", "new async(v) => v", "async\n(v) => v", "async (v)\n=> v"]) {
    let message = "no error";
    try {
        new Function(source);
    } catch (error) {
        message = String(error);
    }
    shouldBe(message, "SyntaxError: Unexpected token '=>'", source);
}
