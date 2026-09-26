//@ requireOptions("--useDollarVM=1")
// A promise that a job with no handler rejects, with nothing handling the rejection: the embedder is told
// (promiseRejectionTracker) in the async context that job was scheduled in, once it has asked for that
// (VM::setReportsUnhandledRejectionsInAsyncContext()). The shell's tracker records the async context it is
// called in, for the promise, in the Map `asyncContextsWhenRejected`.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + String(expected) + " but got " + String(actual));
}

$vm.setReportsUnhandledRejectionsInAsyncContext();
globalThis.asyncContextsWhenRejected = new Map();

const get = () => $vm.asyncContext();
const set = value => $vm.setAsyncContext(value);
function inContext(context, fn) {
    const previous = get();
    set(context);
    try {
        return fn();
    } finally {
        set(previous);
    }
}

const noop = () => { };
class Subclass extends Promise { }
const error = () => new Error("rejected");
const rejecting = () => (async () => { throw error(); })();
const rejectedLater = () => new Promise((_, reject) => { Promise.resolve().then(() => reject(error())); });
function alreadyRejected() {
    const promise = Promise.reject(error());
    promise.catch(noop);
    return promise;
}

// Each returns the promise that ends up rejected with nothing handling it.
const forms = {
    "Promise.reject()": () => Promise.reject(error()),
    "new Promise, rejected by its executor": () => new Promise((_, reject) => reject(error())),
    "async function that throws": () => rejecting(),
    "async function that throws after an await": () => (async () => { await null; throw error(); })(),
    "then() whose handler throws": () => Promise.resolve().then(() => { throw error(); }),

    "new Promise resolved with a promise that rejects": () => new Promise(resolve => resolve(rejecting())),
    "new Promise resolved with a promise that rejects later": () => new Promise(resolve => resolve(rejectedLater())),
    "new Promise resolved with a promise that is given its own constructor before it is adopted": () => new Promise(resolve => {
        const adopted = rejecting();
        resolve(adopted);
        Object.defineProperty(adopted, "constructor", { value: Promise, configurable: true });
    }),
    "async function that returns a promise that rejects": () => (async () => rejecting())(),
    "then() whose handler returns a promise that rejects": () => Promise.resolve().then(() => rejecting()),
    "then() whose handler is an async function that throws": () => Promise.resolve().then(async () => { throw error(); }),
    "catch() whose handler is an async function that throws": () => alreadyRejected().catch(async () => { throw error(); }),
    "finally() whose handler is an async function that throws": () => Promise.resolve().finally(async () => { throw error(); }),

    "then() with no handler for the rejection, of a pending promise": () => rejectedLater().then(noop),
    "then() with no handler for the rejection, of a pending instance of a subclass": () => new Subclass((_, reject) => { Promise.resolve().then(() => reject(error())); }).then(noop),
    "then() with no handler for the rejection, of a rejected promise": () => alreadyRejected().then(noop),
    "then() with no handlers, of a rejected promise": () => alreadyRejected().then(),
    "then() with no handler for the rejection, of a rejected instance of a subclass": () => {
        const rejected = Subclass.reject(error());
        rejected.catch(noop);
        return rejected.then(noop);
    },
    "finally() of a rejected promise": () => alreadyRejected().finally(noop),
    "a chain of then() with no handler for the rejection": () => alreadyRejected().then(noop).then(noop).then(noop),

    "Promise.all() of a promise that rejects": () => Promise.all([Promise.resolve(1), rejecting()]),
    "Promise.all() of a promise that rejects later": () => Promise.all([rejectedLater()]),
    "Promise.race() of a promise that rejects": () => Promise.race([rejecting()]),
    "Promise.race() of a then() whose handler is an async function that throws": () => Promise.race([Promise.resolve().then(async () => { throw error(); })]),
    "Promise.any() of promises that reject": () => Promise.any([rejecting(), rejectedLater()]),
};

const rejectedAtOnce = ["Promise.reject()", "new Promise, rejected by its executor", "async function that throws"];

const A = { name: "A" };
const B = { name: "B" };

function check(round) {
    for (const [name, form] of Object.entries(forms)) {
        for (const context of [A, B, undefined]) {
            asyncContextsWhenRejected.clear();
            const promise = inContext(context, form);
            // What is current when the jobs run is not what counts.
            inContext(context === A ? B : A, drainMicrotasks);
            shouldBe(asyncContextsWhenRejected.has(promise), true, `${name}: the embedder is told (${round})`);
            shouldBe(asyncContextsWhenRejected.get(promise)?.name, context?.name, `${name}, in ${context?.name}: the async context the embedder is told in (${round})`);
        }
    }
}

// What is kept is not seen by anything else: values arrive, and a rejection that is handled is not reported.
function checkValues(round) {
    asyncContextsWhenRejected.clear();
    const seen = [];
    const handled = [];
    inContext(A, () => {
        const adopted = new Promise(resolve => resolve(Promise.resolve("adopted")));
        adopted.then(value => seen.push(value));
        const passedOn = Promise.resolve("passed on").then(undefined, noop);
        passedOn.then(value => seen.push(value));
        (async () => seen.push(await new Promise(resolve => resolve(Promise.resolve("awaited")))))();
        Promise.all([Promise.resolve("all")]).then(values => seen.push(values[0]));
        Promise.race([Promise.resolve("race")]).then(value => seen.push(value));
        Promise.any([Promise.resolve("any")]).then(value => seen.push(value));

        for (const [name, form] of Object.entries(forms)) {
            // (Those are rejected before anything can handle them.)
            if (rejectedAtOnce.includes(name))
                continue;
            const promise = form();
            promise.catch(noop);
            handled.push(promise);
        }
        const caughtLater = new Promise(resolve => resolve(rejectedLater()));
        handled.push(caughtLater);
        Promise.resolve().then(() => caughtLater.catch(reason => seen.push(reason.message)));
    });
    drainMicrotasks();
    shouldBe(seen.sort().join(), "adopted,all,any,awaited,passed on,race,rejected", `values (${round})`);
    for (const promise of handled)
        shouldBe(asyncContextsWhenRejected.has(promise), false, `a handled rejection is not reported (${round})`);
}

// Enough rounds for the functions above to reach every tier.
for (let round = 0; round < Math.max(2, testLoopCount / 50); round++) {
    check(round);
    checkValues(round);
}

// then() of another realm, called on an instance of a subclass of this one: the promise it returns is this
// realm's, and what then() leaves is the other realm's.
{
    const other = createGlobalObject();
    const inBoth = (context, fn) => inContext(context, () => {
        other.$vm.setAsyncContext(context);
        try {
            return fn();
        } finally {
            other.$vm.setAsyncContext(undefined);
        }
    });
    for (const context of [A, B]) {
        asyncContextsWhenRejected.clear();
        const promise = inBoth(context, () => other.Promise.prototype.then.call(new Subclass((_, reject) => { Promise.resolve().then(() => reject(error())); }), noop));
        inBoth(context === A ? B : A, drainMicrotasks);
        shouldBe(asyncContextsWhenRejected.get(promise), context, `then() of another realm, in ${context.name}`);
    }
}

// What script rejects is reported in the async context the script runs in, whatever the promise kept.
{
    for (const [name, combinator] of Object.entries({ race: "race", all: "all", any: "any" })) {
        asyncContextsWhenRejected.clear();
        let reject;
        const element = new Promise(noop);
        element.then = (_, rejectTheResult) => { reject = rejectTheResult; };
        const result = inContext(A, () => Promise[combinator]([element]));
        drainMicrotasks();
        inContext(B, () => reject(name === "any" ? error() : error()));
        if (name === "any")
            continue; // Promise.any() rejects once every element has, in a job.
        shouldBe(asyncContextsWhenRejected.get(result), B, `Promise.${name}() rejected by script`);
    }
}

// The reject function of a capability is script: it runs in the async context the tracker is told in.
{
    let rejectedIn;
    class Recording extends Promise {
        constructor(executor) {
            super((resolve, reject) => executor(resolve, reason => {
                rejectedIn = get();
                reject(reason);
            }));
        }
    }
    for (const context of [A, B, undefined]) {
        asyncContextsWhenRejected.clear();
        rejectedIn = "not rejected";
        const source = new Promise((_, reject) => { Promise.resolve().then(() => reject(error())); });
        const derived = inContext(context, () => Recording.prototype.then.call(Object.setPrototypeOf(source, Recording.prototype), noop));
        inContext(context === A ? B : A, drainMicrotasks);
        shouldBe(rejectedIn, context, `the reject function of a capability, in ${context?.name}`);
        shouldBe(asyncContextsWhenRejected.get(derived), context, `the promise of a capability, in ${context?.name}`);
    }
}

shouldBe(get(), undefined, "nothing is left current");
