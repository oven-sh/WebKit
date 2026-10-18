//@ runDefault("--useDollarVM=1", "--useConcurrentJIT=0")

// A function of a global object with a debugger has debug hooks: op_debug pauses in the function's own frame. The DFG
// turns an inlined op_debug into nothing, and a breakpoint only throws away the optimized code of the function it is
// in, not the code of a caller that has inlined that function. So a function with debug hooks is not inlined, whoever
// the caller is. Three callers have code without the debugger's mark and would inline it:
// - code of another global object that was linked before the debugger attached,
// - the same when that global object generates the caller first,
// - the body of a generator that was suspended when the debugger attached.
//
// Each callee below has a `debugger` statement, which keeps it from tiering up, as a breakpoint does. So
// $vm.dfgTrue() inside it is true only when it runs inlined in a caller's optimized code.

const calls = testLoopCount;
const otherRealm = createGlobalObject();
globalThis.isOptimized = $vm.dfgTrue;

// (An arrow function: with the debug hooks it is still small enough for the DFG to inline.)
function makeCallee(name) {
    const callee = new Function("return x => { debugger; return x + isOptimized(); }")();
    globalThis[name] = otherRealm[name] = callee;
    return callee;
}
const callerSource = name => "return function caller(n) { /* " + name + " */ let sum = 0; for (let i = 0; i < n; i++) sum += " + name + "(i); return sum; }";

// Made before the debugger attaches: both global objects link the caller's one unlinked function.
makeCallee("sharedFirst");
makeCallee("sharedLater");
makeCallee("fromBody");
const mineFirst = new Function(callerSource("sharedFirst"))();
const theirsFirst = new otherRealm.Function(callerSource("sharedFirst"))();
const theirsLater = new otherRealm.Function(callerSource("sharedLater"))();
const mineLater = new Function(callerSource("sharedLater"))();
const body = (function* (n) { let sum = 0; yield; for (let i = 0; i < n; i++) sum += fromBody(i); yield sum; })(calls);
body.next(); // started in code without the debug hooks

$vm.enableDebuggerModeWhenIdle(); // attaches a debugger to this realm only, and deletes all code

setTimeout(() => {
    const expected = calls * (calls - 1) / 2; // every call in the callee's own frame: isOptimized() is 0
    const failures = [];
    const check = (what, sum) => {
        if (sum !== expected)
            failures.push(what + ": " + (sum - expected) + " of " + calls + " calls ran inlined");
    };

    mineFirst(5);
    check("a caller that the debugged global object generated first, run by the other one", theirsFirst(calls));
    check("a caller that the other global object generated first", theirsLater(calls));
    mineLater(5);
    check("the body of a generator that was suspended when the debugger attached", body.next().value);

    if (failures.length)
        throw new Error(failures.join("\n"));
}, 0);
