//@ requireOptions("--useDollarVM=1")
// A promise whose reject function is handed out keeps the owner that was current when it was made
// ($vm.ownerWhenMade, a number), and the embedder is told of it when the promise is rejected with nothing
// handling it, whoever rejects it (the shell records that as promise.ownerWhenRejected, and the owner that is
// current then as promise.ownerCurrentWhenRejected).

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error((msg || "") + " expected " + String(expected) + " but got " + String(actual));
}

const owner = () => $vm.asyncContextOwner();
const made = promise => $vm.ownerWhenMade(promise);
function inside(own, fn) {
    const previous = owner();
    $vm.setAsyncContextOwner(own);
    try {
        return fn();
    } finally {
        $vm.setAsyncContextOwner(previous);
    }
}

class Derived extends Promise { }
function construct() { return new Promise(() => { }); }
function constructDerived() { return new Derived(() => { }); }
function constructWithReject() { let reject; const promise = new Promise((_, r) => { reject = r; }); return { promise, reject }; }
noInline(construct);
noInline(constructDerived);
noInline(constructWithReject);

// Until there are owners nothing is kept, in any tier.
for (let i = 0; i < 1e5; i++) {
    const promise = i & 1 ? construct() : constructDerived();
    if (made(promise) !== undefined)
        throw new Error("a promise kept an owner before there was one");
}
shouldBe(made(Promise.withResolvers().promise), undefined, "withResolvers, no owners yet:");

// The ones that keep it, in code that was optimized before there were owners and in code that is optimized after.
for (let i = 0; i < 1e5; i++) {
    const own = 1 + (i % 3);
    const promise = inside(own, i & 1 ? construct : constructDerived);
    if (made(promise) !== own)
        throw new Error("new Promise #" + i + " expected " + own + " but got " + made(promise));
    if (!(i % 1000))
        shouldBe(made(construct()), undefined, "new Promise with no owner current:");
}
inside(7, () => {
    shouldBe(made(Promise.withResolvers().promise), 7, "withResolvers:");
    shouldBe(made(Derived.withResolvers().promise), 7, "withResolvers of a derived constructor:");
});

// The ones that do not: what rejects them is a job, which runs with the owner it was scheduled with.
inside(7, () => {
    const pending = new Promise(() => { });
    shouldBe(made((async () => { await pending; })()), undefined, "an async function's:");
    shouldBe(made(pending.then(() => { })), undefined, "then's:");
    shouldBe(made(pending.catch(() => { })), undefined, "catch's:");
    shouldBe(made(pending.finally(() => { })), undefined, "finally's:");
    shouldBe(made(Promise.resolve(1)), undefined, "a settled promise:");
    shouldBe(made(Promise.all([pending])), undefined, "all's:");
    shouldBe(made(Promise.allSettled([pending])), undefined, "allSettled's:");
    shouldBe(made(Promise.any([pending])), undefined, "any's:");
    shouldBe(made(Promise.race([pending])), undefined, "race's:");
});

// Nothing is kept once something has been done with the promise.
inside(7, () => {
    const { promise, resolve } = Promise.withResolvers();
    promise.then(() => { });
    shouldBe(made(promise), undefined, "a promise with a reaction:");
    resolve(1);
    shouldBe(made(promise), undefined, "a fulfilled promise:");
    shouldBe(made(1), undefined, "not a promise:");
});

// Rejected by somebody else, with nothing handling it: the embedder is told the owner it was made by.
{
    const a = inside(7, constructWithReject);
    inside(9, () => a.reject(new Error("made by 7, rejected by 9")));
    shouldBe(a.promise.ownerWhenRejected, 7, "made by 7, rejected by 9:");

    const b = inside(7, constructWithReject);
    b.reject(new Error("made by 7, rejected with no owner current"));
    shouldBe(b.promise.ownerWhenRejected, 7, "made by 7, rejected with no owner current:");

    const c = inside(7, () => Promise.withResolvers());
    inside(9, () => c.reject(new Error("withResolvers")));
    shouldBe(c.promise.ownerWhenRejected, 7, "withResolvers made by 7, rejected by 9:");

    // Made with no owner current: the embedder is told of none, and takes the one that is current.
    const d = constructWithReject();
    inside(9, () => d.reject(new Error("made by nobody")));
    shouldBe(d.promise.ownerWhenRejected, undefined, "made with no owner current:");

    // A promise that kept nothing, rejected by a job: the embedder is told of no owner, and the one that is
    // current is the one the job was scheduled with.
    const f = inside(8, constructWithReject);
    const g = inside(7, () => f.promise.then(() => { }));
    inside(9, () => f.reject(new Error("then")));
    drainMicrotasks();
    shouldBe(g.ownerWhenRejected, undefined, "then's, kept:");
    shouldBe(g.ownerCurrentWhenRejected, 7, "then's, current:");
    shouldBe(owner(), undefined, "nothing is left over");

    // Handled: the embedder is not told.
    const e = inside(7, constructWithReject);
    e.promise.catch(() => { });
    inside(9, () => e.reject(new Error("handled")));
    shouldBe(e.promise.ownerWhenRejected, undefined, "handled:");

    // In optimized code.
    for (let i = 0; i < 1e4; i++) {
        const own = 1 + (i % 3);
        const { promise, reject } = inside(own, constructWithReject);
        inside(5, () => reject(i));
        if (promise.ownerWhenRejected !== own)
            throw new Error("rejection #" + i + " expected " + own + " but got " + promise.ownerWhenRejected);
        promise.catch(() => { });
    }
}
drainMicrotasks();
