//@ runDefault
//@ runDefault("--useUnboxedFastArrayIteration=0")
//@ runDefault("--useUnboxedFastArrayIteration=1")
//@ runDefault("--useUnboxedFastArrayIteration=1", "--useJIT=0")
//@ runDefault("--useUnboxedFastArrayIteration=1", "--useDFGJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10")
//@ runDefault("--useUnboxedFastArrayIteration=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20", "--thresholdForOptimizeSoon=20", "--thresholdForFTLOptimizeAfterWarmUp=50", "--thresholdForFTLOptimizeSoon=50")
//@ runDefault("--useUnboxedFastArrayIteration=1", "--forceOSRExitToLLInt=1", "--useFTLJIT=0", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20")

// for-of-array-index-in-frame-close.js makes IteratorClose observable from inside a loop that was opened without an iterator
// object once: a realm's watchpoints do not come back, and from then on it opens its loops with an Array Iterator object. Here
// every way out of a loop, and "return" installed from the default value of an element in the middle of a pattern (with and
// without a throw after it), gets a realm of its own that has done nothing but run the function a few times, for each of the three
// prototypes "return" can be installed on. "return" then stays, and the next call finds it there from the start.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + expected + " but got " + actual);
}

const realmSource = `
    const ArrayIteratorPrototype = Object.getPrototypeOf([][Symbol.iterator]());
    const IteratorPrototype = Object.getPrototypeOf(ArrayIteratorPrototype);
    const originalNext = ArrayIteratorPrototype.next;
    noDFG(originalNext); // It gets a handful of calls in each of these realms.
    const holders = [ArrayIteratorPrototype, IteratorPrototype, Object.prototype];
    const outError = new Error("out");
    const defaultError = new Error("dflt");
    let log = [];
    let seenIterators = [];
    function installReturn(where) {
        where.return = function () {
            // The index is observable through a next() on the iterator that was handed to us.
            let step = originalNext.call(this);
            log.push("return " + Object.prototype.toString.call(this) + " " + (Object.getPrototypeOf(this) === ArrayIteratorPrototype) + " " + JSON.stringify(step));
            seenIterators.push(this);
            return {};
        };
    }

    function breakOut(array, at, hook) { let seen = []; for (let x of array) { seen.push(x); if (x === at) { hook(); break; } } return seen.join(); }
    function returnOut(array, at, hook) { let seen = []; for (let x of array) { seen.push(x); if (x === at) { hook(); return seen.join(); } } return seen.join(); }
    function throwOut(array, at, hook) { let seen = []; try { for (let x of array) { seen.push(x); if (x === at) { hook(); throw outError; } } } catch (e) { seen.push(e.message); } return seen.join(); }
    function continueOuter(array, at, hook) { let seen = []; outer: for (let i = 0; i < 2; i++) { for (let x of array) { seen.push(x); if (x === at) { hook(); continue outer; } } } return seen.join(); }
    function breakOuter(array, at, hook) { let seen = []; outer: for (let i = 0; i < 2; i++) { for (let x of array) { seen.push(x); if (x === at) { hook(); break outer; } } } return seen.join(); }
    function finallyOut(array, at, hook) { let seen = []; try { for (let x of array) { seen.push(x); if (x === at) { hook(); return seen.join(); } } } finally { seen.push("finally"); log.push("finally " + seen.join()); } return seen.join(); }
    function runToEnd(array, at, hook) { let seen = []; for (let x of array) { seen.push(x); if (x === at) hook(); } return seen.join(); }
    function nested(array, at, hook) { let seen = []; for (let x of array) { for (let y of array) { seen.push(x * 10 + y); if (y === at) { hook(); break; } } if (x === at) break; } return seen.join(); }
    function destructure2(array, at, hook) { hook(); let [a, b] = array; return a + "," + b; }
    function destructureEmpty(array, at, hook) { hook(); let [] = array; return ""; }
    function destructureRest(array, at, hook) { hook(); let [a, ...r] = array; return a + "," + r.join(); }
    function throwDefault() { throw defaultError; }
    function destructureDefault(array, at, hook) { let [a, b = (hook(), "d")] = array; return a + "," + b; }
    function destructureThrow(array, at, hook) { try { let [a, b = (hook(), throwDefault())] = array; } catch (e) { return e.message; } return "no throw"; }

    function run(f, array, at, holder, calls) {
        const quietHooks = [() => { }, () => { }];
        for (let i = 0; i < calls; i++)
            f(array.slice(), at, quietHooks[i & 1]);
        let results = { quiet: log };
        log = [];
        results.result = f(array.slice(), at, () => installReturn(holders[holder]));
        results.log = log;
        results.iterators = seenIterators.length;
        results.distinctIterators = new Set(seenIterators).size;
        results.arrayIterators = seenIterators.every(iterator => typeof iterator === "object" && Object.getPrototypeOf(iterator) === ArrayIteratorPrototype);
        log = [];
        results.resultAfter = f(array.slice(), at, quietHooks[0]);
        results.logAfter = log;
        return results;
    }
`;

const afterTwo = 'return [object Array Iterator] true {"value":3,"done":false}';
const secondMissing = "[1, undefined, 3, 4]";
let scenarios = [
    ["breakOut", 2, "1,2", afterTwo],
    ["returnOut", 3, "1,2,3", 'return [object Array Iterator] true {"value":4,"done":false}'],
    ["throwOut", 1, "1,out", 'return [object Array Iterator] true {"value":2,"done":false}'],
    ["continueOuter", 4, "1,2,3,4,1,2,3,4", 'return [object Array Iterator] true {"done":true}|return [object Array Iterator] true {"done":true}'],
    ["breakOuter", 2, "1,2", afterTwo],
    ["finallyOut", 2, "1,2", afterTwo + "|finally 1,2,finally"],
    ["runToEnd", 2, "1,2,3,4", ""],
    ["nested", 2, "11,12,21,22", [afterTwo, afterTwo, afterTwo].join("|")],
    // These run their hook before the pattern is opened: opening sees an observable protocol.
    ["destructure2", 0, "1,2", afterTwo],
    ["destructureEmpty", 0, "", 'return [object Array Iterator] true {"value":1,"done":false}'],
    ["destructureRest", 0, "1,2,3,4", ""],
    // And these from the default value of the second element: the iterator object has to appear half way through the pattern.
    ["destructureDefault", 0, "1,d", afterTwo, secondMissing],
    ["destructureThrow", 0, "dflt", afterTwo, secondMissing],
];

// Enough calls for the Baseline JIT where the run line lowers its threshold. What makes the Array Iterator object is the
// same function in every tier; the other test has the optimized frames.
const calls = Math.min(testLoopCount, 12);

function relevant(name, log) { return (name === "finallyOut" ? log : log.filter(s => s.startsWith("return"))).join("|"); }

function inARealmOfItsOwn(name, at, expected, expectedLog, array, holder) {
    let realm = createGlobalObject();
    realm.eval(realmSource);
    let results = realm.eval(`run(${name}, ${array}, ${at}, ${holder}, ${calls})`);
    let message = name + ", holder " + holder;
    shouldBe(relevant("", results.quiet), "", message + " quiet");
    shouldBe(results.result, expected, message);
    shouldBe(relevant(name, results.log), expectedLog, message);
    shouldBe(results.iterators, results.log.filter(s => s.startsWith("return")).length, message + " iterators");
    shouldBe(results.distinctIterators, results.iterators, message + " distinct iterators");
    shouldBe(results.arrayIterators, true, message);
    shouldBe(results.resultAfter, expected, message + " after");
    shouldBe(relevant(name, results.logAfter), expectedLog, message + " after");
}
// This only drives the other realms.
noDFG(inARealmOfItsOwn);

for (let [name, at, expected, expectedLog, array = "[1, 2, 3, 4]"] of scenarios) {
    for (let holder = 0; holder < 3; holder++)
        inARealmOfItsOwn(name, at, expected, expectedLog, array, holder);
}
