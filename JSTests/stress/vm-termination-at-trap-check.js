//@ runDefault("--useDollarVM=1", "--useJIT=0")

// $vm.callWithTerminationAtTrapCheck(fn, n): the n-th trap check of a call requests a termination and takes it. The
// termination is withdrawn when the call ends (VM::cancelTermination(), as an embedder does for node:vm's `timeout`).
// Native code polls traps at every RETURN_IF_EXCEPTION. The interpreter polls them where a function is called and
// where a loop goes around. So a loop over n stops a call at each of its trap checks in turn.
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

function timesOut(fn, ms) {
    try {
        $vm.callWithTimeLimit(fn, ms);
    } catch (error) {
        if (!(error instanceof RangeError) || error.message != "timed out")
            throw error;
        return true;
    }
    return false;
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
// After that the n-th check of a call is the same place, until a garbage collection empties a cache again.
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

// Each trap check counts once. One turn of a loop is one trap check in the interpreter, so n + 1 stops a loop one turn
// later than n.
{
    let turns;
    const turnsOfCalls = [];
    for (let n = 20; n < 40; ++n) {
        turns = 0;
        shouldBe($vm.callWithTerminationAtTrapCheck(() => { for (;;) ++turns; }, n), true, `an endless loop, n = ${n}`);
        turnsOfCalls.push(turns);
    }
    for (let i = 1; i < turnsOfCalls.length; ++i)
        shouldBe(turnsOfCalls[i], turnsOfCalls[i - 1] + 1, `the turns of an endless loop for n = ${20 + i}, with ${turnsOfCalls[i - 1]} for n - 1`);
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

// Before the n-th check no termination is pending, as when a timer thread requests one at that moment. The function
// sees none, and Atomics.wait() goes to its timeout.
{
    const cell = new Int32Array(new SharedArrayBuffer(4));
    let pending;
    let result;
    let cutBehindTheWait = 0;
    function step() {
    }
    walk(() => {
        pending = undefined;
        result = undefined;
    }, () => {
        pending = $vm.hasPendingTermination();
        result = Atomics.wait(cell, 0, 0, 1);
        step();
    }, (n, wasTaken) => {
        if (pending !== undefined)
            shouldBe(pending, false, `n = ${n}: a termination is pending inside the function`);
        // A termination that is pending ends the wait at once, and then the wait has no result.
        if (wasTaken && result !== undefined)
            ++cutBehindTheWait;
    });
    if (!cutBehindTheWait)
        throw new Error("no trap check stopped the call behind its wait");
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

// A count that the call does not reach ends with the call. The next call has a count of its own.
{
    shouldBe($vm.callWithTerminationAtTrapCheck(() => 1, 0x7fffffff), false, "a count that is not reached");
    nothingLeftBehind("a count that is not reached");
    shouldBe($vm.callWithTerminationAtTrapCheck(() => { }, 1), true, "a call after a count that was not reached");
    nothingLeftBehind("a call after a count that was not reached");
}

// The call withdraws its own termination only. A time limit around the call ends it, in a loop and in a wait.
{
    const cell = new Int32Array(new SharedArrayBuffer(4));
    for (const fn of [() => { for (;;) { } }, () => { Atomics.wait(cell, 0, 0); }]) {
        let returned = false;
        shouldBe(timesOut(() => {
            $vm.callWithTerminationAtTrapCheck(fn, 0x7fffffff);
            returned = true;
        }, 5), true, "a time limit around a call that does not reach its count");
        shouldBe(returned, false, "a call that a time limit cut returned");
        nothingLeftBehind("a time limit around a call");
    }
}

// A time limit around a loop of calls ends the loop, wherever the limit passes. Where it passes while a call withdraws
// its own termination, the call requests the termination of the limit again. A loop whose limit was lost ends by the
// clock, and is reported.
for (let round = 0; round < 10; ++round) {
    const end = preciseTime() + 2;
    shouldBe(timesOut(() => {
        while (preciseTime() < end)
            $vm.callWithTerminationAtTrapCheck(() => { }, 1);
    }, 1), true, `a time limit around a loop of calls, round ${round}`);
    nothingLeftBehind("a time limit around a loop of calls");
}

// The other way round: the count ends inside a time limit in the function, and that limit passes before the
// termination has left it. The time limit requests the termination of the count again, so the function does not go on.
{
    function spin(turns) {
        for (let i = 0; i < turns; ++i) { }
    }
    for (let n = 1; n <= 120; ++n) {
        let wentOn = false;
        const end = preciseTime() + 2;
        shouldBe($vm.callWithTerminationAtTrapCheck(() => {
            while (preciseTime() < end)
                timesOut(() => { spin(40); }, 0);
            wentOn = true;
        }, n), true, `a call around a loop of time limits, n = ${n}`);
        shouldBe(wentOn, false, `n = ${n}: the function went on behind the n-th trap check`);
    }
    nothingLeftBehind("a call around a loop of time limits");
}

// A time limit inside the function ends what it limits, and the count goes on behind it.
{
    const cell = new Int32Array(new SharedArrayBuffer(4));
    let limited = false;
    let turns = 0;
    const wasTaken = $vm.callWithTerminationAtTrapCheck(() => {
        limited = timesOut(() => { Atomics.wait(cell, 0, 0); }, 5);
        // Only the count ends this loop.
        for (;;)
            ++turns;
    }, 1000);
    shouldBe(limited, true, "a time limit inside the function");
    shouldBe(wasTaken, true, "a call whose function used a time limit");
    if (!turns || turns >= 1000)
        throw new Error(`the count went on for ${turns} turns of the loop behind the time limit`);
    nothingLeftBehind("a time limit inside the function");
}

// A call inside the function of another call is refused: the VM has one count.
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

// No trap check counts while terminations are deferred. The setter of an index of a sparse array is script that runs
// so: it never sees a termination, and it runs to its end once it has started. The call is refused there.
{
    const array = [];
    array[1000000] = 1;
    let pendingInside;
    let started;
    let finished;
    let refusal;
    function step() {
    }
    Object.defineProperty(array, 5, {
        set() {
            started = true;
            pendingInside ||= $vm.hasPendingTermination();
            step();
            step();
            step();
            pendingInside ||= $vm.hasPendingTermination();
            if (refusal === undefined) {
                try {
                    refusal = $vm.callWithTerminationAtTrapCheck(() => { }, 1000);
                } catch (error) {
                    refusal = error;
                }
            }
            finished = true;
        },
        configurable: true,
    });
    walk(() => {
        pendingInside = started = finished = false;
    }, () => {
        array[5] = 2;
    }, (n, wasTaken) => {
        shouldBe(pendingInside, false, `n = ${n}: a termination is pending inside script that runs with terminations deferred`);
        shouldBe(finished, started, `n = ${n}: script that runs with terminations deferred went to its end`);
    });
    if (!(refusal instanceof TypeError))
        throw new Error(`a call while terminations are deferred: expected a TypeError, got ${refusal}. If this setter no longer runs with terminations deferred, use another place that does.`);
    shouldBe(refusal.message, "$vm.callWithTerminationAtTrapCheck() cannot be called while terminations are deferred", "a call while terminations are deferred");
    nothingLeftBehind("script that runs with terminations deferred");
}

// What the function throws by itself stays thrown.
shouldThrow(() => $vm.callWithTerminationAtTrapCheck(() => { throw new SyntaxError("mine"); }, 1000), SyntaxError, "a function that throws");
nothingLeftBehind("a function that throws");

shouldThrow(() => $vm.callWithTerminationAtTrapCheck(1, 1), TypeError, "not a function");
shouldThrow(() => $vm.callWithTerminationAtTrapCheck(() => { }, 0), RangeError, "n = 0");
shouldThrow(() => $vm.callWithTerminationAtTrapCheck(() => { }, 1.5), RangeError, "n = 1.5");
shouldThrow(() => $vm.callWithTerminationAtTrapCheck(() => { }, 2 ** 31), RangeError, "n = 2 ** 31");
shouldThrow(() => $vm.callWithTerminationAtTrapCheck(() => { }, NaN), RangeError, "n = NaN");
shouldThrow(() => $vm.callWithTerminationAtTrapCheck(() => { }), RangeError, "no n");
nothingLeftBehind("calls that throw");
