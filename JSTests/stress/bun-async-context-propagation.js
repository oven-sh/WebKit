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
    async function* gen() {
        log.push(["gen start", get()]);
        yield 1;
        log.push(["gen after yield", get()]);
        await null;
        yield 2;
        log.push(["gen end", get()]);
    }
    async function consume(name) {
        for await (const v of gen())
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
    shouldBe(sawA, 3);
    shouldBe(sawNone, 3);
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
