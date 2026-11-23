//@ runDefault("--useDollarVM=1")

// From the throw of the TerminationException until the request for that termination is withdrawn
// (VM::isTerminatingExecution()), no VM entry is made: the entry returns with the TerminationException pending, as if
// its callee threw it. $vm.callWithTimeLimit(fn, ms) is an embedder's time limit (node:vm's `timeout`), which requests
// the termination and withdraws it. $vm.dispatchOverTermination(how, ...functions) is an embedder's dispatcher that
// makes one entry after the other and does not stop at a termination.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected}, got ${actual}`);
}

function spinForever() {
    for (;;) { }
}

// The limit is long next to the time that a call needs to reach spinForever(), also on a slow build under load.
const limit = 100;

// Runs `fn` under a time limit and returns what `entered` holds once the limit has cut it short.
function enteredUnderALimit(what, fn) {
    const entered = [];
    let timedOut = false;
    try {
        $vm.callWithTimeLimit(() => fn(entered), limit);
    } catch (e) {
        if (!(e instanceof RangeError && e.message == "timed out"))
            throw e;
        timedOut = true;
    }
    shouldBe(timedOut, true, `${what}: the limit cut the call short`);
    shouldBe($vm.hasPendingTermination(), false, `${what}: nothing is pending after the limit`);
    // The first entry after the termination is withdrawn is made.
    shouldBe([2, 1].sort((a, b) => a - b).join(), "1,2", `${what}: an entry after the limit`);
    return entered.join();
}

// The exception of the first entry is still pending at the next ones: a script function, a bound function of a script
// function, a host function (Array.prototype.push stores its one argument before it checks for an exception) and a
// constructor are not entered, and neither is the script that called the dispatcher.
shouldBe(enteredUnderALimit("call", entered => {
    $vm.dispatchOverTermination("call", spinForever,
        () => entered.push("script function"),
        (() => entered.push("bound script function")).bind(null),
        Array.prototype.push.bind(entered, "host function"));
    entered.push("script after the dispatcher");
}), "", "call");

shouldBe(enteredUnderALimit("construct", entered => {
    $vm.dispatchOverTermination("construct", spinForever,
        class { constructor() { entered.push("class constructor"); } },
        function () { entered.push("function constructor"); });
    entered.push("script after the dispatcher");
}), "", "construct");

// A dispatcher that clears what each callee threw clears the TerminationException too. The request is not withdrawn,
// so its next entries are refused all the same, and so is the first entry that the script beneath it makes.
shouldBe(enteredUnderALimit("callClearingException", entered => {
    $vm.dispatchOverTermination("callClearingException", spinForever,
        () => entered.push("script function"),
        Array.prototype.push.bind(entered, "host function"));
    for (;;)
        [2, 1].sort(() => entered.push("comparator"));
}), "", "callClearingException");

// A checkpoint runs no microtask over the termination, and the microtasks stay queued for the checkpoint that
// follows its end.
{
    let ranMicrotask = false;
    Promise.resolve().then(() => { ranMicrotask = true; });
    shouldBe(enteredUnderALimit("drainMicrotasks", entered => {
        $vm.dispatchOverTermination("drainMicrotasks", spinForever, spinForever);
        entered.push("script after the dispatcher");
    }), "", "drainMicrotasks");
    shouldBe(ranMicrotask, false, "drainMicrotasks: the microtask did not run over the termination");
    drainMicrotasks();
    shouldBe(ranMicrotask, true, "drainMicrotasks: the microtask ran at the next checkpoint");
}

// A DeferTermination scope defers the termination for what runs inside it: that entry is made. The termination goes
// on when the scope ends.
shouldBe(enteredUnderALimit("callDeferringTermination", entered => {
    $vm.dispatchOverTermination("callDeferringTermination", spinForever,
        () => entered.push("script function"),
        Array.prototype.push.bind(entered, "host function"));
    entered.push("script after the dispatcher");
}), "script function,host function", "callDeferringTermination");

// An inner limit that fires is withdrawn alone: the entries of the outer call that follow it are made.
shouldBe($vm.callWithTimeLimit(() => {
    const entered = [];
    try {
        $vm.callWithTimeLimit(() => $vm.dispatchOverTermination("call", spinForever, () => entered.push("inner")), limit);
    } catch { }
    $vm.dispatchOverTermination("call", () => entered.push("first"), () => entered.push("second"));
    return entered.join();
}, 60000), "first,second", "an outer call after an inner limit");
