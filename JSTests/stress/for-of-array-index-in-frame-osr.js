//@ runDefault
//@ runDefault("--useUnboxedFastArrayIteration=0")
//@ runDefault("--useUnboxedFastArrayIteration=1")
//@ runDefault("--useUnboxedFastArrayIteration=1", "--useJIT=0")
//@ runDefault("--useUnboxedFastArrayIteration=1", "--useDFGJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10")
//@ runDefault("--useUnboxedFastArrayIteration=1", "--useFTLJIT=0", "--useConcurrentJIT=0")
//@ runDefault("--useUnboxedFastArrayIteration=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20", "--thresholdForOptimizeSoon=20", "--thresholdForFTLOptimizeAfterWarmUp=50", "--thresholdForFTLOptimizeSoon=50")
//@ runDefault("--useUnboxedFastArrayIteration=1", "--forceOSRExitToLLInt=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20")

// A loop over an Array that runs without an iterator object changes tier in the middle: OSR entry from a long loop, OSR
// exit when a speculation fails half way, and code compiled only after IteratorClose became observable that has to take
// over frames opened before.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + expected + " but got " + actual);
}

function compiledReasonablyOften(f) {
    let n = numberOfDFGCompiles(f);
    if (n > 20 && n !== 1000000)
        throw new Error(f.name + " was compiled " + n + " times");
}

const ArrayIteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
const originalNext = ArrayIteratorPrototype.next;

function makeLong(n, f) { let a = []; for (let i = 0; i < n; i++) a.push(f(i)); return a; }

// 1. One call, a long loop: enters the optimizing tiers in the middle of the loop.
function sumLong(array) { let s = 0; for (let x of array) s += x; return s; }
noInline(sumLong);
shouldBe(sumLong(makeLong(300000, i => i & 3)), 450000);
shouldBe(sumLong(makeLong(300000, i => i & 3)), 450000);

// 2. The element type changes half way: exit to the lower tiers in the middle of the loop, continue from the right index.
function joinLong(array) { let s = 0; let strings = 0; for (let x of array) { if (typeof x === "string") strings++; else s += x; } return s + ":" + strings; }
noInline(joinLong);
for (let i = 0; i < 5; i++)
    shouldBe(joinLong(makeLong(100000, i => 1)), "100000:0");
shouldBe(joinLong(makeLong(100000, i => i === 70000 ? "s" : 1)), "99999:1");
shouldBe(joinLong(makeLong(100000, i => i > 50000 ? 1.5 : 1)), "124999.5:0");
shouldBe(joinLong(makeLong(100000, i => i === 99999 ? {} : 1)), "99999[object Object]:0");

// 3. The array changes shape in the middle of a long loop.
function growWhileLooping(array) { let n = 0; for (let x of array) { n++; if (n === 50000) { array.push("tail"); array[10] = 0.5; } if (n === 60000) array.length = 70000; } return n; }
noInline(growWhileLooping);
for (let i = 0; i < 4; i++)
    shouldBe(growWhileLooping(makeLong(100000, i => i)), 70000);

// 4. break / return out of a loop that has been running optimized code for a while.
function findLong(array, what) { for (let x of array) { if (x === what) return x; } return -1; }
noInline(findLong);
{
    let array = makeLong(200000, i => i);
    shouldBe(findLong(array, 199999), 199999);
    shouldBe(findLong(array, 150000), 150000);
    shouldBe(findLong(array, -5), -1);
}

// 5. Destructuring in a hot function.
function swap(pair) { let [a, b] = pair; return [b, a]; }
function first3([a, b, c]) { return a + b + c; }
noInline(swap); noInline(first3);
{
    let p = [1, 2];
    for (let i = 0; i < 100000; i++)
        p = swap(p);
    shouldBe(p.join(), "1,2");
    let t = 0;
    for (let i = 0; i < 100000; i++)
        t += first3([1, 2, 3, 4]);
    shouldBe(t, 600000);
    shouldBe(String(first3([1, 2])), "NaN");
    shouldBe(first3("abc"), "abc");
    shouldBe(first3(new Set([1, 2, 3])), 6);
}

// 6. IteratorClose becomes observable in the middle of a long loop which then keeps running, enters optimized code
// compiled after the fact, and finally breaks: exactly one call, on an Array Iterator that is positioned right.
let log = [];
function lateReturn(array, installAt, breakAt) {
    let n = 0;
    for (let x of array) {
        if (n === installAt)
            ArrayIteratorPrototype.return = function () { log.push(Object.prototype.toString.call(this) + " " + JSON.stringify(originalNext.call(this))); return {}; };
        if (n === breakAt)
            break;
        n++;
    }
    return n;
}
noInline(lateReturn);
shouldBe(lateReturn(makeLong(400000, i => i), 10, 399990), 399990);
shouldBe(log.join("|"), '[object Array Iterator] {"value":399991,"done":false}');
log = [];

// Now every close is observable. The same functions still give the same answers, and do not get stuck recompiling.
for (let i = 0; i < 3; i++)
    shouldBe(lateReturn(makeLong(200000, i => i), -1, 150000), 150000);
shouldBe(log.join("|"), '[object Array Iterator] {"value":150001,"done":false}|[object Array Iterator] {"value":150001,"done":false}|[object Array Iterator] {"value":150001,"done":false}');
log = [];
shouldBe(sumLong(makeLong(300000, i => i & 3)), 450000);
shouldBe(findLong(makeLong(200000, i => i), 1234), 1234);
shouldBe(log.join("|"), '[object Array Iterator] {"value":1235,"done":false}');
log = [];
{
    let p = [1, 2, 3];
    for (let i = 0; i < 20000; i++)
        p = swap(p);
    shouldBe(p.join(), "1,2");
    shouldBe(log.length, 20000);
    log = [];
}
delete ArrayIteratorPrototype.return;
for (let i = 0; i < 20000; i++)
    swap([1, 2, 3]);
shouldBe(log.length, 0);

compiledReasonablyOften(sumLong);
compiledReasonablyOften(joinLong);
compiledReasonablyOften(growWhileLooping);
compiledReasonablyOften(findLong);
compiledReasonablyOften(swap);
compiledReasonablyOften(first3);
compiledReasonablyOften(lateReturn);
