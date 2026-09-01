//@ requireOptions("--useJSThreads=1")
// push/pop/length and butterfly-resizing operations on arrays shared between
// threads. Under the GIL stub each array operation is atomic, so exact totals
// are asserted; lock-guarded sections must stay exact in any implementation.
load("../resources/assert.js", "caller relative");

// --- Single foreign thread pushes enough to force repeated vector growth ---

const grown = [];
shouldBe(new Thread(arr => {
    for (let i = 0; i < 10000; ++i)
        arr.push(i);
    return arr.length;
}, grown).join(), 10000);
shouldBe(grown.length, 10000);
shouldBe(grown[0], 0);
shouldBe(grown[1234], 1234);
shouldBe(grown[9999], 9999);

// The spawning thread can keep growing the same butterfly afterwards.
grown.push(10000);
shouldBe(grown.length, 10001);
shouldBe(grown[10000], 10000);

// --- Lock-guarded concurrent pushes: exact count, no lost elements ---

const threadCount = 4;
const perThread = 1000;
const lock = new Lock();
const shared = [];
joinAll(spawnN(threadCount, index => {
    for (let i = 0; i < perThread; ++i)
        lock.hold(() => { shared.push(index * perThread + i); });
}));
shouldBe(shared.length, threadCount * perThread);
// Every value 0..3999 must appear exactly once.
{
    const seen = new Array(threadCount * perThread).fill(false);
    for (let i = 0; i < shared.length; ++i) {
        const value = shared[i];
        shouldBeTrue(Number.isInteger(value) && value >= 0 && value < seen.length, "push value in range");
        shouldBeFalse(seen[value], "push value duplicated: " + value);
        seen[value] = true;
    }
}

// --- Unguarded concurrent pushes: atomic under the GIL, so still exact ---

const unguarded = [];
joinAll(spawnN(threadCount, index => {
    for (let i = 0; i < perThread; ++i)
        unguarded.push(index * perThread + i);
}));
shouldBe(unguarded.length, threadCount * perThread);
{
    const seen = new Array(threadCount * perThread).fill(false);
    for (let i = 0; i < unguarded.length; ++i) {
        const value = unguarded[i];
        shouldBeTrue(value !== undefined, "unguarded push left a hole at " + i);
        shouldBeFalse(seen[value], "unguarded push value duplicated: " + value);
        seen[value] = true;
    }
}

// --- Concurrent pop from a shared work queue under a lock ---

const queue = [];
for (let i = 0; i < 2000; ++i)
    queue.push(i);
const popResults = joinAll(spawnN(threadCount, () => {
    const mine = [];
    for (;;) {
        let item;
        lock.hold(() => { item = queue.pop(); });
        if (item === undefined)
            break;
        mine.push(item);
    }
    return mine;
}));
shouldBe(queue.length, 0);
{
    const seen = new Array(2000).fill(false);
    let total = 0;
    for (const chunk of popResults) {
        for (const value of chunk) {
            shouldBeFalse(seen[value], "popped twice: " + value);
            seen[value] = true;
            ++total;
        }
    }
    shouldBe(total, 2000);
}

// --- Foreign thread resizes via out-of-bounds store ---

const sparseGrow = [1, 2, 3];
new Thread(arr => { arr[100] = "far"; }, sparseGrow).join();
shouldBe(sparseGrow.length, 101);
shouldBe(sparseGrow[100], "far");
shouldBe(sparseGrow[2], 3);
shouldBe(sparseGrow[50], undefined);

// --- Foreign thread shrinks and grows via .length ---

const resizable = [0, 1, 2, 3, 4, 5, 6, 7];
new Thread(arr => { arr.length = 3; }, resizable).join();
shouldBe(resizable.length, 3);
shouldBe(resizable[2], 2);
shouldBe(resizable[3], undefined);
shouldBeFalse(3 in resizable);
new Thread(arr => { arr.length = 6; }, resizable).join();
shouldBe(resizable.length, 6);
shouldBe(resizable[5], undefined);
shouldBeFalse(5 in resizable);
// Truncated-then-regrown slots must not resurrect stale values.
shouldBe(resizable[3], undefined);

// --- shift/unshift from a foreign thread ---

const deque = [1, 2, 3];
shouldBe(new Thread(arr => { arr.unshift(0); return arr.shift(); }, deque).join(), 0);
shouldBe(deque.length, 3);
shouldBe(deque[0], 1);
shouldBe(deque[2], 3);

// --- splice from a foreign thread ---

const spliced = [0, 1, 2, 3, 4];
const removed = new Thread(arr => arr.splice(1, 2, "x"), spliced).join();
shouldBe(removed.length, 2);
shouldBe(removed[0], 1);
shouldBe(removed[1], 2);
shouldBe(spliced.length, 4);
shouldBe(spliced[1], "x");
shouldBe(spliced[2], 3);

// --- Ping-pong growth: alternating threads extend the same array ---

const pingPong = [];
for (let round = 0; round < 8; ++round) {
    new Thread((arr, r) => {
        for (let i = 0; i < 100; ++i)
            arr.push(r * 100 + i);
    }, pingPong, round).join();
}
shouldBe(pingPong.length, 800);
for (let i = 0; i < 800; ++i)
    shouldBe(pingPong[i], i, "pingPong[" + i + "]");

// --- Resize while another thread holds an element reference (object identity) ---

const holder = [{ id: 1 }, { id: 2 }];
const obj = holder[0];
new Thread(arr => {
    for (let i = 0; i < 5000; ++i)
        arr.push(i);
}, holder).join();
shouldBe(holder[0], obj); // resize must not clone elements
shouldBe(holder[0].id, 1);
