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

// Lengths of the long arrays and numbers of calls, as multiples of testLoopCount.
const sumLength = 32 * testLoopCount; // a multiple of 4
const joinLength = 10 * testLoopCount;
const growLength = 10 * testLoopCount;
const findLength = 20 * testLoopCount;
const lateLength = 40 * testLoopCount; // a multiple of 8
const calls = 10 * testLoopCount; // even

// 1. One call, a long loop: enters the optimizing tiers in the middle of the loop.
function sumLong(array) { let s = 0; for (let x of array) s += x; return s; }
noInline(sumLong);
shouldBe(sumLong(makeLong(sumLength, i => i & 3)), sumLength / 4 * 6);
shouldBe(sumLong(makeLong(sumLength, i => i & 3)), sumLength / 4 * 6);

// 2. The element type changes half way: exit to the lower tiers in the middle of the loop, continue from the right index.
function joinLong(array) { let s = 0; let strings = 0; for (let x of array) { if (typeof x === "string") strings++; else s += x; } return s + ":" + strings; }
noInline(joinLong);
{
    let ones = makeLong(joinLength, i => 1);
    for (let i = 0; i < 5; i++)
        shouldBe(joinLong(ones), joinLength + ":0");
}
shouldBe(joinLong(makeLong(joinLength, i => i === Math.floor(joinLength * 7 / 10) ? "s" : 1)), (joinLength - 1) + ":1");
shouldBe(joinLong(makeLong(joinLength, i => i > joinLength / 2 ? 1.5 : 1)), (joinLength * 1.25 - 0.5) + ":0");
shouldBe(joinLong(makeLong(joinLength, i => i === joinLength - 1 ? {} : 1)), (joinLength - 1) + "[object Object]:0");

// 3. The array changes shape in the middle of a long loop.
function growWhileLooping(array, growAt, cutAt, newLength) { let n = 0; for (let x of array) { n++; if (n === growAt) { array.push("tail"); array[10] = 0.5; } if (n === cutAt) array.length = newLength; } return n; }
noInline(growWhileLooping);
for (let i = 0; i < 4; i++)
    shouldBe(growWhileLooping(makeLong(growLength, i => i), growLength / 2, Math.floor(growLength * 6 / 10), Math.floor(growLength * 7 / 10)), Math.floor(growLength * 7 / 10));

// 4. break / return out of a loop that has been running optimized code for a while.
function findLong(array, what) { for (let x of array) { if (x === what) return x; } return -1; }
noInline(findLong);
{
    let array = makeLong(findLength, i => i);
    shouldBe(findLong(array, findLength - 1), findLength - 1);
    shouldBe(findLong(array, Math.floor(findLength * 3 / 4)), Math.floor(findLength * 3 / 4));
    shouldBe(findLong(array, -5), -1);
}

// 5. Destructuring in a hot function.
function swap(pair) { let [a, b] = pair; return [b, a]; }
function first3([a, b, c]) { return a + b + c; }
noInline(swap); noInline(first3);
{
    let p = [1, 2];
    for (let i = 0; i < calls; i++)
        p = swap(p);
    shouldBe(p.join(), "1,2");
    let t = 0;
    for (let i = 0; i < calls; i++)
        t += first3([1, 2, 3, 4]);
    shouldBe(t, 6 * calls);
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
function closedAfter(index) { return '[object Array Iterator] {"value":' + (index + 1) + ',"done":false}'; }
shouldBe(lateReturn(makeLong(lateLength, i => i), 10, lateLength - 10), lateLength - 10);
shouldBe(log.join("|"), closedAfter(lateLength - 10));
log = [];

// Now every close is observable. The same functions still give the same answers, and do not get stuck recompiling.
{
    let array = makeLong(lateLength / 2, i => i);
    for (let i = 0; i < 3; i++)
        shouldBe(lateReturn(array, -1, lateLength / 8 * 3), lateLength / 8 * 3);
}
shouldBe(log.join("|"), [closedAfter(lateLength / 8 * 3), closedAfter(lateLength / 8 * 3), closedAfter(lateLength / 8 * 3)].join("|"));
log = [];
shouldBe(sumLong(makeLong(sumLength, i => i & 3)), sumLength / 4 * 6);
shouldBe(findLong(makeLong(findLength, i => i), Math.floor(testLoopCount / 8)), Math.floor(testLoopCount / 8));
shouldBe(log.join("|"), closedAfter(Math.floor(testLoopCount / 8)));
log = [];
{
    let p = [1, 2, 3];
    for (let i = 0; i < 2 * testLoopCount; i++)
        p = swap(p);
    shouldBe(p.join(), "1,2");
    shouldBe(log.length, 2 * testLoopCount);
    log = [];
}
delete ArrayIteratorPrototype.return;
for (let i = 0; i < 2 * testLoopCount; i++)
    swap([1, 2, 3]);
shouldBe(log.length, 0);

compiledReasonablyOften(sumLong);
compiledReasonablyOften(joinLong);
compiledReasonablyOften(growWhileLooping);
compiledReasonablyOften(findLong);
compiledReasonablyOften(swap);
compiledReasonablyOften(first3);
compiledReasonablyOften(lateReturn);
