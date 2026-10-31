//@ runDefault("--useDollarVM=1", "--useJIT=0")

// $vm.callWithTerminationAtTrapCheck(fn, n): a termination that the VM takes at the n-th trap check of a call, and
// that is withdrawn when the call ends (VM::cancelTermination(), as an embedder does for node:vm's `timeout`).
// Native code polls traps at every RETURN_IF_EXCEPTION. The interpreter polls them where a function is called and
// where a loop goes around. So a loop over n stops a call at each place where a termination can begin, in turn.
// Without the JIT: optimized code polls less, or not at all, so there the n-th check is not a fixed place.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected}, got ${actual}`);
}

function shouldThrow(fn, constructor, what) {
    try {
        fn();
    } catch (error) {
        if (error instanceof constructor)
            return;
        throw new Error(`${what}: threw ${error}`);
    }
    throw new Error(`${what}: did not throw`);
}

function nothingLeftBehind(what) {
    shouldBe($vm.hasPendingTermination(), false, `${what}: something is still pending`);
    let sum = 0;
    for (let i = 0; i < 100; ++i)
        sum += i;
    shouldBe(sum, 4950, `${what}: the code after the call`);
}

// The first trap check is before the function starts.
{
    let started = false;
    shouldBe($vm.callWithTerminationAtTrapCheck(() => { started = true; }, 1), true, "n = 1");
    shouldBe(started, false, "n = 1: the function started");
    nothingLeftBehind("n = 1");
}

// A function has more trap checks while its inline caches and profiles are empty, so a walk runs it a few times first.
// After that the n-th check of a call is the same place every time.
function walk(reset, fn, check) {
    for (let i = 0; i < 10; ++i) {
        reset();
        fn();
    }
    for (let n = 1; ; ++n) {
        if (n > 1000)
            throw new Error("the walk has no end");
        reset();
        const wasTaken = $vm.callWithTerminationAtTrapCheck(fn, n);
        nothingLeftBehind(`n = ${n}`);
        check(n, wasTaken);
        if (!wasTaken)
            return;
    }
}

// Each larger n lets the function get at least as far, and the function returns for the first n that it does not reach.
{
    let steps = 0;
    let done = false;
    function step() {
        ++steps;
    }
    function work() {
        step();
        step();
        step();
        done = true;
    }

    const stepsOfCutCalls = new Set;
    let furthest = 0;
    walk(() => {
        steps = 0;
        done = false;
    }, work, (n, wasTaken) => {
        if (steps < furthest)
            throw new Error(`n = ${n}: ${steps} steps, and ${furthest} steps with n - 1`);
        furthest = steps;
        shouldBe(done, !wasTaken, `n = ${n}: the function went to its end`);
        if (wasTaken)
            stepsOfCutCalls.add(steps);
        else
            shouldBe(steps, 3, `n = ${n}: the steps of a call that was not cut`);
    });
    // Each call of step() has a trap check of its own, so the walk stopped the function before each of them.
    for (let count = 0; count < 3; ++count)
        shouldBe(stepsOfCutCalls.has(count), true, `a call that was cut after ${count} steps`);
}

// Native code has trap checks too: some n stops the call inside Object.defineProperties().
{
    let before;
    let after;
    let insideNativeCode = 0;
    walk(() => {
        before = after = false;
    }, () => {
        before = true;
        Object.defineProperties({ }, { a: { value: 1 }, b: { value: 2 }, c: { value: 3 } });
        after = true;
    }, (n, wasTaken) => {
        if (wasTaken && before && !after)
            ++insideNativeCode;
    });
    if (!insideNativeCode)
        throw new Error("no trap check stopped the call inside native code");
}

// Script cannot catch the termination.
{
    let caught = false;
    let finalized = false;
    const wasTaken = $vm.callWithTerminationAtTrapCheck(() => {
        try {
            for (;;) { }
        } catch {
            caught = true;
        } finally {
            finalized = true;
        }
    }, 10);
    shouldBe(wasTaken, true, "an endless loop");
    shouldBe(caught, false, "an endless loop: catch");
    shouldBe(finalized, false, "an endless loop: finally");
    nothingLeftBehind("an endless loop");
}

// A count that the call does not reach is dropped with the request: the next termination does not wait for it.
{
    shouldBe($vm.callWithTerminationAtTrapCheck(() => 1, 0x7fffffff), false, "a count that is not reached");
    nothingLeftBehind("a count that is not reached");
    let timedOut = false;
    try {
        $vm.callWithTimeLimit(() => { for (;;) { } }, 10);
    } catch (error) {
        if (!(error instanceof RangeError) || error.message != "timed out")
            throw error;
        timedOut = true;
    }
    shouldBe(timedOut, true, "a time limit after a count that was not reached");
    nothingLeftBehind("a time limit after a count that was not reached");
}

// Inside a time-limited call, the limit ends a loop of calls: the termination that a call withdraws is requested again
// where the limit has passed.
{
    let timedOut = false;
    try {
        $vm.callWithTimeLimit(() => {
            for (;;)
                $vm.callWithTerminationAtTrapCheck(() => { }, 1);
        }, 10);
    } catch (error) {
        if (!(error instanceof RangeError) || error.message != "timed out")
            throw error;
        timedOut = true;
    }
    shouldBe(timedOut, true, "a time limit around a loop of calls");
    nothingLeftBehind("a time limit around a loop of calls");
}

// A call inside the function of another call is refused: the count of the outer call would be lost.
{
    let error;
    const wasTaken = $vm.callWithTerminationAtTrapCheck(() => {
        try {
            $vm.callWithTerminationAtTrapCheck(() => { }, 1);
        } catch (e) {
            error = e;
        }
    }, 1000);
    shouldBe(wasTaken, false, "the outer call of two");
    shouldBe(error instanceof TypeError, true, "the inner call of two threw a TypeError");
    nothingLeftBehind("a call inside a call");
}

// What the function throws by itself stays thrown.
shouldThrow(() => $vm.callWithTerminationAtTrapCheck(() => { throw new SyntaxError("mine"); }, 1000), SyntaxError, "a function that throws");
nothingLeftBehind("a function that throws");

shouldThrow(() => $vm.callWithTerminationAtTrapCheck(1, 1), TypeError, "not a function");
shouldThrow(() => $vm.callWithTerminationAtTrapCheck(() => { }, 0), RangeError, "n = 0");
shouldThrow(() => $vm.callWithTerminationAtTrapCheck(() => { }, NaN), RangeError, "n = NaN");
shouldThrow(() => $vm.callWithTerminationAtTrapCheck(() => { }), RangeError, "no n");
nothingLeftBehind("calls that throw");
