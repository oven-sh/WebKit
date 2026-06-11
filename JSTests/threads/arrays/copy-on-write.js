//@ requireOptions("--useJSThreads=1")
// copyOnWrite arrays shared across threads. Array literals of constants start
// with CoW butterflies that may be shared between arrays from the same
// allocation site; a write from any thread must convert only the written
// array, never its CoW siblings.
load("../resources/assert.js", "caller relative");

// $vm is not available under the plain test command line; use indexing-mode
// introspection only when present.
const vm = typeof $vm !== "undefined" ? $vm : null;

function makeInt32() { return [1, 2, 3, 4]; }
function makeDouble() { return [0.5, 1.5, 2.5]; }
function makeContiguous() { return ["a", "b", "c"]; }

if (vm) {
    shouldBeTrue(vm.indexingMode(makeInt32()).includes("CopyOnWrite"), "literal should start CoW");
}

// --- Foreign-thread reads keep CoW arrays intact ---

const readTarget = makeInt32();
shouldBe(new Thread(arr => arr[0] + arr[3], readTarget).join(), 5);
if (vm)
    shouldBeTrue(vm.indexingMode(readTarget).includes("CopyOnWrite"), "foreign read must not convert CoW");
shouldBe(readTarget[0], 1);

// --- Foreign-thread write converts the written array only ---

const a = makeInt32();
const b = makeInt32();
new Thread(arr => { arr[0] = 99; }, a).join();
shouldBe(a[0], 99);
shouldBe(b[0], 1, "CoW sibling must not observe the write");
shouldBe(a[1], 2);
shouldBe(b.length, 4);
if (vm)
    shouldBeTrue(vm.indexingMode(b).includes("CopyOnWrite"), "sibling stays CoW");

// Same for double and contiguous CoW shapes.
const d1 = makeDouble();
const d2 = makeDouble();
new Thread(arr => { arr[1] = -1.5; }, d1).join();
shouldBe(d1[1], -1.5);
shouldBe(d2[1], 1.5);

const c1 = makeContiguous();
const c2 = makeContiguous();
new Thread(arr => { arr[2] = "z"; }, c1).join();
shouldBe(c1[2], "z");
shouldBe(c2[2], "c");

// --- Foreign-thread push converts CoW and grows ---

const pushed = makeInt32();
const pushedSibling = makeInt32();
shouldBe(new Thread(arr => arr.push(5), pushed).join(), 5);
shouldBe(pushed.length, 5);
shouldBe(pushed[4], 5);
shouldBe(pushedSibling.length, 4);

// --- Foreign-thread delete on a CoW array ---

const deleted = makeInt32();
const deletedSibling = makeInt32();
shouldBe(new Thread(arr => delete arr[2], deleted).join(), true);
shouldBeFalse(2 in deleted);
shouldBe(deleted.length, 4);
shouldBeTrue(2 in deletedSibling);
shouldBe(deletedSibling[2], 3);

// --- Foreign-thread sort/reverse (in-place mutators) convert CoW ---

const sorted = makeContiguous();
const sortedSibling = makeContiguous();
new Thread(arr => { arr.reverse(); }, sorted).join();
shouldBe(sorted.join(","), "c,b,a");
shouldBe(sortedSibling.join(","), "a,b,c");

// --- Foreign-thread length truncation converts CoW ---

const truncated = makeInt32();
const truncatedSibling = makeInt32();
new Thread(arr => { arr.length = 2; }, truncated).join();
shouldBe(truncated.length, 2);
shouldBe(truncatedSibling.length, 4);
shouldBe(truncatedSibling[3], 4);

// --- Non-mutating methods from foreign threads leave CoW alone ---

const surveyed = makeInt32();
shouldBe(new Thread(arr => arr.slice(1, 3).join(","), surveyed).join(), "2,3");
shouldBe(new Thread(arr => arr.indexOf(3), surveyed).join(), 2);
shouldBe(new Thread(arr => arr.includes(4), surveyed).join(), true);
shouldBe(new Thread(arr => arr.join("-"), surveyed).join(), "1-2-3-4");
if (vm)
    shouldBeTrue(vm.indexingMode(surveyed).includes("CopyOnWrite"), "non-mutating methods keep CoW");
shouldBe(surveyed[0], 1);

// --- Two threads write to two CoW siblings concurrently ---

const lock = new Lock();
const siblings = [];
for (let i = 0; i < 8; ++i)
    siblings.push(makeInt32());
joinAll(spawnN(4, index => {
    for (let i = index; i < 8; i += 4)
        lock.hold(() => { siblings[i][0] = 100 + i; });
}));
for (let i = 0; i < 8; ++i) {
    shouldBe(siblings[i][0], 100 + i, "sibling " + i);
    shouldBe(siblings[i][1], 2, "sibling " + i + " untouched tail");
}

// --- CoW array created inside a thread, written by the spawner ---

const fromThread = new Thread(() => [7, 8, 9]).join();
shouldBe(fromThread[1], 8);
fromThread[1] = 80;
shouldBe(fromThread[1], 80);
shouldBe(new Thread(arr => arr[1], fromThread).join(), 80);

// --- Spread/iteration of a CoW array inside a foreign thread ---

const spreadSource = makeInt32();
shouldBe(new Thread(arr => Math.max(...arr), spreadSource).join(), 4);
shouldBe(spreadSource.join(","), "1,2,3,4");
