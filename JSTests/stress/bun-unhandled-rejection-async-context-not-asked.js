//@ requireOptions("--useDollarVM=1")
// An embedder that has not asked for unhandled rejections to be reported in the async context
// (VM::reportUnhandledRejectionsInAsyncContext()) gets what it got before: a job with no handler runs in whatever
// async context is current when it runs. Seen here by the reject function of a capability, which is script.

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
