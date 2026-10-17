// A module's body runs once. The code linked for it (ModuleProgramCodeBlock) is released when the body has finished,
// not kept, through the functions the module created, until its TTL has passed in some later full collection; a
// suspended top-level-await body is not finished, and resumes fine whether or not a collection took its code meanwhile.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

function liveCells(className) {
    const snapshot = generateHeapSnapshot(); // collects first
    const index = snapshot.nodeClassNames.indexOf(className);
    if (index < 0)
        return 0;
    let count = 0;
    for (let i = 2; i < snapshot.nodes.length; i += 4) {
        if (snapshot.nodes[i] === index)
            ++count;
    }
    return count;
}

let releaseGate;
globalThis.moduleCodeReleasedGate = new Promise((resolve) => { releaseGate = resolve; });

// Code that left the interpreter is not released on the spot (a compiler thread may be looking at it); it ages out.
const options = jscOptions();
const staysInterpreted = options.useLLInt && options.thresholdForJITAfterWarmUp >= 100 && options.thresholdForJITSoon >= 100;

async function test() {
    assert(liveCells("ModuleProgramCodeBlock") === 0, "no module code before the first import");

    const entry = await import("./resources/module-code-released/entry.js");
    if (staysInterpreted) {
        const found = liveCells("ModuleProgramCodeBlock");
        assert(found === 0, "entry.js and dep.js let go of their linked code, found " + found);
    }

    // Everything the module created keeps working without it.
    assert(entry.sum() === 30, "sum");
    assert(entry.arrow(1) === 17, "arrow");
    assert(entry.Point.origin.norm === 0 && new entry.Point(3, 4).norm === 5, "class");
    assert([...entry.numbers()].length === 16, "generator");
    assert(entry.readCounter() === 1, "live binding before");
    assert(entry.bumpTwice() === 3 && entry.readCounter() === 3, "live binding after");
    assert(entry.sameTemplateObject(), "template object identity");
    assert(typeof entry.meta === "object", "import.meta");
    const again = await import("./resources/module-code-released/entry.js");
    assert(again === entry && entry.readCounter() === 3, "a second import does not evaluate again");

    // Suspended at its first await: the code stays; it goes once the body has run to its end.
    const pending = import("./resources/module-code-released/tla.js");
    while (!globalThis.moduleCodeReleasedStarted)
        await new Promise((resolve) => setTimeout(resolve, 1));
    // (Unless a collection took it for the old code it also is; then resuming links it again.)
    const suspendedCode = liveCells("ModuleProgramCodeBlock");
    if (!options.useEagerCodeBlockJettisonTiming && !options.forceCodeBlockToJettisonDueToOldAge)
        assert(staysInterpreted ? suspendedCode === 1 : suspendedCode >= 1, "the suspended module keeps its code, found " + suspendedCode);
    else
        assert(!staysInterpreted || suspendedCode <= 1, "only the suspended module may still have code, found " + suspendedCode);
    releaseGate();
    const tla = await pending;
    assert(tla.stage === "done:0123" && tla.later() === "done:0123", "top-level await result " + tla.stage);
    // (Asked of the executable, which is what this is about; whether a collection right now finds the CodeBlock
    // unreachable is a different question.)
    if (staysInterpreted)
        assert($vm.codeBlockCensus().moduleExecutablesWithLinkedCode === 0, "the finished top-level-await module let go of its code");

    let error;
    try {
        await import("./resources/module-code-released/throws.js");
    } catch (e) {
        error = e;
    }
    assert(error && error.message === "thrown by the module body", "module body exception");
    let secondError;
    try {
        await import("./resources/module-code-released/throws.js");
    } catch (e) {
        secondError = e;
    }
    assert(secondError === error, "the evaluation error is remembered");
}

test().then(() => { }, (e) => { print("FAIL: " + e + "\n" + e.stack); $vm.abort(); });
