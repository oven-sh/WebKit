//@ skip if not $jitTests
//@ requireOptions("--useConcurrentJIT=false")
//@ runDefault
//@ runDefault("--useFTLJIT=false")

// When DFG code is jettisoned for exiting too often and one of its array checks failed on ArrayStorage, an ArrayStorage
// array that another site saw last in Baseline code is that site's speculation failure too. These are the cases in which
// such a sample must stay a sample. Each case ends with a call that gives a frozen array to optimized code. That call
// must exit: the site still speculates on ordinary arrays.

if (!$vm.useDFGJIT())
    quit();

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

// False when the call left the optimized code before its last statement.
let ranOptimized = false;

function createInt32() {
    let array = [];
    array.push(1, 1, 1, 1);
    return array;
}

function createContiguous() {
    let array = [{ }, { }, { }, { }];
    array.length = 0;
    array.push(1, 1, 1, 1);
    return array;
}

function callUntilJettisoned(func, call, message) {
    let retries = reoptimizationRetryCount(func);
    for (let calls = 0; reoptimizationRetryCount(func) === retries && calls < 5000; ++calls)
        call(calls);
    shouldBe(reoptimizationRetryCount(func), retries + 1, `${message}: jettisoned`);
}

// 1. A watchpoint jettisons the code. It did not exit too often.
{
    function Point() { }
    Point.prototype.v = 1;
    let point = new Point;

    function readThenLoad(a, o) {
        let result = a[0] + a[1] + a[2] + o.v;
        ranOptimized = $vm.dfgTrue();
        return result;
    }
    noInline(readThenLoad);

    let ordinary = createInt32();
    let frozen = Object.freeze(createInt32());
    readThenLoad(frozen, point);
    fullGC();
    readThenLoad(ordinary, point);
    fullGC();
    readThenLoad(frozen, point);
    fullGC();
    // Stop as soon as the DFG has compiled it: code that reaches the FTL is left alone for another reason.
    for (let i = 0; i < testLoopCount && !numberOfDFGCompiles(readThenLoad); ++i)
        readThenLoad(ordinary, point);
    readThenLoad(ordinary, point);
    shouldBe(ranOptimized, true, "watchpoint: optimized");

    readThenLoad(frozen, point);
    shouldBe(ranOptimized, false, "watchpoint: the frozen array exits");
    Point.prototype.v = 3;
    for (let i = 0; i < testLoopCount * 20; ++i)
        readThenLoad(ordinary, point);
    shouldBe(ranOptimized, true, "watchpoint: optimized again");

    readThenLoad(frozen, point);
    shouldBe(ranOptimized, false, "watchpoint: the sites still speculate");
}

// 2. The array check that failed too often is at another site, and it failed on a Double array.
{
    function readBoth(a, h) {
        let x = a[0];
        return x + h.b[0];
    }
    function hot(a, b, n) {
        let s = 0;
        for (let i = 0; i < n; ++i)
            s += i;
        let result = readBoth(a, b) + s;
        ranOptimized = $vm.dfgTrue();
        return result;
    }
    noInline(hot);
    function cold(a, b) {
        return readBoth(a, b);
    }
    noInline(cold);
    function withProperty(value) {
        let array = [];
        array.push(value, value, value);
        array.tag = 1;
        return array;
    }

    let ordinary = createInt32();
    let frozen = Object.freeze(createInt32());
    let int32Holder = { b: withProperty(1) };
    let doubleHolder = { b: withProperty(1.5) };
    cold(frozen, int32Holder);
    fullGC();
    cold(ordinary, int32Holder);
    fullGC();
    cold(frozen, int32Holder);
    fullGC();
    cold(ordinary, int32Holder);
    fullGC();
    for (let i = 0; i < 400; ++i)
        hot(ordinary, int32Holder, 100);
    shouldBe(ranOptimized, true, "other array check: optimized");

    // One call from code that is not optimized leaves a frozen array as the last sample of the first site.
    callUntilJettisoned(hot, (calls) => {
        if (calls === 100)
            cold(frozen, int32Holder);
        hot(ordinary, doubleHolder, 100);
    }, "other array check");
    shouldBe(numberOfDFGCompiles(readBoth), 0, "other array check: the inlined function has no optimized code of its own");
    for (let i = 0; i < 3000; ++i)
        hot(ordinary, (i & 1) ? int32Holder : doubleHolder, 100);
    shouldBe(ranOptimized, true, "other array check: optimized again");

    hot(frozen, int32Holder, 100);
    shouldBe(ranOptimized, false, "other array check: the site still speculates");
}

// 3. The check that failed too often is a structure check of a property load.
{
    function loadThenRead(a, o) {
        let s = o.x;
        s += a[0];
        s += a[1];
        s += a[2];
        s += a[3];
        ranOptimized = $vm.dfgTrue();
        return s;
    }
    noInline(loadThenRead);

    let contiguous = [];
    let int32s = [];
    let frozen = [];
    for (let i = 0; i < 64; ++i) {
        contiguous.push(createContiguous());
        int32s.push(createInt32());
        frozen.push(Object.freeze(createContiguous()));
    }
    let first = { x: 1 };
    let second = { y: 0, x: 1 };
    for (let i = 0; i < 3000; ++i)
        loadThenRead((i & 64) ? int32s[i & 63] : contiguous[i & 63], first);
    shouldBe(ranOptimized, true, "property check: optimized");

    // The call before the jettison gets a frozen array.
    let lastCallBeforeJettison = jscOptions().osrExitCountForReoptimization - 1;
    callUntilJettisoned(loadThenRead, (calls) => {
        loadThenRead(calls === lastCallBeforeJettison ? frozen[0] : contiguous[calls & 63], second);
    }, "property check");
    for (let i = 0; i < 300000; ++i)
        loadThenRead(contiguous[i & 63], (i & 1) ? first : second);
    shouldBe(ranOptimized, true, "property check: optimized again");

    loadThenRead(frozen[1], first);
    shouldBe(ranOptimized, false, "property check: the sites still speculate");
}

// 4. A site that has seen ArrayStorage arrays only is not a site where ArrayStorage is ignored.
{
    function readTwo(a, b) {
        let s = a[0];
        s += b[0];
        ranOptimized = $vm.dfgTrue();
        return s;
    }
    noInline(readTwo);

    let ordinary = [];
    let frozen = [];
    for (let i = 0; i < 16; ++i) {
        ordinary.push(createInt32());
        frozen.push(Object.freeze(createInt32()));
    }
    for (let i = 0; i < testLoopCount * 10; ++i)
        readTwo((i % 2) ? ordinary[(i >> 1) % 16] : frozen[(i >> 1) % 16], frozen[i % 16]);
    readTwo(frozen[0], frozen[1]);
    shouldBe(ranOptimized, true, "ArrayStorage only: the site that failed is generic");

    for (let i = 0; i < testLoopCount * 10; ++i)
        readTwo(ordinary[i % 16], ordinary[(i + 1) % 16]);
    shouldBe(ranOptimized, true, "ArrayStorage only: optimized again");

    readTwo(ordinary[0], frozen[1]);
    shouldBe(ranOptimized, false, "ArrayStorage only: the other site speculates on ordinary arrays");
}

// 5. Code that reached the FTL and then exits too often: its frozen arrays are rare.
{
    let ranInFTL = false;
    function readFour(a) {
        let s = a[0] + a[1] + a[2] + a[3];
        ranOptimized = $vm.dfgTrue();
        ranInFTL = $vm.ftlTrue();
        return s;
    }
    noInline(readFour);

    let ordinary = [];
    let frozen = [];
    for (let i = 0; i < 16; ++i) {
        ordinary.push(createInt32());
        frozen.push(Object.freeze(createInt32()));
    }
    // Both kinds of arrays until the DFG has compiled it, then ordinary arrays until the FTL has.
    for (let i = 0; !numberOfDFGCompiles(readFour); ++i)
        readFour((i % 2) ? ordinary[i % 16] : frozen[i % 16]);
    for (let i = 0; i < testLoopCount * 10; ++i)
        readFour(ordinary[i % 16]);
    shouldBe(ranOptimized, true, "FTL: optimized");

    if (ranInFTL) {
        callUntilJettisoned(readFour, (calls) => {
            readFour(frozen[calls % 16]);
        }, "FTL");
        for (let i = 0; i < testLoopCount * 10; ++i)
            readFour(ordinary[i % 16]);
        shouldBe(ranOptimized, true, "FTL: optimized again");

        readFour(frozen[0]);
        shouldBe(ranOptimized, false, "FTL: the other sites still speculate");
    }
}

// 6. Code that cannot reach the FTL: nothing says that its frozen arrays are frequent.
if (!$vm.useFTLJIT()) {
    function readThree(a) {
        let s = a[0] + a[1] + a[2];
        ranOptimized = $vm.dfgTrue();
        return s;
    }
    noInline(readThree);

    let ordinary = [];
    let frozen = [];
    for (let i = 0; i < 16; ++i) {
        ordinary.push(createInt32());
        frozen.push(Object.freeze(createInt32()));
    }
    for (let i = 0; i < 4; ++i) {
        readThree(frozen[i]);
        fullGC();
        readThree(ordinary[i]);
        fullGC();
    }
    for (let i = 0; i < testLoopCount && !numberOfDFGCompiles(readThree); ++i)
        readThree(ordinary[i % 16]);
    readThree(ordinary[0]);
    shouldBe(ranOptimized, true, "no FTL: optimized");

    callUntilJettisoned(readThree, (calls) => {
        readThree(frozen[calls % 16]);
    }, "no FTL");
    for (let i = 0; i < testLoopCount * 10; ++i)
        readThree(ordinary[i % 16]);
    shouldBe(ranOptimized, true, "no FTL: optimized again");

    readThree(frozen[0]);
    shouldBe(ranOptimized, false, "no FTL: the sites still speculate");
}
