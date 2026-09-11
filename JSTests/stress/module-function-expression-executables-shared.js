//@ requireOptions("--useSharedModuleFunctionExpressionExecutables=1")

// With useSharedModuleFunctionExpressionExecutables the FunctionExecutables of a module's top-level function expressions
// belong to the ModuleProgramExecutable, so code linked for the module a second time (here: a top-level-await body whose
// CodeBlock was collected while it was suspended) creates closures that share code with the ones created before.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

const open = [];
globalThis.moduleSharedFunctionExpressionsGates = [0, 1].map((i) => new Promise((resolve) => { open[i] = resolve; }));

async function test() {
    const pending = import("./resources/module-shared-function-expressions/tla.js");
    while (globalThis.moduleSharedFunctionExpressionsRound !== 0)
        await new Promise((resolve) => setTimeout(resolve, 1));
    // The suspended body ran once: cold. These collections take its CodeBlock (unless a mode keeps CodeBlocks alive).
    fullGC();
    fullGC();
    open[0]();
    while (globalThis.moduleSharedFunctionExpressionsRound !== 1)
        await new Promise((resolve) => setTimeout(resolve, 1));
    open[1]();
    const { makers } = await pending;
    assert(makers.length === 2 && makers[0](5) === 10 && makers[1](5) === 11, "closures work");
    for (let i = 0; i < 20; ++i) {
        makers[0](i);
        makers[1](i);
    }
    // One executable, so one CodeBlock at any time (compared at one time: an eager tier-up may replace it in between).
    const first = $vm.codeBlockFor(makers[0]), second = $vm.codeBlockFor(makers[1]);
    const address = (description) => /0x[0-9a-f]+/.exec(description)[0];
    const options = jscOptions();
    if (options.useLLInt && options.thresholdForJITAfterWarmUp >= 100 && options.thresholdForJITSoon >= 100)
        assert(address(first) === address(second), "one CodeBlock for both closures: " + first + " vs " + second);
}

test().then(() => { }, (e) => { print("FAIL: " + e + "\n" + e.stack); $vm.abort(); });
