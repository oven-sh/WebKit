//@ requireOptions("--useJSThreads=1")
// Flag-on, putDirectIndex (Object.defineProperty with a plain data descriptor,
// and the builtins that define elements directly) stores through
// trySetIndexQuicklyConcurrent and falls to putDirectIndexSlowOrBeyondVectorLength
// when the try declines: the bound is re-read from the word the store goes
// through, so a stale snapshot (owner shrink, racing CoW materialization) can
// never become an out-of-bounds store. Single-threaded coverage of every shape
// the routing dispatches on, a foreign-thread ArrayStorage write (the §4.6 F1
// flip runs before the cell-locked store), and the needsSlowPutIndexing blank
// leg, which re-dispatches after createArrayStorage instead of indexing the
// returned vector directly.
load("../harness.js", "caller relative");

function defineData(o, i, v) {
    Object.defineProperty(o, i, { value: v, writable: true, enumerable: true, configurable: true });
}

// Int32 -> Double -> Contiguous through the quick path; in-bounds, at
// publicLength, and beyond the vector.
{
    const a = [1, 2, 3];
    defineData(a, 1, 42);
    shouldBe(a[1], 42);
    defineData(a, 3, 4);
    shouldBe(a.length, 4);
    shouldBe(a[3], 4);
    defineData(a, 100, 5);
    shouldBe(a.length, 101);
    shouldBe(a[100], 5);
    shouldBe(a[50], undefined);
    shouldBeFalse(a.hasOwnProperty(50));
    defineData(a, 2, 1.5);
    shouldBe(a[2], 1.5);
    defineData(a, 0, "s");
    shouldBe(a[0], "s");
    shouldBe(a[1], 42);
    shouldBe(a[2], 1.5);
    shouldBe(a[3], 4);
}

// Copying shrink, then a define past the new vector: the bound must come from
// the fresh word, and the miss must take the beyond-vector slow path.
{
    const a = [];
    for (let i = 0; i < 200; ++i)
        a.push(i);
    a.length = 5;
    defineData(a, 50, "x");
    shouldBe(a.length, 51);
    shouldBe(a[50], "x");
    shouldBe(a[4], 4);
    shouldBe(a[5], undefined);
    shouldBeFalse(a.hasOwnProperty(5));
    shouldBe(Object.keys(a).length, 6);
}

{
    const d = [];
    for (let i = 0; i < 100; ++i)
        d.push(i + 0.5);
    d.length = 3;
    defineData(d, 40, 2.5);
    shouldBe(d.length, 41);
    shouldBe(d[40], 2.5);
    shouldBe(d[2], 2.5);
    shouldBe(d[3], undefined);
    shouldBe(Object.keys(d).length, 4);
}

// Existing ArrayStorage: in-vector and beyond-vector defines take the locked
// AS leg of the slow path.
{
    const s = [];
    s[100000] = "sparse";
    defineData(s, 3, "three");
    shouldBe(s[3], "three");
    shouldBe(s[100000], "sparse");
    defineData(s, 100001, "next");
    shouldBe(s.length, 100002);
    shouldBe(s[100001], "next");
    shouldBe(Object.keys(s).join(), "3,100000,100001");
}

// Foreign thread writing an owner-created ArrayStorage array through putByIndex
// and putDirectIndex: the first foreign write flips SW under a per-event stop,
// then every store lands under the cell lock.
{
    const a = [];
    a[100000] = 1;
    for (let i = 0; i < 8; ++i)
        a[i] = i;
    const t = new Thread(() => {
        a[3] = "foreign";
        defineData(a, 5, "defined");
        a[200] = "grow";
        return a[3] === "foreign" && a[5] === "defined" && a[200] === "grow";
    });
    shouldBeTrue(t.join());
    shouldBe(a[3], "foreign");
    shouldBe(a[5], "defined");
    shouldBe(a[200], "grow");
    shouldBe(a[100000], 1);
    shouldBe(a.length, 100001);
    a[7] = 70;
    shouldBe(a[7], 70);
    shouldBe(Object.keys(a).length, 10);
}

// needsSlowPutIndexing blank leg: a prototype that may intercept indexed
// accesses makes the first indexed define create SlowPut ArrayStorage; the
// value then lands through the AS leg, not through the fresh vector pointer.
{
    const o = Object.create(new Proxy({}, {}));
    defineData(o, 5, "five");
    shouldBe(o[5], "five");
    shouldBe(Object.keys(o).join(), "5");
    defineData(o, 6, "six");
    shouldBe(o[6], "six");
    o[7] = "seven";
    shouldBe(o[7], "seven");
    shouldBe(Object.keys(o).join(), "5,6,7");
}
