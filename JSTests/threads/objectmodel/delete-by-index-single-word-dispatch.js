//@ requireOptions("--useJSThreads=1")
// Flag-on, JSObject::deletePropertyByIndex dispatches on one loaded butterfly
// word and stores the hole through that same word (or through the cell-locked
// ArrayStorage); after ensureSharedWriteBit or a CoW materialization it
// re-dispatches instead of falling into the flat-only butterfly() switch.
// Every leg the dispatch has, owner-side and foreign-side, with the result
// checked from both threads.
load("../harness.js", "caller relative");

function checkHole(a, i, message) {
    shouldBe(a[i], undefined, message);
    shouldBeFalse(a.hasOwnProperty(i), message);
    shouldBeFalse(i in a, message);
}

// Owner deletes on flat Int32 / Double / Contiguous, in-bounds and beyond the
// vector; length never changes.
{
    const ints = [];
    for (let i = 0; i < 10; ++i)
        ints.push(i);
    shouldBeTrue(delete ints[3]);
    checkHole(ints, 3);
    shouldBe(ints.length, 10);
    shouldBe(ints[2], 2);
    shouldBe(ints[4], 4);
    shouldBeTrue(delete ints[1000]);
    shouldBe(ints.length, 10);

    const doubles = [];
    for (let i = 0; i < 10; ++i)
        doubles.push(i + 0.5);
    shouldBeTrue(delete doubles[0]);
    checkHole(doubles, 0);
    shouldBe(doubles[1], 1.5);
    shouldBeTrue(delete doubles[9]);
    checkHole(doubles, 9);
    shouldBe(doubles.length, 10);
    shouldBe(Object.keys(doubles).length, 8);

    const strings = [];
    for (let i = 0; i < 10; ++i)
        strings.push("s" + i);
    shouldBeTrue(delete strings[5]);
    checkHole(strings, 5);
    shouldBe(strings[6], "s6");
    shouldBe(strings.join(), "s0,s1,s2,s3,s4,,s6,s7,s8,s9");
}

// Owner delete on a copy-on-write literal: materializes, then the hole lands
// in the private copy; a second literal from the same site is untouched.
{
    function make() { return [1, 2, 3, 4]; }
    const a = make();
    shouldBeTrue(delete a[1]);
    checkHole(a, 1);
    shouldBe(a[0], 1);
    shouldBe(a[2], 3);
    shouldBe(make()[1], 2);
    shouldBeTrue(delete a[100]);
    shouldBe(a.length, 4);

    function makeDoubles() { return [0.5, 1.5, 2.5]; }
    const d = makeDoubles();
    shouldBeTrue(delete d[2]);
    checkHole(d, 2);
    shouldBe(d[1], 1.5);
    shouldBe(makeDoubles()[2], 2.5);
}

// Blank and Undecided storage: nothing to clear, delete reports true.
{
    const o = {};
    shouldBeTrue(delete o[3]);
    const u = new Array(8);
    shouldBeTrue(delete u[3]);
    shouldBe(u.length, 8);
    shouldBeFalse(3 in u);
}

// ArrayStorage: in-vector slot, sparse-map entry, and a non-configurable
// sparse entry that must survive.
{
    const s = [];
    s[100000] = "far";
    for (let i = 0; i < 5; ++i)
        s[i] = i;
    Object.defineProperty(s, 200000, { value: "pinned", configurable: false, writable: true, enumerable: true });
    shouldBeTrue(delete s[2]);
    checkHole(s, 2);
    shouldBeTrue(delete s[100000]);
    checkHole(s, 100000);
    shouldBeFalse(delete s[200000]);
    shouldBe(s[200000], "pinned");
    shouldBe(s.length, 200001);
    shouldBe(Object.keys(s).join(), "0,1,3,4,200000");
}

// Foreign deletes. The first foreign write to an owner-created flat array fires
// the F1 flip and re-dispatches; on a copy-on-write literal it materializes
// first; on a segmented array (a foreign named-property add converted it) the
// hole goes through the spine; on ArrayStorage it is cell-locked.
{
    const ints = [];
    for (let i = 0; i < 16; ++i)
        ints.push(i);
    const doubles = [];
    for (let i = 0; i < 16; ++i)
        doubles.push(i + 0.25);
    const strings = [];
    for (let i = 0; i < 16; ++i)
        strings.push("v" + i);
    const cow = [10, 20, 30, 40];
    const cowDoubles = [0.5, 1.5, 2.5, 3.5];
    const segmented = [];
    for (let i = 0; i < 16; ++i)
        segmented.push(i * 2);
    const storage = [];
    storage[100000] = "far";
    for (let i = 0; i < 8; ++i)
        storage[i] = i;

    const t = new Thread(() => {
        const results = [];
        results.push(delete ints[4], ints[4] === undefined, 4 in ints, ints.length === 16);
        results.push(delete doubles[7], doubles[7] === undefined, 7 in doubles, doubles[8] === 8.25);
        results.push(delete strings[15], strings[15] === undefined, strings.length === 16, strings[14] === "v14");
        results.push(delete cow[0], cow[0] === undefined, cow[1] === 20, cow.length === 4);
        results.push(delete cowDoubles[3], cowDoubles[3] === undefined, cowDoubles[2] === 2.5);
        segmented.tag = "foreign"; // Foreign named-property add: the word goes segmented.
        results.push(delete segmented[2], segmented[2] === undefined, segmented[3] === 6, segmented.length === 16);
        results.push(delete segmented[1000], segmented.length === 16);
        results.push(delete storage[3], storage[3] === undefined, delete storage[100000], storage[100000] === undefined, storage.length === 100001);
        results.push(delete ints[500], ints.length === 16);
        return results;
    });
    const results = t.join();
    const expected = [
        true, true, false, true,
        true, true, false, true,
        true, true, true, true,
        true, true, true, true,
        true, true, true,
        true, true, true, true,
        true, true,
        true, true, true, true, true,
        true, true,
    ];
    shouldBe(results.length, expected.length);
    for (let i = 0; i < expected.length; ++i)
        shouldBe(results[i], expected[i], "foreign result " + i);

    checkHole(ints, 4);
    shouldBe(ints[3], 3);
    shouldBe(ints[5], 5);
    checkHole(doubles, 7);
    shouldBe(doubles[6], 6.25);
    checkHole(strings, 15);
    checkHole(cow, 0);
    shouldBe(cow[3], 40);
    checkHole(cowDoubles, 3);
    shouldBe(cowDoubles[0], 0.5);
    checkHole(segmented, 2);
    shouldBe(segmented.tag, "foreign");
    shouldBe(segmented[15], 30);
    checkHole(storage, 3);
    checkHole(storage, 100000);
    shouldBe(storage[7], 7);
    shouldBe(Object.keys(storage).join(), "0,1,2,4,5,6,7");

    // The owner keeps writing after the foreign deletes; the holes refill.
    ints[4] = "back";
    shouldBe(ints[4], "back");
    doubles[7] = 7.75;
    shouldBe(doubles[7], 7.75);
    segmented[2] = 4;
    shouldBe(segmented[2], 4);
    cow[0] = 10;
    shouldBe(cow.join(), "10,20,30,40");
}

// Deleting the same index from several threads at once: the slot is a hole
// afterwards and the neighbours are intact.
{
    const shared = [];
    for (let i = 0; i < 64; ++i)
        shared.push(i);
    const workers = spawnN(4, (t) => {
        for (let round = 0; round < 8; ++round) {
            delete shared[t * 8 + round];
            delete shared[32];
        }
        return true;
    });
    joinAll(workers).forEach(r => shouldBeTrue(r));
    for (let i = 0; i < 32; ++i)
        checkHole(shared, i);
    checkHole(shared, 32);
    for (let i = 33; i < 64; ++i)
        shouldBe(shared[i], i);
    shouldBe(shared.length, 64);
}
