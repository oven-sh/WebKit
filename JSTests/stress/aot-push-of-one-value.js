//@ requireOptions("--useImmutableIntrinsics=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTInlineFastPathsInLoops=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTLoopSplitting=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--verifyGC=1", "--scribbleFreeCells=1", "--useZombieMode=1", "--slowPathAllocsBetweenGCs=10")

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function throwsTypeError(f, what) {
    try {
        f();
    } catch (e) {
        if (e instanceof TypeError)
            return;
        throw new Error(what + ": threw " + String(e));
    }
    throw new Error(what + ": did not throw");
}
function remarksOf(f) {
    return typeof aotRemarks === "function" && isAOTCompiled(f) ? aotRemarks(f.name) : null;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
    }
}
function doesNotApply(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + f.name + ": " + remarks.join(" "));
    }
}

function pushes(array, value) { return array.push(value); }
function pushesAndDrops(array, value) { array.push(value); }
function pushesInt32(array, i) { return array.push(i | 0); }
function pushesTwo(array, first, second) { return array.push(first, second); }
function pushesNone(array) { return array.push(); }
function pushesByCall(array, value) { return Array.prototype.push.call(array, value); }
function lengthAround(array, value) { const before = array.length; array.push(value); return before * 1000 + array.length; }
function elementAround(array, value) { const index = array.length; const before = array[index]; array.push(value); return String(before) + ":" + array[index]; }
function otherObjectAround(o, array, value) { const before = o.kept; array.push(value); return before + o.kept; }
function pushesInTailPosition(array, value) { "use strict"; return array.push(value); }
const callEachOther = {
    push(n) { "use strict"; return n ? this.pop(n - 1) : "push"; },
    pop(n) { "use strict"; return n ? this.push(n - 1) : "pop"; },
};
function fills(n) {
    let array = [];
    for (let i = 0; i < n; ++i)
        array.push({ index: i });
    return array;
}
function fillsWithNumbers(n) {
    let array = [];
    for (let i = 0; i < n; ++i)
        array.push(i);
    return array;
}

for (let round = 0; round < 300; ++round) {
    let contiguous = ["a", "b"];
    check(pushes(contiguous, "c"), 3, "the length after a push onto strings");
    check(contiguous.join(), "a,b,c", "strings after a push");
    check(pushes(contiguous, round), 4, "a number pushed onto strings");
    check(contiguous[3], round, "the number pushed onto strings");

    let int32 = [1, 2, 3];
    check(pushesInt32(int32, 4), 4, "an int32 pushed onto a literal of int32");
    check(int32.join(), "1,2,3,4", "a literal of int32 after a push");
    check([1, 2, 3].join(), "1,2,3", "another literal with the same elements");
    check(pushes(int32, "five"), 5, "a string pushed onto int32");
    check(int32.join(), "1,2,3,4,five", "int32 that became values");

    let becomesDouble = [1, 2];
    check(pushes(becomesDouble, 2.5), 3, "a double pushed onto int32");
    check(becomesDouble[2], 2.5, "the double pushed onto int32");
    check(pushes(becomesDouble, 3), 4, "an int32 pushed onto doubles");
    check(becomesDouble[3], 3, "the int32 pushed onto doubles");
    check(pushes(becomesDouble, NaN), 5, "NaN pushed onto doubles");
    check(becomesDouble[4], NaN, "the NaN pushed onto doubles");
    check(pushes(becomesDouble, "s"), 6, "a string pushed onto doubles");
    check(becomesDouble.join(), "1,2,2.5,3,NaN,s", "doubles that became values");

    let undecided = [];
    check(pushes(undecided, undefined), 1, "undefined pushed onto an empty array");
    check(0 in undecided, true, "undefined that was pushed is an element");
    check(pushes(undecided, null), 2, "null pushed");
    check(undecided[1], null, "the null pushed");

    let holey = ["a", , "c"];
    check(pushes(holey, "d"), 4, "a push onto an array with a hole");
    check(1 in holey, false, "the hole remains");
    check(holey[3], "d", "the element after the hole");

    let shortened = ["a", "b", "c", "d"];
    shortened.length = 1;
    check(pushes(shortened, "z"), 2, "a push after the length was lowered");
    check(shortened.join(), "a,z", "elements after the length was lowered");
    check(2 in shortened, false, "what lay beyond the lowered length is gone");

    let lengthened = ["a"];
    lengthened.length = 5;
    check(pushes(lengthened, "z"), 6, "a push after the length was raised");
    check(lengthened[5], "z", "the element after the raised length");
    check(3 in lengthened, false, "a raised length makes holes");

    pushesAndDrops(contiguous, "dropped");
    check(contiguous.length, 5, "a push whose result is dropped");
    check(contiguous[4], "dropped", "the element of a push whose result is dropped");

    check(lengthAround(["a", "b"], "c"), 2003, "the length read before and after a push");
    check(lengthAround([], round), 1, "the length of an empty array read before and after a push");
    check(elementAround(["a", "b"], "c"), "undefined:c", "the element read before and after it is pushed");
    check(otherObjectAround({ kept: 21 }, ["a"], "b"), 42, "a property of another object read before and after a push");
    check(pushesInTailPosition(["a", "b"], "c"), 3, "a push in tail position");
    check(pushesInTailPosition({ push(value) { return "own:" + value; } }, round), "own:" + round, "a method called push, in tail position");
    check(pushesTwo(["a"], "b", "c"), 3, "two values");
    check(pushesNone(["a"]), 1, "no value");
    check(pushesByCall(["a"], "b"), 2, "by call");
}

for (let capacity = 0; capacity <= 100; ++capacity) {
    let array = [];
    for (let i = 0; i < capacity; ++i)
        check(pushes(array, "e" + i), i + 1, "the length while growing");
    check(array.length, capacity, "the length after growing");
    for (let i = 0; i < capacity; ++i)
        check(array[i], "e" + i, "an element after growing");
}

check(callEachOther.push(300001), "pop", "two methods, one of them called push, that call each other in tail position");
check(callEachOther.pop(300001), "push", "the same, begun with the other");
throwsTypeError(() => pushes(Object.freeze(["a"]), "b"), "a frozen array");
throwsTypeError(() => pushes(Object.seal(["a"]), "b"), "a sealed array");
throwsTypeError(() => pushes(Object.preventExtensions(["a"]), "b"), "an array that is not extensible");
throwsTypeError(() => pushes(Object.defineProperty(["a"], "length", { writable: false }), "b"), "a length that is not writable");
throwsTypeError(() => pushes(null, "b"), "null");
throwsTypeError(() => pushes({ }, "b"), "an object without push");

class Derived extends Array { push(value) { return super.push(value + "!"); } }
let derived = new Derived;
check(pushes(derived, "a"), 1, "a subclass with its own push");
check(derived[0], "a!", "the element pushed by a subclass");
class Plain extends Array { }
let plain = new Plain;
check(pushes(plain, "a"), 1, "a subclass without its own push");
check(plain[0], "a", "the element pushed onto a subclass");

let withOwnPush = ["a"];
withOwnPush.push = function (value) { return "own:" + value; };
check(pushes(withOwnPush, "b"), "own:b", "an array with its own push");
check(withOwnPush.length, 1, "an own push that does not push");

let otherPrototype = Object.setPrototypeOf(["a"], { push(value) { return "inherited:" + value; } });
check(pushes(otherPrototype, "b"), "inherited:b", "an array with another prototype");

let withProperty = ["a"];
withProperty.extra = 1;
check(pushes(withProperty, "b"), 2, "an array with a named property");
check(withProperty.join() + withProperty.extra, "a,b1", "elements and the named property");

let arrayLike = { length: 2, push: Array.prototype.push };
check(pushes(arrayLike, "c"), 3, "an array-like object");
check(arrayLike[2] + arrayLike.length, "c3", "the element of an array-like object");

let collector = { items: 0, push(value) { return this.items += value; } };
check(pushes(collector, 5), 5, "an object with a method called push");

let old = [];
for (let i = 0; i < 20; ++i)
    old.push("warm" + i);
gc();
gc();
for (let i = 0; i < 2000; ++i) {
    pushes(old, { young: i });
    if (!(i % 250))
        gc();
}
for (let i = 0; i < 2000; ++i)
    check(old[20 + i].young, i, "a young object pushed onto an old array");

let filled = fills(100000);
check(filled.length, 100000, "many pushes");
for (let i = 0; i < 100000; i += 997)
    check(filled[i].index, i, "an element of many pushes");
let numbers = fillsWithNumbers(100000);
check(numbers.length, 100000, "many pushes of numbers");
for (let i = 0; i < 100000; i += 997)
    check(numbers[i], i, "a number of many pushes");

function oneOfApplies(f, ...patterns) {
    let remarks = remarksOf(f);
    if (remarks && !remarks.some(remark => patterns.some(pattern => matches(remark, pattern))))
        throw new Error("none of " + patterns.join(", ") + " applies to " + f.name + ": " + remarks.join(" "));
}
function readsProperty(o) { return o.property; }
readsProperty({ property: 1 });
for (let f of [pushes, pushesAndDrops, pushesInt32, pushesByCall, lengthAround, elementAround, otherObjectAround])
    applies(f, "pushes-inline");
for (let f of [fills, fillsWithNumbers])
    oneOfApplies(f, "pushes-inline", "guarded-intrinsic-call");
for (let f of [pushesTwo, pushesNone, pushesInTailPosition, readsProperty])
    doesNotApply(f, "pushes-inline");
