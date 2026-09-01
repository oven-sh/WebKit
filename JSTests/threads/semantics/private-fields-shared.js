//@ requireOptions("--useJSThreads=1")
// semantics/private-fields-shared.js — private fields (#x), private methods,
// and brand checks on instances shared across threads. Private names are
// per-class hidden symbols stored on the instance: an instance constructed
// on ANY thread must expose its private state to class methods called from
// ANY other thread, brand checks (`#x in o`, method TypeErrors) must agree
// everywhere, and racing private-field writes through a Lock must be exact.
load("../harness.js", "caller relative");

class Counter {
    #count = 0;
    #id;
    static #instances = 0;
    constructor(id) {
        this.#id = id;
        Counter.#instances++;
    }
    inc() { return ++this.#count; }
    get() { return this.#count; }
    id() { return this.#id; }
    #secret() { return "secret:" + this.#id; }
    callSecret() { return this.#secret(); }
    static isCounter(o) { return #count in o; } // ergonomic brand check
    static instances() { return Counter.#instances; }
}

// --- Instance made on main, private state driven from threads (exact count
// under a Lock).
{
    const c = new Counter(7);
    const lock = new Lock();
    const THREADS = 4, PER = 500;
    const workers = spawnN(THREADS, () => {
        for (let i = 0; i < PER; ++i)
            lock.hold(() => { c.inc(); });
        return c.id(); // private read off-main
    });
    for (const id of joinAll(workers))
        shouldBe(id, 7, "private #id readable from worker");
    shouldBe(c.get(), THREADS * PER, "locked private increments are exact");
    shouldBe(c.callSecret(), "secret:7", "private method via private access cross-checked");
}

// --- Instance constructed ON a spawned thread, used on main: the private
// brand installed by a foreign thread's constructor is honored here.
{
    const made = new Thread(() => new Counter(99)).join();
    shouldBe(made.id(), 99);
    shouldBe(made.inc(), 1);
    shouldBeTrue(Counter.isCounter(made), "brand present on thread-constructed instance");
}

// --- Brand checks agree on every thread: a non-instance must throw
// TypeError from private access no matter which thread asks, and `#x in o`
// must answer identically everywhere.
{
    const real = new Counter(1);
    const fake = { count: 0 };
    const reports = joinAll(spawnN(3, () => {
        const r = [];
        r.push(Counter.isCounter(real));   // true
        r.push(Counter.isCounter(fake));   // false
        try {
            Counter.prototype.inc.call(fake);
            r.push("no-throw");
        } catch (e) {
            r.push((e instanceof TypeError || (e && e.name === "TypeError")) ? "TypeError" : "wrong:" + e);
        }
        return r.join(",");
    }));
    for (const rep of reports)
        shouldBe(rep, "true,false,TypeError", "brand semantics identical on every thread");
    shouldBe(Counter.isCounter(real), true);
    shouldBe(Counter.isCounter(fake), false);
}

// --- Static private state is class-level shared state: visible to all
// threads. `Counter.#instances++` in the constructor is a plain
// unsynchronized RMW, and the 4 spawned threads run concurrently with each
// other (join() only orders them against main), so the constructions are
// serialized under a Lock to make the exact count a legal expectation.
// (Unlocked concurrent ++ may lose updates — that documented racy outcome
// is range-asserted in the final section, not here.)
{
    const before = Counter.instances();
    const ctorLock = new Lock();
    joinAll(spawnN(4, () => { ctorLock.hold(() => { new Counter(0); }); }));
    shouldBe(Counter.instances(), before + 4, "static private counter saw all 4 locked constructions");
}

// --- Two classes' private names never alias, even when instances cross
// threads: an object carrying BOTH brands keeps the two #x fields separate.
{
    class A { #x = "a"; static readX(o) { return o.#x; } static install(o) { return Object.assign(new A(), o); } }
    class B { #x = "b"; static readX(o) { return o.#x; } }
    const a = new A(), b = new B();
    const fromThread = joinAll([
        new Thread(o => A.readX(o), a),
        new Thread(o => B.readX(o), b),
        new Thread(o => { try { A.readX(o); return "no-throw"; } catch (e) { return e.name; } }, b),
    ]);
    shouldBe(fromThread[0], "a");
    shouldBe(fromThread[1], "b");
    shouldBe(fromThread[2], "TypeError", "B instance has no A brand on a foreign thread");
}

// --- Unlocked racing increments on a private field are NOT asserted exact
// (lost updates are the documented racy semantics for ++), but the field
// must remain a sane number and the object coherent. The only guaranteed
// lower bound is 1: ++ is a plain RMW, and stale-overwrite interleavings
// (a thread reads a low value, stalls — e.g. under the amplifier's random
// yields — then writes it back late) can legally land the final value
// well below PER; but the LAST write is some thread's read+1 with read>=0,
// and main joins all threads before reading, so v >= 1 always holds.
{
    const c = new Counter(3);
    const THREADS = 4, PER = 300;
    joinAll(spawnN(THREADS, () => {
        for (let i = 0; i < PER; ++i)
            c.inc();
        return true;
    }));
    const v = c.get();
    shouldBeTrue(Number.isInteger(v) && v >= 1 && v <= THREADS * PER,
        "racy private counter stayed a sane integer: " + v);
    shouldBe(c.id(), 3, "neighboring private field undamaged by the race");
}

// WOULD-FAIL-IF: private-name identity or brand installation becomes
// per-thread — e.g. each thread (or VMLite) gets its own copy of a class's
// PrivateName symbols so a method called from thread T can't find #count on
// an instance built on thread U (TypeError where "true,false,TypeError"
// expects true), the brand-install during a foreign-thread construction is
// not visible to main (Counter.isCounter(made) false), two classes' #x
// names collide across threads (A.readX(b) returning "b" instead of
// TypeError), or a racing private-field store tears the butterfly so #id is
// damaged by #count traffic (c.id() !== 3 / non-integer count). Every brand
// and identity expectation here is exact, so any per-thread privatization
// or torn private storage fails the corresponding compare.
