//@ defaultRun
//@ runDefault("--useAOT=1") if $aot

// A failed op_check_tdz names the binding that the check guards: for every kind of expression that reads or writes a
// name, in every kind of frame, and in every tier. The check carries the name (BytecodeGenerator::emitTDZCheck) and
// slow_path_check_tdz reads it from there. Upstream's slow_path_check_tdz cuts the name out of the source text at the
// expression info of a frame that a stack walk finds. A sync that puts that text back makes this test fail.
// The run with --useAOT=1 is the interpreter as a compiled program has it: a read of a captured variable and its check
// are one step there (LowLevelInterpreterAOT.asm), and a variable in its dead zone leaves that step for the check.

function messageOf(run) {
    try {
        run();
    } catch (error) {
        return String(error);
    }
    return "no error";
}

function tdz(name) {
    return `ReferenceError: Cannot access '${name}' before initialization.`;
}

const failures = new Set;
function check(label, actual, expected) {
    if (actual !== expected)
        failures.add(`${label}: expected ${expected} but got ${actual}`);
}

function noise() { }

// Each entry is the body of a function, behind a call that has nothing to do with `x`. The comment names the function
// of bytecompiler/NodesCodegen.cpp that emits the check.
const expressions = {
    "read": "x;", // ResolveNode
    "read in a return": "return x;",
    "read in parentheses": "(x);",
    "shorthand property": "({ x });",
    "member": "x.y;",
    "member call": "x.y();",
    "member store": "x.y = 1;",
    "index": "x[0];",
    "spread": "[...x];",
    "template literal": "`${x}`;",
    "binary operand": "1 + x;",
    "construct": "new x;",
    "construct with arguments": "new x(1);",
    "call": "x();", // FunctionCallResolveNode
    "call with arguments": "x(1, 2);",
    "tagged template": "x`a`;", // TaggedTemplateNode
    "typeof": "typeof x;", // TypeOfResolveNode
    "typeof in parentheses": "typeof (x);",
    "postfix increment": "x++;", // PostfixNode
    "postfix decrement": "x--;",
    "prefix increment": "++x;", // PrefixNode
    "prefix decrement": "--x;",
    "compound assignment": "x += 1;", // ReadModifyResolveNode
    "compound assignment, other operators": "x -= 1; x *= 2; x **= 2; x <<= 1;",
    "logical or assignment": "x ||= 1;", // ShortCircuitReadModifyResolveNode
    "logical and assignment": "x &&= 1;",
    "nullish assignment": "x ??= 1;",
    "assignment": "x = 1;", // AssignResolveNode
    "array pattern": "[x] = [1];", // AssignmentElementNode
    "array pattern with a default": "[x = 1] = [];",
    "array pattern with a rest": "[...x] = [1];",
    "object pattern with a key": "({ a: x } = { a: 1 });",
    "object pattern with a rest": "({ ...x } = {});",
    "nested pattern": "({ a: [x] } = { a: [1] });",
    "object pattern": "({ x } = { x: 1 });", // BindingNode
    "object pattern with a default": "({ x = 1 } = {});",
    "direct eval": "eval('x');",
    "direct eval that writes": "eval('x++');",
};

for (const [label, expression] of Object.entries(expressions)) {
    // `x` is a register of the function that reads it, or a variable of an enclosing function. The bytecode generator
    // emits the check in a different way for each.
    const asLocal = (0, eval)(`(function (noise) { noise(0); ${expression} let x; })`);
    const asCaptured = (0, eval)(`(function (noise) { (() => { noise(0); ${expression} })(); let x; })`);
    check(`${label}, local`, messageOf(() => asLocal(noise)), tdz("x"));
    check(`${label}, captured`, messageOf(() => asCaptured(noise)), tdz("x"));
}

// EvalFunctionCallNode: the callee of a direct eval is itself a binding.
check("a binding named eval", messageOf(() => { (0, eval)("(function () { eval('1'); let eval = 1; })")(); }), tdz("eval"));

// The name is the binding's, however the source spells it.
check("escaped first character", messageOf(() => { \u{61}bc; let abc; }), tdz("abc"));
check("escaped middle character", messageOf(() => { a\u0062c; let abc; }), tdz("abc"));
check("escaped write", messageOf(() => { a\u0062c++; let abc; }), tdz("abc"));
check("dollar and underscore", messageOf(() => { $_$; let $_$; }), tdz("$_$"));
check("Latin-1 name", messageOf(() => { café; let café; }), tdz("café"));
check("name outside Latin-1", messageOf(() => { 名前; let 名前; }), tdz("名前"));
check("name outside Latin-1, write", messageOf(() => { 名前 += 1; let 名前; }), tdz("名前"));
check("long name", messageOf((0, eval)(`(function () { ${"n".repeat(5000)}; let ${"n".repeat(5000)}; })`)), tdz("n".repeat(5000)));
check("the name of another binding is not used", messageOf(() => { let other = 0; if (other) noise(other); later++; let later; }), tdz("later"));

// Other bindings with a dead zone.
check("class read in its heritage", messageOf(() => { class C extends C { } }), tdz("C"));
check("class read in a computed key", messageOf(() => { class C { [C] = 1; } }), tdz("C"));
check("class used above its declaration", messageOf(() => { new C; class C { } }), tdz("C"));
check("parameter default reads a later parameter", messageOf(() => { (function (a = b, b) { })(); }), tdz("b"));
check("parameter default reads its own parameter", messageOf(() => { (function (a = a) { })(); }), tdz("a"));
check("generator parameter default", messageOf(() => { (function* (a = b, b) { })(); }), tdz("b"));
check("initializer reads its own binding", messageOf(() => { let x = x; }), tdz("x"));
check("const, read", messageOf(() => { noise(0); x; const x = 1; }), tdz("x"));
check("const, write", messageOf(() => { noise(0); x = 2; const x = 1; }), tdz("x"));
check("generator body, write", messageOf(() => { (function* () { noise(0); x++; let x; })().next(); }), tdz("x"));
check("async function body, write", settle((async function () { await null; noise(0); x++; let x; })()), tdz("x"));
check("switch case", messageOf(() => { switch (0) { case 0: x; case 1: let x; } }), tdz("x"));
check("loop head", messageOf(() => { for (let i = j, j = 0; ;) { } }), tdz("j"));
check("catch parameter pattern", messageOf(() => { try { throw {}; } catch ({ a = b, b }) { } }), tdz("b"));
check("function declared above the binding", messageOf(() => { read(); function read() { return x; } let x; }), tdz("x"));
check("global lexical binding", messageOf(() => { (0, eval)("globalLexical; let globalLexical;"); }), tdz("globalLexical"));

// The operand of the check is the index of the name plus one. These checks stand behind 246 other names, so their operands
// lie on both sides of 0xff: the narrow and the wide form of the instruction are both decoded. loadString() and not
// eval, so that the run with the bytecode cache writes the function and reads it back.
{
    const accesses = ["wide0;", "wide1++;", "wide2 += 1;", "[wide3] = [1];", "typeof wide4;", "wide5();", "wide6 = 1;", "({ wide7 } = {});",
        "(() => wide8)();", "(() => { wide9++; })();", "(() => { wide10 ||= 1; })();", "(() => { [wide11] = [1]; })();",
        "wide12`a`;", "--wide13;", "new wide14;", "wide15.x;"];
    let source = "function tdzWideOperands(o, messages) {";
    for (let i = 0; i < 246; ++i)
        source += `o.p${i};`;
    for (const access of accesses)
        source += `try { ${access} } catch (error) { messages.push(String(error)); }`;
    source += `let ${accesses.map((access, i) => `wide${i}`).join(", ")}; }`;
    loadString(source);
    const messages = [];
    tdzWideOperands({ }, messages);
    check("operands on both sides of 0xff", JSON.stringify(messages), JSON.stringify(accesses.map((access, i) => tdz(`wide${i}`))));
}

// `this` in a derived constructor has its own message.
const thisMessage = "ReferenceError: 'super()' must be called in derived constructor before accessing |this| or returning non-object.";
check("this before super()", messageOf(() => { new (class extends Object { constructor() { this.x = 1; super(); } }); }), thisMessage);
check("this before super() in an arrow function", messageOf(() => { new (class extends Object { constructor() { (() => this)(); super(); } }); }), thisMessage);
check("super property before super()", messageOf(() => { new (class extends Object { constructor() { super.x; super(); } }); }), thisMessage);

// Frames that a stack trace hides. The check runs in a function that the engine makes: the function that runs the
// field initializers of a class, and the outer half of an async function that awaits. A stack walk that starts at such
// a frame finds the caller's frame, or none.
check("field initializer", messageOf(() => { class X { name = foo; } new X; const foo = 1; }), tdz("foo"));
check("field initializer, constructor", messageOf(() => { class X { name = foo; constructor() { this.a = 1; } } new X; const foo = 1; }), tdz("foo"));
check("field initializer, derived class", messageOf(() => { class B { } class X extends B { name = foo; } new X; const foo = 1; }), tdz("foo"));
check("field initializer, derived class with a constructor", messageOf(() => { class B { } class X extends B { name = foo; constructor() { super(); } } new X; const foo = 1; }), tdz("foo"));
check("field initializer, Reflect.construct", messageOf(() => { class X { name = foo; } Reflect.construct(X, []); const foo = 1; }), tdz("foo"));
check("field initializer, class expression", messageOf(() => { new (class { name = foo; }); const foo = 1; }), tdz("foo"));
check("second field initializer", messageOf(() => { class X { a = 1; b = this.a + foo; } new X; const foo = 1; }), tdz("foo"));
check("private field initializer", messageOf(() => { class X { #p = foo; } new X; const foo = 1; }), tdz("foo"));
check("field initializer that calls", messageOf(() => { class X { name = foo(); } new X; const foo = () => 1; }), tdz("foo"));
check("field initializer that writes", messageOf(() => { class X { name = foo++; } new X; let foo = 1; }), tdz("foo"));
check("field initializer with typeof", messageOf(() => { class X { name = typeof (foo); } new X; const foo = 1; }), tdz("foo"));
check("field initializer, arrow function", messageOf(() => { class X { name = () => foo; } new X().name(); const foo = 1; }), tdz("foo"));
check("field initializer, direct eval", messageOf(() => { class X { name = eval("foo"); } new X; const foo = 1; }), tdz("foo"));
check("static field initializer", messageOf(() => { class X { static name = foo; } const foo = 1; }), tdz("foo"));
check("static field initializer reads a later class", messageOf(() => { class X { static other = new Y; } class Y { } }), tdz("Y"));
check("static block", messageOf(() => { class X { static { foo; } } const foo = 1; }), tdz("foo"));
check("field initializer at the top level of eval code", messageOf(() => { (0, eval)("class EvalX { name = evalFoo; } new EvalX; const evalFoo = 1;"); }), tdz("evalFoo"));
check("field initializer in new Function", messageOf(new Function("class X { name = foo; } new X; const foo = 1;")), tdz("foo"));

function settle(promise) {
    let outcome = "pending";
    promise.then(() => { outcome = "fulfilled"; }, error => { outcome = String(error); });
    drainMicrotasks();
    return outcome;
}

{
    async function awaits(a = b, b) { await null; }
    async function doesNotAwait(a = b, b) { }
    const arrow = async (a = b, b) => { await null; };
    const object = { async method(a = b, b) { await null; } };
    class K { async method(a = b, b) { await null; } static async method(a = b, b) { await null; } }
    async function* generator(a = b, b) { await null; }
    async function self(a = a) { await null; }
    async function writes(a = b++, b) { await null; }
    function caller() { return awaits(); }

    for (let i = 0; i < 2; ++i) {
        check("async function that awaits", settle(awaits()), tdz("b"));
        check("async function that awaits, through a caller", settle(caller()), tdz("b"));
        check("async function that awaits, through Reflect.apply", settle(Reflect.apply(awaits, undefined, [])), tdz("b"));
        check("async function that awaits, through Array.prototype.map", settle([undefined].map(awaits)[0]), tdz("b"));
        check("async function that does not await", settle(doesNotAwait()), tdz("b"));
        check("async arrow function", settle(arrow()), tdz("b"));
        check("async method", settle(object.method()), tdz("b"));
        check("async class method", settle(new K().method()), tdz("b"));
        check("static async class method", settle(K.method()), tdz("b"));
        check("async function, own parameter", settle(self()), tdz("a"));
        check("async function, write", settle(writes()), tdz("b"));
        check("async generator", messageOf(() => generator()), tdz("b"));
    }
}

// With no JavaScript caller below the hidden frame, the stack walk found no frame, and slow_path_check_tdz crashed.
asyncTestStart(2);
Promise.resolve().then(async (first = second, second) => { await null; }).then(
    () => { throw new Error("the async arrow function did not reject"); },
    error => {
        if (String(error) !== tdz("second"))
            throw new Error(`promise reaction: expected ${tdz("second")} but got ${String(error)}`);
        asyncTestPassed();
    });
setTimeout(() => {
    const message = messageOf(() => { class X { name = foo; } new X; const foo = 1; });
    if (message !== tdz("foo"))
        throw new Error(`timer: expected ${tdz("foo")} but got ${message}`);
    asyncTestPassed();
}, 0);

// Every tier. The check passes while the function warms up, and fails in the code of the tier that it reached.
{
    function readsLocal(early) {
        if (early)
            return value;
        let value = 1;
        return value;
    }
    noInline(readsLocal);

    function writesLocal(early) {
        if (early)
            value++;
        let value = 1;
        return value;
    }
    noInline(writesLocal);

    function readsCaptured(early) {
        const read = () => value;
        if (early)
            return read();
        let value = 1;
        return read();
    }
    noInline(readsCaptured);

    let ready = true;
    class Fields {
        field = ready ? 1 : later;
    }
    function constructs() {
        return new Fields;
    }
    noInline(constructs);

    for (let i = 0; i < testLoopCount; ++i) {
        readsLocal(false);
        writesLocal(false);
        readsCaptured(false);
        constructs();
    }
    ready = false;

    for (let i = 0; i < 20; ++i) {
        check("compiled, local read", messageOf(() => readsLocal(true)), tdz("value"));
        check("compiled, local write", messageOf(() => writesLocal(true)), tdz("value"));
        check("compiled, captured read", messageOf(() => readsCaptured(true)), tdz("value"));
        check("compiled, field initializer inlined in its constructor", messageOf(constructs), tdz("later"));
    }
    const later = 1;
}

if (failures.size)
    throw new Error(`${failures.size} wrong messages:\n${[...failures].join("\n")}`);
