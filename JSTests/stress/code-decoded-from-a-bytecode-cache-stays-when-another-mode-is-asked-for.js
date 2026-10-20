//@ runDefault("--useDollarVM=1")
//@ runDefault("--useDollarVM=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=200")

// As function-code-is-generated-for-the-mode-of-the-realm-that-runs-it.js, for functions whose code is decoded from a
// bytecode cache that was built ahead of time, with the bytecode optimizer, the way an embedder builds one. Two realms
// link the one decoded program, and a debugger then attaches to one of them.
//
// The realm with the debugger asks for code with the debug hooks, which the payload does not have, so that code is
// generated for it. It must not take the place of the decoded code: a generator that is suspended in the decoded code
// resumes in it, and the optimizer's code keeps other registers in the generator frame than code generated from the
// source does (the loop below has an iterator and a pending call alive across its yields). Code generated again for the
// other realm would read its for-of iterator out of the wrong slot.

const source = `
    var base = 100;
    function sink(a, b, c) { return a + ":" + b + ":" + c; }
    function* gen(tag) {
        let n = 0;
        const values = [1, 2, 3, 4, 5, 6];
        for (const v of values) {
            const got = yield sink(tag, n + base, v);
            n += got + base - base;
            const again = yield sink(base, (yield tag + v), n);
            n += again;
        }
    }
    function counter(by) { let n = by; n += by; return n; }
    ({ gen, counter })
`;

const failures = [];
function check(what, actual, expected) {
    if (actual !== expected)
        failures.push(what + " is " + actual + ", expected " + expected);
}

// The same decoded program in this realm, which gets the debugger, and in a fresh one.
const [debugged, plain] = evalTwiceFromPersistentBytecodeCache(source);
const debuggedGenerator = debugged.gen("d"); // made, not started
const plainGenerator = plain.gen("p");
check("plainGenerator.next()", plainGenerator.next().value, "p:100:1");
check("plainGenerator.next(5)", plainGenerator.next(5).value, "p1"); // suspended inside the pending call, in decoded code

$vm.enableDebuggerModeWhenIdle(); // attaches a debugger to this realm only, and deletes all code

setTimeout(() => {
    // This realm asks for the mode with the debug hooks: code is generated for it, and the decoded code stays.
    check("debuggedGenerator.next()", debuggedGenerator.next().value, "d:100:1");
    check("plainGenerator.next(7)", plainGenerator.next(7).value, "100:7:5");
    check("plainGenerator.next(9)", plainGenerator.next(9).value, "p:114:2");
    check("plainGenerator.next(11)", plainGenerator.next(11).value, "p2");
    check("debuggedGenerator.next(3)", debuggedGenerator.next(3).value, "d1");

    // A function that is not resumable: this realm's code has the debug hooks whichever realm runs it first, and the
    // other realm still runs the decoded code.
    check("plain.counter(2)", plain.counter(2), 4);
    check("plain.counter has no debug hooks", $vm.wasCompiledWithDebuggerHooks(plain.counter), false);
    check("debugged.counter(3)", debugged.counter(3), 6);
    check("debugged.counter has debug hooks", $vm.wasCompiledWithDebuggerHooks(debugged.counter), true);
    check("plain.counter(4)", plain.counter(4), 8);
    check("plain.counter still has no debug hooks", $vm.wasCompiledWithDebuggerHooks(plain.counter), false);

    $vm.disableDebuggerModeWhenIdle(); // deletes all code again

    setTimeout(() => {
        // The generator that started in code generated for the debugger resumes in code generated for it again, and the
        // other one in the decoded code.
        check("debuggedGenerator.next(4) with the debugger off", debuggedGenerator.next(4).value, "100:4:3");
        check("plainGenerator.next(13) with the debugger off", plainGenerator.next(13).value, "100:13:25");
        check("debugged.counter(5) with the debugger off", debugged.counter(5), 10);
        check("debugged.counter has no debug hooks with the debugger off", $vm.wasCompiledWithDebuggerHooks(debugged.counter), false);

        if (failures.length)
            throw new Error(failures.join("; "));
    }, 0);
}, 0);
