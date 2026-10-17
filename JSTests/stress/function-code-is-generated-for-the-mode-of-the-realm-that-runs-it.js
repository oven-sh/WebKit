//@ runDefault("--useDollarVM=1")
//@ runDefault("--useDollarVM=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=200")
//@ runBytecodeCache("--useDollarVM=1")

// Two realms of one VM share the unlinked executable of a function: both ask the CodeCache for the same source while
// neither has a debugger, so both get the same one. A debugger then attaches to one of them. From then on that realm's
// code has to be generated with the debug hooks, and with every variable captured in the scope
// (BytecodeGenerator::shouldEmitDebugHooks), and the other realm's code has to be generated without them.
//
// The code of one realm must never be handed to the other. A generator or an async function that is suspended keeps its
// registers in a generator frame and resumes in the code that suspended it, which is why the mode of a resumable body
// is pinned the first time its code is generated (ScriptExecutable::newCodeBlockFor): code of the other mode looks for
// its variables somewhere else. And a function whose code was generated for the realm without the debugger carries no
// op_debug, so a breakpoint in the debugged realm would never be reached.

const source = "return function* gen(tag) { let n = 0; while (true) { const got = yield tag + n; n += got; } }";
const otherRealm = createGlobalObject();
const failures = [];

function check(what, actual, expected) {
    if (actual !== expected)
        failures.push(what + " is " + actual + ", expected " + expected);
}

// The realm with the debugger and the realm without it, each with a generator of the shared function.
const debugged = new Function(source)()("d");
const plain = new otherRealm.Function(source)()("p");
// A plain function of the same shape, to see which mode its code is generated with.
const debuggedCounter = new Function("return function counter(by) { let n = by; n += by; return n; }")();
const plainCounter = new otherRealm.Function("return function counter(by) { let n = by; n += by; return n; }")();

// Suspended at its first yield, in code generated before any debugger, so its body is pinned to that mode.
check("plain.next()", plain.next().value, "p0");

$vm.enableDebuggerModeWhenIdle(); // attaches a debugger to this realm only, and deletes all code

setTimeout(() => {
    // The debugged realm generates the body first and keeps it. The other realm, which asks for the mode it was
    // suspended in, must not be given that code.
    check("debugged.next() with the debugger on", debugged.next().value, "d0");
    check("plain.next(5) with the debugger on", plain.next(5).value, "p5");

    // The same for a function that is not resumable: the realm without the debugger generates the code first, and the
    // debugged realm still gets code that a breakpoint can be set in.
    check("plainCounter(2)", plainCounter(2), 4);
    check("plainCounter has no debug hooks", $vm.wasCompiledWithDebuggerHooks(plainCounter), false);
    check("debuggedCounter(3)", debuggedCounter(3), 6);
    check("debuggedCounter has debug hooks", $vm.wasCompiledWithDebuggerHooks(debuggedCounter), true);

    $vm.disableDebuggerModeWhenIdle(); // deletes all code again, and this realm asks for no debug hooks from now on

    setTimeout(() => {
        // Now the other way around: the realm without the debugger resumes first, and the one whose body is pinned to
        // the mode with the debug hooks follows.
        check("plain.next(1) with the debugger off", plain.next(1).value, "p6");
        check("debugged.next(2) with the debugger off", debugged.next(2).value, "d2");
        check("debuggedCounter(4) with the debugger off", debuggedCounter(4), 8);

        if (failures.length)
            throw new Error(failures.join("; "));
    }, 0);
}, 0);
