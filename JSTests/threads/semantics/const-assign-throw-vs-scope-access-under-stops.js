//@ requireOptions("--useJSThreads=1")
// Assigning to a `const` binding of a closure scope throws a TypeError. The
// engine used to build that error object while it held the scope's symbol
// table lock, and with the GIL off building an object can wait for a
// stop-the-world that another thread requested. A thread that was blocked on
// the same symbol table lock (any other access to a variable of that scope)
// waits without a safepoint, so the stop never completed: a three-way
// deadlock, ended by the 30 s stop-the-world watchdog. Here two threads hammer
// one shared closure scope, one of them through the throwing assignment, while
// the main thread keeps requesting stops with full collections.
load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 4000;

function makeScope() {
    const fixed = 1;
    let counter = 0;
    return {
        // Throws TypeError (assignment to const) from inside the scope.
        assignConst(v) { try { fixed = v; } catch (e) { return e instanceof TypeError; } return false; },
        bump() { counter += 1; return counter; },
        read() { return fixed + counter; },
    };
}
const scope = makeScope();

const gate = { go: 0, done: 0 };
const threads = spawnN(THREADS, (t) => {
    while (Atomics.load(gate, "go") === 0) { }
    let ok = 0;
    for (let i = 0; i < ROUNDS; ++i) {
        if (t & 1) {
            if (scope.assignConst(i))
                ++ok;
        } else {
            scope.bump();
            if (scope.read() >= 1)
                ++ok;
        }
    }
    Atomics.add(gate, "done", 1);
    return ok;
});
Atomics.store(gate, "go", 1);
// Stop-the-world requests while the threads run.
let collections = 0;
while (Atomics.load(gate, "done") < THREADS) {
    fullGC();
    ++collections;
    sleepMs(0); // GIL on: let the threads run between collections.
}
for (const ok of joinAll(threads))
    shouldBe(ok, ROUNDS, "rounds that behaved");
shouldBeTrue(collections >= 1, "collections ran");
