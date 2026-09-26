//@ requireOptions("--useDollarVM=1")
// The embedder async-context slot ($vm.asyncContext) must be captured when a
// promise reaction / async continuation is scheduled and be the current value
// while it runs; a job that captured nothing runs with nothing, and whatever a
// job installs does not leak into the next one.

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error((msg || "") + " expected " + String(expected) + " but got " + String(actual));
}

const get = () => $vm.asyncContext();
const set = v => $vm.setAsyncContext(v);
function inContext(ctx, fn) {
    const prev = get();
    set(ctx);
    try {
        return fn();
    } finally {
        set(prev);
    }
}

const A = ["A"];
const B = ["B"];
const C = ["C"];
let log = [];

// .then on a pending promise, one and two handlers; .then on settled promises.
{
    let resolve;
    const pending = new Promise(r => { resolve = r; });
    const settled = Promise.resolve(1);
    const rejected = Promise.reject(1);
    inContext(A, () => {
        pending.then(() => log.push(["pending.then", get()]));
        pending.then(() => log.push(["pending.then#2", get()]), () => {});
        settled.then(() => log.push(["settled.then", get()]));
        rejected.then(() => {}, () => log.push(["rejected.then", get()]));
        rejected.catch(() => log.push(["rejected.catch", get()]));
        settled.finally(() => log.push(["settled.finally", get()]));
    });
    pending.then(() => log.push(["pending.then (no ctx)", get()]));
    inContext(B, () => {
        pending.then(() => { log.push(["pending.then B", get()]); set(A); /* residue */ });
        pending.then(() => log.push(["pending.then B#2", get()]));
    });
    resolve(1);
    drainMicrotasks();
    shouldBe(get(), undefined, "residue after drain");
    const expected = {
        "pending.then": A, "pending.then#2": A, "settled.then": A, "rejected.then": A, "rejected.catch": A, "settled.finally": A,
        "pending.then (no ctx)": undefined, "pending.then B": B, "pending.then B#2": B,
    };
    for (const [name, ctx] of log)
        shouldBe(ctx, expected[name], name);
    shouldBe(log.length, Object.keys(expected).length, "all handlers ran");
}

// await: pending native promise, settled promise, non-promise, thenable, and a
// promise that already has reactions (spilled inline reaction).
{
    log = [];
    let resolveP;
    const p = new Promise(r => { resolveP = r; });
    async function f(name, value) {
        log.push([name + " before", get()]);
        await value;
        log.push([name + " after", get()]);
        await null;
        log.push([name + " after2", get()]);
    }
    const thenable = { then(r) { log.push(["thenable.then", get()]); r(1); } };
    inContext(A, () => {
        f("pendingA", p);
        f("settledA", Promise.resolve(1));
        f("valueA", 42);
        f("thenableA", thenable);
    });
    inContext(B, () => {
        f("pendingB", p); // second reaction on p: p's inline reaction spills to the list
        f("pendingB2", p);
    });
    f("pendingNone", p);
    // enterWith-style residue inside an async function persists across its own awaits only
    (async () => {
        set(B);
        log.push(["self before", get()]);
        await p;
        log.push(["self after", get()]);
        await 0;
        log.push(["self after2", get()]);
    })();
    set(undefined);
    resolveP(1);
    drainMicrotasks();
    shouldBe(get(), undefined, "residue after drain 2");
    const want = name => {
        const who = name.split(" ")[0];
        if (who === "pendingNone")
            return undefined;
        if (who === "self" || who === "pendingB" || who === "pendingB2")
            return B;
        return A; // pendingA settledA valueA thenableA thenable.then
    };
    for (const [name, ctx] of log)
        shouldBe(ctx, want(name), name);
    shouldBe(log.filter(([n]) => n.endsWith("after2")).length, 8, "all continuations ran");
}

// async generators and for-await
{
    log = [];
    async function* gen(name) {
        log.push([name + " gen start", get()]);
        yield 1;
        log.push([name + " gen after yield", get()]);
        await null;
        yield 2;
        log.push([name + " gen end", get()]);
    }
    async function consume(name) {
        for await (const v of gen(name))
            log.push([name + " body", get()]);
        log.push([name + " done", get()]);
    }
    inContext(A, () => consume("A"));
    consume("none");
    drainMicrotasks();
    let sawA = 0, sawNone = 0;
    for (const [name, ctx] of log) {
        if (name.startsWith("A ")) { shouldBe(ctx, A, name); sawA++; }
        if (name.startsWith("none ")) { shouldBe(ctx, undefined, name); sawNone++; }
    }
    shouldBe(sawA, 6);
    shouldBe(sawNone, 6);
    shouldBe(get(), undefined, "residue after drain 3");
}

// Promise combinators keep the context of the awaiter.
{
    log = [];
    let r1, r2;
    const p1 = new Promise(r => { r1 = r; }), p2 = new Promise(r => { r2 = r; });
    inContext(A, () => {
        (async () => { await Promise.all([p1, p2]); log.push(["all", get()]); })();
        (async () => { await Promise.race([p1, p2]); log.push(["race", get()]); })();
        (async () => { await Promise.allSettled([p1, p2]); log.push(["allSettled", get()]); })();
        (async () => { await Promise.any([p1, p2]); log.push(["any", get()]); })();
    });
    inContext(B, () => r1(1));
    r2(2);
    drainMicrotasks();
    shouldBe(log.length, 4);
    for (const [name, ctx] of log)
        shouldBe(ctx, A, name);
}

// A handler that throws still restores the slot.
{
    inContext(A, () => {
        Promise.resolve().then(() => { set(B); throw new Error("x"); }).catch(() => {});
    });
    drainMicrotasks();
    shouldBe(get(), undefined, "restored after throwing handler");
}

// then() with no handler for the side that settles: the derived promise adopts the
// settlement in the context then() was called in. A Promise subclass makes the job
// observable, because it settles the derived promise through the capability's functions.
{
    log = [];
    let nextName = null;
    class Observed extends Promise {
        constructor(executor) {
            const name = nextName;
            nextName = null;
            super((resolve, reject) => executor(
                value => { if (name) log.push([name, get()]); resolve(value); },
                reason => { if (name) log.push([name, get()]); reject(reason); }));
        }
    }
    const derive = (name, fn) => { nextName = name; fn(); shouldBe(nextName, null, name + " derived a promise"); };
    let rejectPending, resolvePending;
    const pendingRejects = new Observed((_, reject) => { rejectPending = reject; });
    const pendingFulfills = new Observed(resolve => { resolvePending = resolve; });
    const rejected = Observed.reject(1);
    const fulfilled = Observed.resolve(1);
    rejected.catch(() => {});
    inContext(A, () => {
        derive("pending.then, rejects", () => pendingRejects.then(() => {}).catch(() => {}));
        derive("pending.then(), rejects", () => pendingRejects.then().catch(() => {}));
        derive("pending.catch, fulfills", () => pendingFulfills.catch(() => {}));
        derive("rejected.then", () => rejected.then(() => {}).catch(() => {}));
        derive("fulfilled.catch", () => fulfilled.catch(() => {}));
    });
    inContext(B, () => {
        derive("pending.then, rejects B", () => pendingRejects.then(() => {}).catch(() => {}));
        derive("rejected.then B", () => rejected.then(() => {}).catch(() => {}));
    });
    derive("pending.then, rejects (no ctx)", () => pendingRejects.then(() => {}).catch(() => {}));
    derive("pending.then(), rejects (no ctx)", () => pendingRejects.then().catch(() => {}));
    derive("rejected.then (no ctx)", () => rejected.then(() => {}).catch(() => {}));
    // Settled and drained inside C: no job sees it, not even one that captured nothing.
    inContext(C, () => { rejectPending(1); resolvePending(1); drainMicrotasks(); });
    shouldBe(get(), undefined, "residue after drain 4");
    const expected = {
        "pending.then, rejects": A, "pending.then(), rejects": A, "pending.catch, fulfills": A, "rejected.then": A, "fulfilled.catch": A,
        "pending.then, rejects B": B, "rejected.then B": B,
        "pending.then, rejects (no ctx)": undefined, "pending.then(), rejects (no ctx)": undefined, "rejected.then (no ctx)": undefined,
    };
    for (const [name, ctx] of log)
        shouldBe(ctx, expected[name], name);
    shouldBe(log.length, Object.keys(expected).length, "every derived promise settled once");
}

// A promise that adopts a native promise, and the second phase of finally(), settle in the
// context they were set up in. Both resolve their promise with the source's value, so a
// `then` getter added to that value after the source fulfilled with it observes the job.
{
    log = [];
    const observable = name => {
        const value = {};
        const source = Promise.resolve(value);
        Object.defineProperty(value, "then", { get() { log.push([name, get()]); return undefined; } });
        return source;
    };
    let settleThenable;
    inContext(A, () => {
        const adopted = observable("resolve(promise)");
        new Promise(resolve => resolve(adopted));
        // An own `constructor` added after the job was queued sends it down its species slow path.
        const respecied = observable("resolve(promise), species slow path");
        new Promise(resolve => resolve(respecied));
        respecied.constructor = Promise;
        const returned = observable("async return promise");
        (async () => returned)();
        const chained = observable("then returns promise");
        Promise.resolve().then(() => chained);
        observable("finally returns promise").finally(() => Promise.resolve());
        observable("finally returns thenable").finally(() => ({ then(resolve) { settleThenable = resolve; } }));
    });
    inContext(B, () => {
        observable("finally returns promise B").finally(() => Promise.resolve());
    });
    const noContext = observable("resolve(promise) (no ctx)");
    new Promise(resolve => resolve(noContext));
    observable("finally returns promise (no ctx)").finally(() => Promise.resolve());
    // Drained inside C, and the thenable settled inside C: no job sees it.
    inContext(C, () => { drainMicrotasks(); settleThenable(); drainMicrotasks(); });
    shouldBe(get(), undefined, "residue after drain 5");
    const expected = {
        "resolve(promise)": A, "resolve(promise), species slow path": A, "async return promise": A, "then returns promise": A,
        "finally returns promise": A, "finally returns thenable": A, "finally returns promise B": B,
        "resolve(promise) (no ctx)": undefined, "finally returns promise (no ctx)": undefined,
    };
    for (const [name, ctx] of log)
        shouldBe(ctx, expected[name], name);
    shouldBe(log.length, Object.keys(expected).length, "every observable value was resolved with once");
}

// The same two jobs when the promise they wait for is still pending: the context waits in the
// reaction, inline at first and in the list once the promise has a second reaction.
{
    log = [];
    // Fulfills with a plain value; the `then` getter is added before the reactions run.
    const pendingObservable = name => {
        const value = {};
        let resolve;
        const source = new Promise(r => { resolve = r; });
        const settle = () => {
            resolve(value);
            Object.defineProperty(value, "then", { get() { log.push([name, get()]); return undefined; } });
        };
        return { source, settle };
    };
    const observable = name => {
        const { source, settle } = pendingObservable(name);
        settle();
        return source;
    };
    const gate = () => {
        let open;
        const promise = new Promise(r => { open = r; });
        return { promise, open };
    };
    const adoptedOnce = pendingObservable("pending, one adopter");
    const adoptedTwice = pendingObservable("pending, three adopters");
    const inlineGate = gate(), listGate = gate(), noContextGate = gate();
    listGate.promise.then(() => {});
    inContext(A, () => {
        new Promise(resolve => resolve(adoptedOnce.source));
        new Promise(resolve => resolve(adoptedTwice.source));
        observable("finally returns pending promise").finally(() => inlineGate.promise);
        observable("finally returns pending promise with a reaction").finally(() => listGate.promise);
    });
    inContext(B, () => { new Promise(resolve => resolve(adoptedTwice.source)); });
    new Promise(resolve => resolve(adoptedTwice.source));
    observable("finally returns pending promise (no ctx)").finally(() => noContextGate.promise);
    // The first drain registers the reactions on the pending promises. They settle inside C.
    inContext(C, () => {
        drainMicrotasks();
        adoptedOnce.settle();
        adoptedTwice.settle();
        inlineGate.open();
        listGate.open();
        noContextGate.open();
        drainMicrotasks();
    });
    shouldBe(get(), undefined, "residue after drain 6");
    const expected = {
        "pending, one adopter": [A],
        "pending, three adopters": [A, B, undefined],
        "finally returns pending promise": [A],
        "finally returns pending promise with a reaction": [A],
        "finally returns pending promise (no ctx)": [undefined],
    };
    for (const name of Object.keys(expected)) {
        const seen = log.filter(entry => entry[0] === name).map(entry => entry[1]);
        shouldBe(seen.length, expected[name].length, name + " count");
        for (let i = 0; i < seen.length; i++)
            shouldBe(seen[i], expected[name][i], name + " #" + i);
    }
    shouldBe(log.length, 7, "nothing else observed a value");
}

// A job that captured no context and installs one itself does not leak it past its end.
{
    set(undefined);
    Promise.resolve().then(() => { set(A); });
    let seen = "unset";
    Promise.resolve().then(() => { seen = get(); });
    drainMicrotasks();
    shouldBe(seen, undefined, "residue from a sibling microtask");
    shouldBe(get(), undefined, "residue after a microtask that captured nothing");
}
