//@ requireOptions("--useDollarVM=1")
// An embedder that has not asked for unhandled rejections to be reported in the async context
// (VM::setReportsUnhandledRejectionsInAsyncContext()) gets what it got before: a job with no handler, and what it
// leads to, runs in whatever async context is current when it runs.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + String(expected?.name ?? expected) + " but got " + String(actual?.name ?? actual));
}

const A = { name: "A" };
const B = { name: "B" };
function inContext(context, fn) {
    const previous = $vm.asyncContext();
    $vm.setAsyncContext(context);
    try {
        return fn();
    } finally {
        $vm.setAsyncContext(previous);
    }
}

let rejectedIn;
class Subclass extends Promise {
    constructor(executor) {
        super((resolve, reject) => executor(resolve, reason => {
            rejectedIn.push($vm.asyncContext());
            reject(reason);
        }));
    }
}

for (let round = 0; round < Math.max(2, testLoopCount / 50); round++) {
    for (const scheduledIn of [A, undefined]) {
        rejectedIn = [];
        // then() is given no handler for the rejection: what it returns is rejected by a job with no handler.
        const derived = inContext(scheduledIn, () => new Subclass((_, reject) => { Promise.resolve().then(() => reject(new Error("rejected"))); }).then(() => { }));
        derived.catch(() => { });
        inContext(B, drainMicrotasks);
        // The first is the promise then() was called on, rejected by script scheduled in scheduledIn. The second is the
        // one then() returned.
        shouldBe(rejectedIn.length, 2, "both promises are rejected");
        shouldBe(rejectedIn[0], scheduledIn, "the promise then() was called on");
        shouldBe(rejectedIn[1], B, "the promise then() returned");
    }
}

// A promise resolved with a native promise that has its own constructor by the time it is adopted.
for (let round = 0; round < Math.max(2, testLoopCount / 50); round++) {
    const ran = [];
    inContext(A, () => {
        const adopted = new Promise((_, reject) => { Promise.resolve().then(() => reject(new Error("rejected"))); });
        const outer = new Promise(resolve => resolve(adopted));
        outer.catch(() => { });
        Object.defineProperty(adopted, "constructor", {
            get() {
                ran.push($vm.asyncContext());
                return Promise;
            },
            configurable: true,
        });
    });
    inContext(B, drainMicrotasks);
    shouldBe(ran.length, 1, "the constructor is read once");
    shouldBe(ran[0], B, "the constructor is read");
}

// Promises that were made before the embedder asked: once it has, what a job with no handler rejects is
// reported in the async context it was scheduled in if that is still known, else in none. Never in whatever is
// current when the job runs.
{
    const rejecting = () => (async () => { await null; throw new Error("rejected"); })();
    const made = inContext(A, () => ({
        "Promise.all()": Promise.all([rejecting()]),
        "Promise.race()": Promise.race([rejecting()]),
        "finally()": Promise.resolve().finally(() => rejecting()),
        "then() with no handler for the rejection": rejecting().then(() => { }),
        "new Promise resolved with a promise that rejects": new Promise(resolve => resolve(rejecting())),
    }));
    $vm.setReportsUnhandledRejectionsInAsyncContext();
    globalThis.asyncContextsWhenRejected = new Map();
    inContext(B, drainMicrotasks);
    for (const [name, promise] of Object.entries(made)) {
        shouldBe(asyncContextsWhenRejected.has(promise), true, name + " is reported");
        shouldBe(asyncContextsWhenRejected.get(promise) === B, false, name + " is reported in what is current when the job runs");
    }
}
