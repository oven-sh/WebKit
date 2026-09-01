//@ requireOptions("--useJSThreads=1", "--thresholdForJITAfterWarmUp=50", "--thresholdForOptimizeAfterWarmUp=200")
// a[i] = a[i] + 1 in one block: local CSE rewrites the InBounds PutByVal into
// PutByValDirectResolved because the GetByVal already defined the same
// (base, index) location. A foreign push past the vector length segments the
// array in place; the flat-only GetButterfly of the read and then of the write
// each exit once with BadIndexingType, and the third DFG compile carries the
// segmented-butterfly bit on the PutByVal, which then compiles through the
// segmented-aware path with no storage child. That path must accept
// PutByValDirectResolved and take the ordinary InBounds arm.

function check(cond, msg) { if (!cond) throw new Error(msg); }

if (typeof Thread !== "function" || typeof numberOfDFGCompiles !== "function") {
    print("SKIP: needs jsc shell with Thread + numberOfDFGCompiles");
    quit(0);
}

function bumpInt(a, i) { a[i] = a[i] + 1; }
noInline(bumpInt);
function bumpDouble(a, i) { a[i] = a[i] + 0.5; }
noInline(bumpDouble);
function bumpObj(a, i) { a[i] = a[i].next; }
noInline(bumpObj);

function makeInt(i) { return [i, i + 1, i + 2, i + 3]; }
noInline(makeInt);
function makeDouble(i) { return [i + 0.5, i + 1.5, i + 2.5, i + 3.5]; }
noInline(makeDouble);
function makeObj(o) { return [o, o, o, o]; }
noInline(makeObj);

function segmentByForeignPush(a) {
    var before = a.length;
    var t = new Thread(() => { for (var i = 0; i < 64; i++) a.push(a[0]); return a.length; });
    check(t.join() === before + 64, "foreign push did not run");
}

// Returns the number of calls made. Stops once the segmented-aware compile is
// installed (two exits precede it) or after a generous cap.
function drive(f, a) {
    var calls = 0;
    for (var round = 0; round < 4000; round++) {
        for (var n = 0; n < 1000; n++)
            f(a, n & 3);
        calls += 1000;
        if (numberOfDFGCompiles(f) >= 3)
            break;
    }
    return calls;
}

var chain = { value: 0 };
chain.next = chain;
var ints = makeInt(1);
var doubles = makeDouble(1);
var objs = makeObj(chain);
segmentByForeignPush(ints);
segmentByForeignPush(doubles);
segmentByForeignPush(objs);

var intCalls = drive(bumpInt, ints);
var doubleCalls = drive(bumpDouble, doubles);
var objCalls = drive(bumpObj, objs);

for (var i = 0; i < 4; i++) {
    check(ints[i] === i + 1 + intCalls / 4, "ints[" + i + "] = " + ints[i] + " after " + intCalls + " calls");
    check(doubles[i] === i + 1.5 + doubleCalls / 8, "doubles[" + i + "] = " + doubles[i] + " after " + doubleCalls + " calls");
    check(objs[i] === chain, "objs[" + i + "] lost the store");
}
check(ints.length === 68 && doubles.length === 68 && objs.length === 68, "length changed");
check(numberOfDFGCompiles(bumpInt) >= 1, "bumpInt never reached DFG");
check(numberOfDFGCompiles(bumpDouble) >= 1, "bumpDouble never reached DFG");
check(numberOfDFGCompiles(bumpObj) >= 1, "bumpObj never reached DFG");
