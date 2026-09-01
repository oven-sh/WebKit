//@ requireOptions("--useJSThreads=1")
// Shared-object semantics: reading and writing existing properties across
// threads. Covers inline-cell properties, out-of-line (butterfly) properties,
// value identity, special values, and symbol keys. Ordering is established
// only via join() and Lock, never by timing assumptions.
load("../resources/assert.js", "caller relative");

// --- Cross-thread read of properties written by the main thread.
{
    const obj = { num: 42, str: "hello", nul: null, undef: undefined, t: true };
    const inner = { tag: "inner" };
    obj.ref = inner;
    const result = new Thread(o => {
        shouldBe(o.num, 42);
        shouldBe(o.str, "hello");
        shouldBe(o.nul, null);
        shouldBe(o.undef, undefined);
        shouldBe(o.t, true);
        shouldBe(o.ref, inner); // object identity is preserved across threads
        shouldBe(o.ref.tag, "inner");
        return o.num + 1;
    }, obj).join();
    shouldBe(result, 43);
}

// --- Cross-thread write: main observes values stored by a spawned thread.
{
    const obj = { x: 1, y: 2 };
    new Thread(o => {
        o.x = 100;
        o.y = o.y * 10;
    }, obj).join();
    shouldBe(obj.x, 100);
    shouldBe(obj.y, 20);
}

// --- Special values survive the cross-thread store/load round trip.
{
    const obj = { a: 0 };
    new Thread(o => {
        o.nan = NaN;
        o.negZero = -0;
        o.big = 2 ** 53;
        o.intMax = 0x7fffffff;
        o.bigint = 123456789012345678901234567890n;
    }, obj).join();
    shouldBe(obj.nan, NaN);
    shouldBe(obj.negZero, -0);
    shouldBe(obj.big, 2 ** 53);
    shouldBe(obj.intMax, 0x7fffffff);
    shouldBe(obj.bigint === 123456789012345678901234567890n, true);
}

// --- Object identity: a thread returning its argument returns the same cell,
// and writes through either reference are visible through the other.
{
    const shared = { v: "original" };
    const t = new Thread(o => { o.v = "from-thread"; return o; }, shared);
    const returned = t.join();
    shouldBe(returned, shared);
    shouldBe(shared.v, "from-thread");
    returned.v2 = "again";
    shouldBe(shared.v2, "again");
}

// --- Out-of-line storage: enough properties to force a butterfly. Foreign
// reads and overwrites of out-of-line slots must behave like local ones.
{
    const big = {};
    for (let i = 0; i < 200; ++i)
        big["p" + i] = i;
    const sum = new Thread(o => {
        let s = 0;
        for (let i = 0; i < 200; ++i)
            s += o["p" + i];
        o.p199 = -1; // overwrite an out-of-line slot from a foreign thread
        return s;
    }, big).join();
    shouldBe(sum, 199 * 200 / 2);
    shouldBe(big.p199, -1);
    shouldBe(big.p0, 0);
}

// --- Symbol-keyed properties shared across threads.
{
    const key = Symbol("shared-key");
    const obj = { [key]: "symbol-value" };
    const got = new Thread((o, k) => {
        const v = o[k];
        o[k] = "updated";
        return v;
    }, obj, key).join();
    shouldBe(got, "symbol-value");
    shouldBe(obj[key], "updated");
}

// --- Ping-pong: alternating writers, ordering established by join().
{
    const cell = { v: 0 };
    for (let round = 0; round < 10; ++round) {
        new Thread((o, r) => {
            shouldBe(o.v, 2 * r);
            o.v = 2 * r + 1;
        }, cell, round).join();
        shouldBe(cell.v, 2 * round + 1);
        cell.v = 2 * round + 2;
    }
    shouldBe(cell.v, 20);
}

// --- Many threads writing distinct existing properties: no lost writes.
{
    const obj = {};
    const threadCount = 8;
    for (let i = 0; i < threadCount; ++i)
        obj["slot" + i] = -1;
    joinAll(spawnN(threadCount, i => { obj["slot" + i] = i * i; }));
    for (let i = 0; i < threadCount; ++i)
        shouldBe(obj["slot" + i], i * i);
}

// --- Many threads incrementing one property under a Lock: full mutual
// exclusion means exactly threads*iterations increments survive.
{
    const lock = new Lock();
    const counter = { n: 0 };
    const threads = 4, iterations = 250;
    joinAll(spawnN(threads, () => {
        for (let i = 0; i < iterations; ++i)
            lock.hold(() => { counter.n++; });
    }));
    shouldBe(counter.n, threads * iterations);
}

// --- Lexical capture: a thread function's closure variables are shared state.
{
    let captured = { v: 1 };
    new Thread(() => { captured.v = 2; captured = { v: 3 }; }).join();
    shouldBe(captured.v, 3);
}

// --- Nested shared graph: writes deep in a shared structure are visible.
{
    const graph = { a: { b: { c: { leaf: 0 } } } };
    new Thread(g => { g.a.b.c.leaf = 99; }, graph).join();
    shouldBe(graph.a.b.c.leaf, 99);
}

// --- Cyclic shared structure traversal from a foreign thread.
{
    const a = { name: "a" };
    const b = { name: "b", peer: a };
    a.peer = b;
    const seen = new Thread(start => start.peer.peer.peer.name, a).join();
    shouldBe(seen, "b");
}
