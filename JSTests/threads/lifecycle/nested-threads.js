//@ requireOptions("--useJSThreads=1")
// Nested thread creation: threads spawning threads, deep chains, fan-out
// from a spawned thread, and handles crossing thread boundaries.
load("../resources/assert.js", "caller relative");

// A thread can spawn and join a child.
shouldBe(new Thread(() => {
    const child = new Thread(() => 21);
    return child.join() * 2;
}).join(), 42);

// Grandchildren: three levels deep, results bubble up through joins.
shouldBe(new Thread(() => {
    return new Thread(() => {
        return new Thread(() => "deep").join() + "-mid";
    }).join() + "-top";
}).join(), "deep-mid-top");

// A recursive spawn chain N levels deep.
function chain(depth) {
    if (!depth)
        return 0;
    return new Thread(d => chain(d), depth - 1).join() + 1;
}
shouldBe(new Thread(() => chain(8)).join(), 8);

// Fan-out from a spawned thread: a worker spawns several grandchildren and
// aggregates their results.
shouldBe(new Thread(() => {
    const kids = [];
    for (let i = 0; i < 5; ++i)
        kids.push(new Thread(n => n * n, i));
    return kids.map(k => k.join()).reduce((a, b) => a + b, 0);
}).join(), 30);

// A Thread object created inside a thread can be joined by the main thread.
const escaped = new Thread(() => new Thread(() => "escaped-child")).join();
shouldBeTrue(escaped instanceof Thread);
shouldBe(escaped.join(), "escaped-child");

// A Thread object created on the main thread can be joined inside a child.
const mainSpawned = new Thread(() => "from-main");
shouldBe(new Thread(t => "got:" + t.join(), mainSpawned).join(), "got:from-main");

// Nested threads share the same heap: every level mutates one object.
const ledger = { levels: [] };
new Thread(l => {
    l.levels.push("child");
    new Thread(l2 => {
        l2.levels.push("grandchild");
    }, l).join();
    l.levels.push("child-after");
}, ledger).join();
ledger.levels.push("main");
shouldBe(ledger.levels.join(","), "child,grandchild,child-after,main");

// Exceptions propagate through nested joins level by level.
const inner = new Error("innermost");
shouldBe(shouldThrow(() => new Thread(e => {
    // Outer rethrows whatever crossed the inner join.
    new Thread(err => { throw err; }, e).join();
}, inner).join()), inner);

// Each nesting level has a distinct id; ids are visible to ancestors.
const ids = new Thread(() => {
    const myId = Thread.current.id;
    const childId = new Thread(() => Thread.current.id).join();
    return [myId, childId];
}).join();
shouldBe(typeof ids[0], "number");
shouldBe(typeof ids[1], "number");
shouldBeTrue(ids[0] !== ids[1]);
shouldBeTrue(ids[0] !== 0);
shouldBeTrue(ids[1] !== 0);
shouldBeTrue(ids[0] !== Thread.current.id);

// A spawned thread does not need to be joined for its side effects to have
// happened by the time some *other* join orders them: parent joins only the
// writer, main joins parent.
const cell = { value: null };
new Thread(c => {
    new Thread(inner => { inner.value = "written"; }, c).join();
    return c.value;
}, cell).join();
shouldBe(cell.value, "written");
