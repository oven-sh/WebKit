//@ runDefault("--useDollarVM=1")
//@ run("validate-exception-checks", "--useDollarVM=1", "--validateExceptionChecks=1")

// No VM entry is made while the TerminationException is pending: the entry returns with it pending, as if its callee
// threw it. $vm.callWithTimeLimit(fn, ms) is an embedder's time limit (node:vm's `timeout`), which requests the
// termination and withdraws it. $vm.dispatchOverTermination(how, ...functions) is an embedder's dispatcher that makes
// one entry after the other and does not stop at a termination.

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

// The overload of call() that clears what the callee threw clears the TerminationException too, also when the entry
// was not made. Nothing is pending then: the entry that follows is made, and the script that called the dispatcher
// goes on. The request for the termination stays for the embedder to act on. (The callee after the clear is a host
// function: the first call of a script function ends a DeferTermination scope, which throws the termination again.)
shouldBe(enteredUnderALimit("callClearingException", entered => {
    $vm.dispatchOverTermination("callClearingException", spinForever,
        () => entered.push("the entry over the pending exception"),
        Array.prototype.push.bind(entered, "host function after the clear"));
    entered.push("script after the dispatcher");
}), "host function after the clear,script after the dispatcher", "callClearingException");

// A SuspendExceptionScope takes the TerminationException away for what runs inside it: that entry is made. The
// exception is pending again when the scope ends, and the entries that follow are not made.
shouldBe(enteredUnderALimit("callSuspendingException", entered => {
    $vm.dispatchOverTermination("callSuspendingException", spinForever,
        Array.prototype.push.bind(entered, "host function inside the scope"),
        () => entered.push("script function after the scope"),
        Array.prototype.push.bind(entered, "host function after the scope"));
    entered.push("script after the dispatcher");
}), "host function inside the scope", "callSuspendingException");

// A checkpoint runs no microtask over the pending TerminationException and dequeues none: the microtasks run at the
// checkpoint that follows.
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

// A DeferTermination scope takes the TerminationException away for what runs inside it: that entry is made. The
// scope throws it again when it ends.
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
