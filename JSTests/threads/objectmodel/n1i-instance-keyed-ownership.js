//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// SPEC-objectmodel §2.1 N1-I (rev 16): a butterfly-less object's transition
// owner is the thread that ALLOCATED it (its butterfly word carries that
// thread's TID from birth), not the thread that created its structure.
// Before rev 16 ownership followed the shape's creator, so a second thread
// adding properties to its OWN fresh `{}` counted as foreign, fired the empty-
// object structure's thread-local sets, and from then on no thread could use
// the lock-free / cached transition for plain objects.
//
// Checks, with $vm probes: (1) objects are born tagged with the allocating
// thread's TID on both threads, with and without a butterfly; (2) a second
// thread adding properties to its own objects leaves the shared shapes' sets
// VALID; (3) a genuinely foreign add (second thread -> main's object) still
// fires that object's structure's sets and lands exactly; (4) values exact.
load("../harness.js", "caller relative");

if ($vm.butterflyOwnerTID === undefined)
    throw new Error("needs $vm.butterflyOwnerTID / $vm.currentButterflyTID / $vm.structureThreadLocalSetsValid");

function bothValid(o) { const v = $vm.structureThreadLocalSetsValid(o); return v[0] && v[1]; }

const mainTID = $vm.currentButterflyTID();
const mine = {};
shouldBe($vm.butterflyOwnerTID(mine), mainTID, "main's fresh {} is tagged with main's TID");
const mineBig = { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9 }; // has out-of-line storage
shouldBe($vm.butterflyOwnerTID(mineBig), mainTID, "main's butterfly-bearing object is tagged with main's TID");

// Warm the shapes {} -> {x} -> {x,y} on main so they exist and are main-created.
function addXY(o, v) { o.x = v; o.y = v + 1; return o; }
noInline(addXY);
for (let i = 0; i < 5000; ++i) addXY({}, i);
const probe = addXY({}, -1);
shouldBeTrue(bothValid({}), "empty-object shape sets valid before the second thread runs");
shouldBeTrue(bothValid(probe), "{x,y} shape sets valid before the second thread runs");

const sharedVictim = {};          // main-owned, will receive a FOREIGN add
const r = new Thread((victim) => {
    const tid = $vm.currentButterflyTID();
    const own = {};
    const ownTag = $vm.butterflyOwnerTID(own);
    const ownBig = { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9 };
    const ownBigTag = $vm.butterflyOwnerTID(ownBig);
    // Own adds through the main-created shapes: must NOT fire anything.
    for (let i = 0; i < 5000; ++i) addXY({}, i);
    const after = addXY({}, 7);
    const setsStillValid = (() => { const v = $vm.structureThreadLocalSetsValid(after); return v[0] && v[1]; })();
    const emptyStillValid = (() => { const v = $vm.structureThreadLocalSetsValid({}); return v[0] && v[1]; })();
    // Foreign add: main's object.
    victim.z = 42;
    return { tid, ownTag, ownBigTag, setsStillValid, emptyStillValid, ax: after.x, ay: after.y };
}, sharedVictim).join();

shouldBeTrue(r.tid !== mainTID, "second thread has its own TID");
shouldBe(r.ownTag, r.tid, "second thread's fresh {} is tagged with ITS TID");
shouldBe(r.ownBigTag, r.tid, "second thread's butterfly-bearing object is tagged with its TID");
shouldBeTrue(r.setsStillValid, "{x,y} shape sets still valid after the second thread's own adds");
shouldBeTrue(r.emptyStillValid, "empty-object shape sets still valid after the second thread's own adds");
shouldBe(r.ax, 7, "second thread's add exact (x)");
shouldBe(r.ay, 8, "second thread's add exact (y)");
shouldBeTrue(bothValid(addXY({}, 3)), "main still sees valid sets on {x,y} afterwards");
shouldBe(sharedVictim.z, 42, "foreign add landed");
shouldBeTrue(!bothValid(sharedVictim), "foreign add fired the victim structure's sets (F2)");
