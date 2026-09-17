//@ requireOptions("--useDollarVM=1")
// The script execution owner slot next to the embedder async-context slot
// ($vm.asyncContextScriptExecutionOwner) is captured with the async context when a
// promise reaction / async continuation is scheduled, and both are current while it
// runs. A job that captured neither runs with neither, and what a job installs does
// not leak into the next one.

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error((msg || "") + " expected " + String(expected) + " but got " + String(actual));
}

const context = () => $vm.asyncContext();
const owner = () => $vm.asyncContextScriptExecutionOwner();
const setContext = v => $vm.setAsyncContext(v);
const setOwner = v => $vm.setAsyncContextScriptExecutionOwner(v);
// What is current, as one comparable string.
const names = new Map([[undefined, "-"]]);
const named = name => { const value = [name]; names.set(value, name); return value; };
const current = () => names.get(context()) + "/" + names.get(owner());
function inside(ctx, own, fn) {
    const previousContext = context(), previousOwner = owner();
    setContext(ctx);
    setOwner(own);
    try {
        return fn();
    } finally {
        setContext(previousContext);
        setOwner(previousOwner);
    }
}

const A = named("A"), B = named("B");
const O = named("O"), P = named("P");
let log = [];
const check = (expected, count) => {
    for (const [name, seen] of log)
        shouldBe(seen, expected(name), name);
    shouldBe(log.length, count, "every continuation ran");
    shouldBe(current(), "-/-", "nothing is left over");
};

// .then on pending and settled promises: an owner alone, a context alone, both.
{
    let resolve;
    const pending = new Promise(r => { resolve = r; });
    const settled = Promise.resolve(1);
    const rejected = Promise.reject(1);
    const register = tag => {
        pending.then(() => log.push([tag + " pending.then", current()]));
        pending.then(() => log.push([tag + " pending.then#2", current()]), () => {});
        settled.then(() => log.push([tag + " settled.then", current()]));
        rejected.then(() => {}, () => log.push([tag + " rejected.then", current()]));
        rejected.catch(() => log.push([tag + " rejected.catch", current()]));
        settled.finally(() => log.push([tag + " settled.finally", current()]));
    };
    inside(undefined, O, () => register("-/O"));
    inside(A, undefined, () => register("A/-"));
    inside(A, O, () => register("A/O"));
    inside(B, P, () => register("B/P"));
    register("-/-");
    // What a job leaves in the slots ends with it.
    inside(A, O, () => pending.then(() => { log.push(["A/O residue", current()]); setContext(B); setOwner(P); }));
    resolve(1);
    drainMicrotasks();
    check(name => name.split(" ")[0], 31);
}

// await: pending promise (its inline reaction spills to a list), settled promise, non-promise, thenable.
{
    log = [];
    let resolve;
    const pending = new Promise(r => { resolve = r; });
    async function f(name, value) {
        log.push([name + " before", current()]);
        await value;
        log.push([name + " after", current()]);
        await null;
        log.push([name + " after2", current()]);
    }
    const thenable = tag => ({ then(r) { log.push([tag + " thenable.then", current()]); r(1); } });
    const start = tag => {
        f(tag + " pending", pending);
        f(tag + " settled", Promise.resolve(1));
        f(tag + " value", 42);
        f(tag + " thenable", thenable(tag));
    };
    inside(undefined, O, () => start("-/O"));
    inside(A, P, () => start("A/P"));
    start("-/-");
    // An async function that changes the owner itself keeps it across its own awaits only.
    (async () => {
        setOwner(P);
        log.push(["-/P self", current()]);
        await pending;
        log.push(["-/P self", current()]);
        await 0;
        log.push(["-/P self", current()]);
    })();
    setOwner(undefined);
    resolve(1);
    drainMicrotasks();
    check(name => name.split(" ")[0], 3 * (4 * 3 + 1) + 3);
}

// async generators and for-await
{
    log = [];
    async function* gen(tag) {
        log.push([tag + " gen start", current()]);
        yield 1;
        log.push([tag + " gen after yield", current()]);
        await null;
        yield 2;
        log.push([tag + " gen end", current()]);
    }
    async function consume(tag) {
        for await (const v of gen(tag))
            log.push([tag + " body", current()]);
        log.push([tag + " done", current()]);
    }
    inside(undefined, O, () => consume("-/O"));
    inside(B, O, () => consume("B/O"));
    consume("-/-");
    drainMicrotasks();
    check(name => name.split(" ")[0], 18);
}

// Promise combinators keep the awaiter's owner, whoever settles the inputs.
{
    log = [];
    let r1, r2;
    const p1 = new Promise(r => { r1 = r; }), p2 = new Promise(r => { r2 = r; });
    inside(A, O, () => {
        (async () => { await Promise.all([p1, p2]); log.push(["A/O all", current()]); })();
        (async () => { await Promise.race([p1, p2]); log.push(["A/O race", current()]); })();
        (async () => { await Promise.allSettled([p1, p2]); log.push(["A/O allSettled", current()]); })();
        (async () => { await Promise.any([p1, p2]); log.push(["A/O any", current()]); })();
    });
    inside(B, P, () => r1(1));
    r2(2);
    drainMicrotasks();
    check(name => name.split(" ")[0], 4);
}

// A handler that throws still restores both slots.
{
    log = [];
    inside(A, O, () => {
        Promise.resolve().then(() => { setContext(B); setOwner(P); throw new Error("x"); }).catch(() => log.push(["A/O catch", current()]));
    });
    drainMicrotasks();
    check(name => name.split(" ")[0], 1);
}

// A job that captured nothing and installs an owner itself does not leak it past its end.
{
    log = [];
    Promise.resolve().then(() => { setOwner(O); });
    Promise.resolve().then(() => log.push(["-/- sibling", current()]));
    drainMicrotasks();
    check(name => name.split(" ")[0], 1);
}

// The owner of a capture made in the optimizing tiers is the one that was current
// (the inline reaction fast path is for when there is nothing to capture).
{
    log = [];
    const settled = Promise.resolve(1);
    let failure;
    // then() with one handler on a pending promise that has no reactions yet: the inline reaction.
    function thenInLoop(tag, iterations) {
        for (let i = 0; i < iterations; i++) {
            let resolve;
            const pending = new Promise(r => { resolve = r; });
            pending.then(() => {
                if (current() !== tag)
                    failure = new Error("then: expected " + tag + " but got " + current());
            });
            resolve(1);
        }
    }
    noInline(thenInLoop);
    // (In rounds, so that no more than a round's worth of jobs is ever queued.)
    const rounds = 10, iterations = Math.ceil(2 * testLoopCount / rounds);
    for (let round = 0; round < rounds; round++) {
        thenInLoop("-/-", iterations);
        inside(undefined, O, () => thenInLoop("-/O", iterations));
        inside(A, undefined, () => thenInLoop("A/-", iterations));
        drainMicrotasks();
    }
    async function awaitInLoop(tag, iterations) {
        for (let i = 0; i < iterations; i++) {
            await settled;
            if (current() !== tag)
                failure = new Error("await: expected " + tag + " but got " + current());
        }
    }
    for (let round = 0; round < rounds; round++) {
        awaitInLoop("-/-", iterations);
        inside(undefined, P, () => awaitInLoop("-/P", iterations));
        inside(B, O, () => awaitInLoop("B/O", iterations));
        drainMicrotasks();
    }
    if (failure)
        throw failure;
    shouldBe(current(), "-/-", "nothing is left over");
}
