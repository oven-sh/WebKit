//@ runBytecodeCache
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useLazyCachedExpressionInfo=0")

// An Error captures its stack as raw frames (CodeBlock + BytecodeIndex); the line/column of each frame is resolved
// later. When the code an Error's frames point at is about to be collected, the collector's end phase
// (ErrorInstance::computeErrorInfo) resolves them itself. With the functions decoded from a persistent bytecode cache
// payload the UnlinkedCodeBlock expression info is left in the payload until a position is first asked for
// (useLazyCachedExpressionInfo), so here that first decode happens inside the collector. Nothing else may have asked
// these blocks for a position before: no exception thrown through them, no .stack read, no profiler.

let expectedLines = { thrower: 0, level1: 0, level2: 0 };

function makeErrors() {
    // Each function's only "expression" use is the call / new below; their line numbers are checked at the end.
    function thrower(i) { return new Error("boom " + i); }           expectedLines.thrower = 16;
    function level1(i) { let pad = i + 1; return thrower(pad - 1); }  expectedLines.level1 = 17;
    function level2(i) { return [level1(i)][0]; }                     expectedLines.level2 = 18;
    let errors = [];
    for (let i = 0; i < 40; ++i)
        errors.push(level2(i));
    // A second family created through eval'd-at-runtime callers is not in the cache; mix cached and uncached frames.
    let viaIndirect = new Function("f", "return f(100);");
    errors.push(viaIndirect(level2));
    return errors;
}

let errors = makeErrors();
// Drop every reference to the functions so their CodeBlocks become garbage while the errors stay alive: that is what
// makes the collector materialize the stacks (Interpreter::stackTraceAsString -> StackFrame::computeLineAndColumn ->
// CodeBlock::lineColumnForBytecodeIndex -> UnlinkedCodeBlock::expressionInfo()).
makeErrors = null;
fullGC();
fullGC();
edenGC();
fullGC();

function check(error, i) {
    let stack = String(error.stack);
    for (let name of ["thrower", "level1", "level2"]) {
        let re = new RegExp("^" + name + "@.*:(\\d+):(\\d+)$", "m");
        let m = re.exec(stack);
        if (!m)
            throw new Error("frame " + name + " missing in error " + i + ": " + stack);
        let line = Number(m[1]), column = Number(m[2]);
        if (line !== expectedLines[name])
            throw new Error("frame " + name + " in error " + i + " has line " + line + ", expected " + expectedLines[name] + ": " + stack);
        if (!(column > 1))
            throw new Error("frame " + name + " in error " + i + " has column " + column + ": " + stack);
    }
}
errors.forEach(check);

// The same blocks, asked again from the mutator after the collector decoded them, agree.
let again = [], againLine;
(function recreate() {
    function thrower(i) { return new Error("again " + i); }           againLine = 57;
    again.push(thrower(0));
})();
let m = /thrower@.*:(\d+):\d+/.exec(again[0].stack);
if (!m || Number(m[1]) !== againLine)
    throw new Error("mutator-side line wrong (expected " + againLine + "): " + again[0].stack);
