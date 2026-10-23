//@ defaultRun; run("bytecode-optimizer", "--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1"); run("debugger-bytecode", "--forceDebuggerBytecodeGeneration=true"); run("profilers", "--useTypeProfiler=true", "--useControlFlowProfiler=true")

// A sloppy direct eval in a parameter expression declares its vars in the environment of the called function, outside
// the environment of the parameters (ECMA-262 FunctionDeclarationInstantiation, step 20). This file runs that in code
// that tiers up. direct-eval-in-parameter-expression-var-scope.js has every kind of function and of eval.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + "\n  expected: " + expected + "\n  actual:   " + actual);
}

// Most calls do not run the eval: they must not see the var of a call that did.
function hotFunction(declare, a = declare && eval("var x = 1"), b = () => typeof x) {
    return b();
}
var hotArrow = (declare, a = declare && eval("var x = 1"), b = () => typeof x) => b();
for (let i = 0; i < testLoopCount; i++) {
    const declare = !(i % 16);
    const expected = declare ? "number" : "undefined";
    shouldBe(hotFunction(declare), expected, "hotFunction");
    shouldBe(hotArrow(declare), expected, "hotArrow");
}
shouldBe(typeof x, "undefined", "a global x after the hot functions");

// A closure of the parameters that was compiled before any eval declared a var. It read the bindings around the
// function, and the JIT can have that in its code. The eval has to take it away. A new realm has seen no eval yet.
for (const host of [
    `function host(read = () => x + y + z, warmed = warm(read), declared = eval("var x = 10, y = 20, z = 30")) { return read(); }`,
    `var host = (read = () => x + y + z, warmed = warm(read), declared = eval("var x = 10, y = 20, z = 30")) => read();`,
]) {
    const realm = runString(`
        var x = 1;
        let y = 2;
        function warm(read) {
            for (let i = 0; i < ${testLoopCount}; i++) {
                if (read() !== 6)
                    throw new Error("before the eval: " + read());
            }
        }
        function enclosing() {
            var z = 3;
            ${host}
            return [host(), x, y, z];
        }
        var result = String(enclosing());
    `);
    shouldBe(realm.result, "60,1,2,3", host);
}
