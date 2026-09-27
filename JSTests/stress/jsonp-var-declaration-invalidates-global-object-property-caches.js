// A program of the form `var x = <JSON>;` runs on the JSONP fast path of Interpreter::executeProgram, without
// bytecode. The variable it declares is a new own property of the global object, so what inline caches and
// optimized code recorded about the global object before the declaration must not be used after it.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + expected + " but got " + actual);
}

// The same declarations as a program that runs as bytecode give the same results.
const forms = [
    ["JSONP", ""],
    ["bytecode", " void 0;"],
];

for (const [form, suffix] of forms) {
    // One realm per case: after the first declaration the global object is a dictionary that was flattened
    // before, and caches give up on it. A second case in the same realm would not reach a cached site.
    function test(name, setup, declaration, expected) {
        const realm = $262.createRealm();
        const access = realm.evalScript(setup + `
            noInline(access);
            for (let i = 0; i < ${testLoopCount}; ++i)
                access(i);
            access;
        `);
        realm.evalScript(declaration + suffix);
        shouldBe(String(access(7)), expected, form + ", " + name);
    }

    // A name that no object has.
    test("get_by_id miss", `function access() { return globalThis.name1; }`, `var name1 = 33;`, "33");
    test("get_by_id miss, this", `function access() { return this.name1; }`, `var name1 = 33;`, "33");
    test("get_by_val, constant key", `const key = "name1"; function access() { return globalThis[key]; }`, `var name1 = 33;`, "33");
    test("get_by_id_with_this", `var home = { __proto__: globalThis, method() { return super.name1; } }; function access() { return home.method(); }`, `var name1 = 33;`, "33");
    test("get_by_val_with_this", `function access() { return Reflect.get(globalThis, "name1", globalThis); }`, `var name1 = 33;`, "33");
    test("object value", `function access() { return globalThis.name1?.a; }`, `var name1 = {"a":1};`, "1");
    test("two declarations", `function access() { return globalThis.name2; }`, `var name1 = 1; var name2 = [2];`, "2");

    // A name that the prototype chain of the global object has.
    test("prototype data property", `function access() { return typeof globalThis.toString; }`, `var toString = 33;`, "number");
    test("prototype getter", `Object.defineProperty(Object.prototype, "name1", { get() { return "getter"; }, configurable: true });
        function access() { return globalThis.name1; }`, `var name1 = 33;`, "33");
    test("prototype method call", `function access() { try { return globalThis.valueOf() === globalThis; } catch (e) { return e.name; } }`, `var valueOf = 33;`, "TypeError");
    test("prototype setter", `var calls = 0; Object.defineProperty(Object.prototype, "name1", { set(v) { calls++; }, get() { return "getter"; }, configurable: true });
        function access(v) { let before = calls; globalThis.name1 = v; return calls - before + ":" + globalThis.name1; }`, `var name1 = 33;`, "0:7");

    // An identifier resolves against the global object itself, not against the JSGlobalProxy.
    test("identifier read", `function access() { return typeof valueOf; }`, `var valueOf = 33;`, "number");
    test("identifier write", `var calls = 0; Object.defineProperty(Object.prototype, "name1", { set(v) { calls++; }, get() { return "getter"; }, configurable: true });
        function access(v) { let before = calls; name1 = v; return calls - before; }`, `var name1 = 33;`, "0");
    test("identifier delete", `function access() { return delete name1; }`, `var name1 = 33;`, "false");

    // The cache is created between two statements of the program that declares the variable.
    test("cache created by an earlier statement", `function read() { return globalThis.name2; } noInline(read);
        var holder = { set warm(v) { for (let i = 0; i < ${testLoopCount}; ++i) read(); } };
        function access() { return read(); }`, `var name1 = 1; holder.warm = 1; var name2 = 2;`, "2");

    // The reader is on the stack, in optimized code, when the program runs.
    {
        const realm = $262.createRealm();
        const result = realm.evalScript(`
            function read() { return globalThis.name1; }
            function loop(count) {
                let seen = [];
                for (let i = 0; i < count; ++i) {
                    if (i === count - 2)
                        $262.evalScript("var name1 = 33;${suffix}");
                    let value = read();
                    if (i >= count - 3)
                        seen.push(String(value));
                }
                return seen.join();
            }
            noInline(loop);
            loop(${testLoopCount});
        `);
        shouldBe(result, "undefined,33,33", form + ", reader on the stack");
    }

    // The same name again: no second binding, the value is assigned, the descriptor stays.
    {
        const realm = $262.createRealm();
        const access = realm.evalScript(`
            function access() { return globalThis.name1; }
            noInline(access);
            for (let i = 0; i < ${testLoopCount}; ++i)
                access();
            access;
        `);
        realm.evalScript(`var name1 = 1;` + suffix);
        shouldBe(access(), 1, form + ", first run");
        realm.evalScript(`var name1 = 2;` + suffix);
        shouldBe(access(), 2, form + ", second run");
        const descriptor = Object.getOwnPropertyDescriptor(realm.global, "name1");
        shouldBe(JSON.stringify(descriptor), `{"value":2,"writable":true,"enumerable":true,"configurable":false}`, form + ", descriptor");
    }
}

// On the JSONP fast path only a new name gives the global object another Structure. A `var` of a name
// that exists creates no binding, and the caches that are valid stay.
{
    const global = $vm.createGlobalObject();
    // The list has five entries for each Structure. The first of the last five is the ID of the current one.
    const structureID = () => $vm.getStructureTransitionList(global).at(-5);
    global.$vm.evaluateWithScopeExtension(`var name1 = 1;`);
    $vm.flattenDictionaryObject(global);
    const flattened = structureID();
    global.$vm.evaluateWithScopeExtension(`var name1 = 2;`);
    shouldBe(structureID(), flattened, "same name: same Structure");
    shouldBe(global.name1, 2, "same name: value");
    global.$vm.evaluateWithScopeExtension(`var name2 = 3;`);
    shouldBe(structureID() !== flattened, true, "new name: another Structure");
    shouldBe(global.name2, 3, "new name: value");
}
