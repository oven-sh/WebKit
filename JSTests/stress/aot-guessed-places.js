//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTOperationCounters=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}

function makesPoint(x, y) { return { pointX: x, pointY: y, pointZ: x + y }; }
function readsThirdOfPoint(o) { return o.pointZ; }
function readsTwoOfPoint(o) { return o.pointX * 100 + o.pointY; }
function writesSecondOfPoint(o, v) { o.pointY = v; return o.pointY; }
function readsAllOfPoint(o) { return o.pointX * 10000 + o.pointY * 100 + o.pointZ; }
function writesAllOfPoint(o, v) { o.pointZ = v; o.pointY = v + 1; o.pointX = v + 2; }

function makesFirstOrder(a, b) { return { swappedA: a, swappedB: b }; }
function makesSecondOrder(a, b) { return { swappedB: b, swappedA: a }; }
function readsSwapped(o) { return o.swappedA * 10 + o.swappedB; }
function readsAndWritesSwapped(o) { o.swappedA = o.swappedB; return o.swappedA; }

function makesShort(v) { return { sharedName: v, onlyInShort: 1 }; }
function makesLong(v) { return { onlyInLong: 2, sharedName: v }; }
function readsSharedAlone(o) { return o.sharedName; }
function readsSharedWithShort(o) { return o.sharedName + o.onlyInShort; }
function readsSharedWithLong(o) { return o.sharedName + o.onlyInLong; }
function readsAndWritesShort(o) { o.onlyInShort = o.sharedName; return o.onlyInShort; }
function readsAndWritesLong(o) { o.onlyInLong = o.sharedName; return o.onlyInLong; }

function readsNameNobodyIsBornWith(o) { return o.pointX + (o.neverGivenAtBirth === undefined ? 0 : 1); }

function Counter(start) {
    this.count = start;
    this.step = helper();
    this.limit = start > 5 ? 100 : 10;
    this.label = "c";
}
function helper() { return 2; }
Counter.prototype.advance = function () { this.count += this.step; return this.count; };
function readsFourthOfCounter(o) { return o.label; }
function readsAndCallsCounter(o) { return o.advance() + o.limit; }
function readsThreeOfCounter(o) { return o.step + o.limit + o.label; }

class Holder {
    first = 1;
    second;
    #hidden = 3;
    fourth = [4];
    constructor(v) { this.fifth = v; }
    hidden() { return this.#hidden; }
}
function readsFieldsOfHolder(o) { return o.first + o.fourth[0] + o.fifth; }

function makesBitByBit(v) {
    let o = {};
    o.builtFirst = v;
    o.builtSecond = v + 1;
    return o;
}
function readsBuiltSecond(o) { return o.builtSecond; }

function makesWithConditionalValue(v) { return { chosenA: v, chosenB: v > 2 ? 1 : 2, chosenC: 3 }; }
function readsChosenC(o) { return o.chosenC; }
function readsAllChosen(o) { return o.chosenA * 100 + o.chosenB * 10 + o.chosenC; }

function manyNames() {
    return {
        n0: 0, n1: 1, n2: 2, n3: 3, n4: 4, n5: 5, n6: 6, n7: 7, n8: 8, n9: 9, n10: 10, n11: 11, n12: 12, n13: 13, n14: 14, n15: 15,
        n16: 16, n17: 17, n18: 18, n19: 19, n20: 20, n21: 21, n22: 22, n23: 23, n24: 24, n25: 25,
    };
}
function readsLastSlotWithName(o) { return o.n23; }
function readsFirstSlotWithoutName(o) { return o.n24; }
function readsLastThreeSlotsWithNames(o) { return o.n21 * 10000 + o.n22 * 100 + o.n23; }
function readsFirstSlotsWithoutNames(o) { return o.n24 + o.n25 + o.n24; }

function makesComputedFirst(i) { return { ["computed" + i]: 1, behindComputed: i }; }
function readsBehindComputed(o) { return o.behindComputed; }
function makesComputedInTheMiddle(i) { return { aheadOfComputed: 1, ["middle" + i]: 2, farBehindComputed: i }; }
function readsAroundComputed(o) { return o.aheadOfComputed + o.farBehindComputed; }
function makesWithIndexKeys(i) { return { 0: 1, behindIndex: i, 7: 2, farBehindIndex: i }; }
function readsBehindIndexKeys(o) { return o.behindIndex + o.farBehindIndex; }
function makesWithSpread(e, i) { return { aheadOfSpread: 1, ...e, behindSpread: i }; }
function readsAheadOfSpread(o) { return o.aheadOfSpread; }
function readsBehindSpread(o) { return o.behindSpread; }
function makesWithAccessor(i) { return { aheadOfAccessor: 1, get accessor() { return 2; }, behindAccessor: i }; }
function readsAheadOfAccessor(o) { return o.aheadOfAccessor; }
function readsBehindAccessor(o) { return o.behindAccessor; }
function makesHandedOut(i) {
    let o = {};
    o.aheadOfCall = i;
    addsProperty(o);
    o.behindCall = i;
    return o;
}
function addsProperty(o) { o.addedByCall = 0; }
function readsBehindCall(o) { return o.behindCall; }
function makesPlainThree(i) { return { sameInBoth: 1, onlyInPlain: 2, thirdInPlain: i }; }
function makesSpreadThree(e, i) { return { sameInBoth: 1, ...e, thirdInPlain: i }; }
function readsThirdInPlain(o) { return o.thirdInPlain; }

function definesClasses() {
    class Root { constructor(a) { this.rootFirst = a; this.rootSecond = 2; } }
    class Middle extends Root { middleField = 7; constructor(a) { super(a); this.middleStore = a; } }
    class Leaf extends Middle { constructor(a) { super(a); this.leafStore = a; } }
    class Silent extends Middle { }
    class SilentWithField extends Root { silentField = 1; }
    class RepeatsName extends Root { constructor(a) { super(a); this.rootSecond = 5; this.behindRepeated = a; } }
    function Old(a) { this.oldFirst = a; }
    class OfOld extends Old { constructor(a) { super(a); this.behindOld = a; } }
    class OfError extends Error { constructor(m) { super(m); this.behindError = 1; } }
    class OfArray extends Array { constructor() { super(); this.behindArray = 1; } }
    class OfNull extends null {
        constructor() {
            let o = Object.create(new.target.prototype);
            o.storedOnOfNull = 1;
            return o;
        }
    }
    class ReturnsAnother {
        constructor(a) {
            this.neverSeen = a;
            return { elsewhereFirst: 1, elsewhereSecond: 2, elsewhereThird: 3 };
        }
    }
    class OfReturnsAnother extends ReturnsAnother { constructor(a) { super(a); this.landsElsewhere = a; } }
    class StoresInLoop {
        constructor(a) {
            this.aheadOfLoop = a;
            for (let i = 0; i < a % 3; i++)
                this["looped" + i] = i;
            this.behindLoop = a;
        }
    }
    class OfStoresInLoop extends StoresInLoop { constructor(a) { super(a); this.behindUnknownSlots = a; } }
    let mixesIn = Base => class extends Base { constructor(a) { super(a); this.behindUnknownParent = a; } };
    return { Root, Middle, Leaf, Silent, SilentWithField, RepeatsName, OfOld, OfError, OfArray, OfNull, OfReturnsAnother, OfStoresInLoop, Mixed: mixesIn(Root) };
}
let classes = definesClasses();
function readsAllOfLeaf(o) { return [o.rootFirst, o.rootSecond, o.middleField, o.middleStore, o.leafStore].join(); }
function readsMiddleStore(o) { return o.middleStore; }
function readsSilentField(o) { return o.silentField; }
function readsBehindRepeated(o) { return o.rootSecond * 10 + o.behindRepeated; }
function readsBehindOld(o) { return o.behindOld; }
function readsBehindError(o) { return o.behindError; }
function readsBehindArray(o) { return o.behindArray; }
function readsStoredOnOfNull(o) { return o.storedOnOfNull; }
function readsLandsElsewhere(o) { return o.landsElsewhere; }
function readsBehindUnknownParent(o) { return o.behindUnknownParent; }
function readsAheadOfLoop(o) { return o.aheadOfLoop; }
function readsBehindUnknownSlots(o) { return o.behindLoop + o.behindUnknownSlots; }

function inAnotherOrder(x, y) {
    let o = {};
    o["point" + "Z"] = x + y;
    o["point" + "Y"] = y;
    o["point" + "X"] = x;
    return o;
}
let withGetter = { get pointZ() { return 77; }, pointX: 1, pointY: 2 };
let inherits = Object.create(makesPoint(5, 6));

for (let i = 0; i < 200; i++) {
    check(readsThirdOfPoint(makesPoint(i, 1)), i + 1, "the third property");
    check(readsTwoOfPoint(makesPoint(i, 2)), i * 100 + 2, "two properties");
    check(writesSecondOfPoint(makesPoint(i, 2), i), i, "a store");
    check(readsThirdOfPoint(inAnotherOrder(i, 3)), i + 3, "the same names in other slots");
    check(readsTwoOfPoint(inAnotherOrder(i, 3)), i * 100 + 3, "two of the same names in other slots");
    check(writesSecondOfPoint(inAnotherOrder(i, 3), i), i, "a store to another slot");
    check(readsThirdOfPoint(withGetter), 77, "a getter");
    check(readsThirdOfPoint(inherits), 11, "an inherited property");
    check(readsThirdOfPoint({ other: 1 }), undefined, "an absent property");
    check(readsThirdOfPoint("text"), undefined, "a primitive");
    check(readsThirdOfPoint(i), undefined, "a number");
    check(readsSwapped(makesFirstOrder(i, 1)), i * 10 + 1, "one order");
    check(readsSwapped(makesSecondOrder(i, 2)), i * 10 + 2, "the other order");
    check(readsSharedAlone(makesShort(i)) + readsSharedAlone(makesLong(i)), 2 * i, "a name in two slots");
    check(readsSharedWithShort(makesShort(i)), i + 1, "told apart by another name");
    check(readsSharedWithLong(makesLong(i)), i + 2, "told apart by another name");
    check(readsNameNobodyIsBornWith(makesPoint(i, 0)), i, "a name nobody is born with");
    check(readsFourthOfCounter(new Counter(i)), "c", "a constructor's fourth store");
    check(readsAndCallsCounter(new Counter(i)), i + 2 + (i > 5 ? 100 : 10), "a method and a property");
    check(readsFieldsOfHolder(new Holder(i)), 5 + i, "class fields");
    check(new Holder(i).hidden(), 3, "a private field");
    check(readsBuiltSecond(makesBitByBit(i)), i + 1, "an object put together by stores");
    check(readsChosenC(makesWithConditionalValue(i)), 3, "behind a conditional value");
    check(readsLastSlotWithName(manyNames()) + readsFirstSlotWithoutName(manyNames()), 47, "high slots");
    check(readsBehindSpread(makesWithSpread(i % 2 ? { x: 1, y: 2 } : {}, i)), i, "behind a spread");
    check(readsAheadOfSpread(makesWithSpread({ x: 1 }, i)), 1, "ahead of a spread");
    check(readsAheadOfSpread(makesWithSpread({ aheadOfSpread: 5 }, i)), 5, "overwritten by a spread");
    check(readsBehindAccessor(makesWithAccessor(i)) + readsAheadOfAccessor(makesWithAccessor(i)), i + 1, "around an accessor");
    check(readsBehindCall(makesHandedOut(i)), i, "behind a call that was given the object");
    check(readsThirdInPlain(makesSpreadThree(i % 2 ? { x: 1, y: 2 } : {}, i)), i, "a name that another literal has behind a spread");
    check(readsBehindComputed({ ["behind" + "Computed"]: i, behindComputed: i + 1 }), i + 1, "a computed key that repeats a name");
    check(readsBehindComputed({ [i]: 0, behindComputed: i }), i, "a computed key that is an index");
    check(readsAllOfLeaf(new classes.Leaf(i)), [i, 2, 7, i, i].join(), "a chain of three classes");
    check(readsMiddleStore(new classes.Middle(i)) + readsMiddleStore(new classes.Leaf(i)) + readsMiddleStore(new classes.Silent(i)), 3 * i, "a store of the middle class");
    check(readsSilentField(new classes.SilentWithField(i)), 1, "a field of a class without a written constructor");
    check(readsBehindRepeated(new classes.RepeatsName(i)), 50 + i, "behind a store to a name of the parent");
    check(readsBehindOld(new classes.OfOld(i)), i, "a class that extends a function");
    check(readsBehindError(new classes.OfError("m")), 1, "a class that extends Error");
    check(readsBehindArray(new classes.OfArray), 1, "a class that extends Array");
    check(readsStoredOnOfNull(new classes.OfNull), 1, "a class that extends null");
    check(readsLandsElsewhere(new classes.OfReturnsAnother(i)), i, "a parent that returns another object");
    check(new classes.OfReturnsAnother(i).elsewhereThird, 3, "the object that the parent returned");
    check(new classes.OfReturnsAnother(i).neverSeen, undefined, "the object that the parent gave up");
    check(readsBehindUnknownParent(new classes.Mixed(i)), i, "a parent that is a parameter");
    check(readsAheadOfLoop(new classes.OfStoresInLoop(i)) + readsBehindUnknownSlots(new classes.OfStoresInLoop(i)), 3 * i, "a parent that stores in a loop");
}
{
    let countOf = detail => typeof aotOperationCount === "function" && aotOperationCount("Family::guard:" + detail) || 0;
    let exits = () => countOf("exits-with-another-number") + countOf("exits-without-number") + countOf("exits-not-a-cell");
    let before = [countOf("passes"), exits()];
    for (let i = 0; i < 100; i++) {
        check(readsAllOfPoint(makesPoint(i % 50, 1)), (i % 50) * 10000 + 100 + i % 50 + 1, "every property of an object as it was born");
        let point = makesPoint(i, 2);
        writesAllOfPoint(point, i);
        check([point.pointX, point.pointY, point.pointZ].join(), [i + 2, i + 1, i].join(), "stores to every property of an object as it was born");
        check(readsAndWritesShort(makesShort(i)) + readsAndWritesLong(makesLong(i)), 2 * i, "one name in two slots, told apart by another name");
        check(readsAllChosen(makesWithConditionalValue(i % 9)), (i % 9) * 100 + (i % 9 > 2 ? 10 : 20) + 3, "around a conditional value");
        check(readsLastThreeSlotsWithNames(manyNames()), 212223, "the last slots that may be guessed");
        check(readsFirstSlotsWithoutNames(manyNames()), 73, "the first slots that may not be guessed");
        check(readsAndWritesSwapped(makesFirstOrder(i, 1)) + readsAndWritesSwapped(makesSecondOrder(i, 2)), 3, "stores to two orders");
        check(readsThreeOfCounter(new Counter(i)), 2 + (i > 5 ? 100 : 10) + "c", "three stores of a constructor behind a call");
        check(readsBehindComputed(makesComputedFirst(i)), i, "behind a computed key");
        check(readsAroundComputed(makesComputedInTheMiddle(i)), i + 1, "around a computed key");
        check(readsBehindIndexKeys(makesWithIndexKeys(i)), 2 * i, "behind index keys");
        check(readsThirdInPlain(makesPlainThree(i)), i, "a name that another literal has behind a spread");
    }
    if (jscOptions().useAOTOperationCounters && jscOptions().useAOTDataStubs) {
        check(countOf("passes") - before[0] >= 600, true, "the guards pass objects as they were born");
        check(exits() - before[1], 0, "no guard fails on an object as it was born");
        before = exits();
        for (let i = 0; i < 100; i++) {
            check(readsAllOfPoint(inAnotherOrder(i % 50, 3)), (i % 50) * 10000 + 300 + i % 50 + 3, "the same names in other slots, counted");
            check(readsAllOfPoint(i), NaN, "a number, counted");
            let point = inAnotherOrder(i, 3);
            writesAllOfPoint(point, i);
            check([point.pointX, point.pointY, point.pointZ].join(), [i + 2, i + 1, i].join(), "stores to the same names in other slots, counted");
        }
        check(exits() - before, 300, "a guard fails on everything else");
    }
}
let deleted = makesPoint(1, 2);
delete deleted.pointX;
check(readsThirdOfPoint(deleted), 3, "after a deletion");
let frozen = Object.freeze(makesPoint(1, 2));
check(writesSecondOfPoint(frozen, 9), 2, "a store to a frozen object");

if (aotRemarks("readsThirdOfPoint") && jscOptions().useAOTDataStubs) {
    let has = (name, remark) => aotRemarks(name).includes(remark);
    let applies = (name, remark) => {
        if (!has(name, remark))
            throw new Error(remark + " does not apply to " + name + ": " + aotRemarks(name).join(" | "));
    };
    let doesNotApply = (name, remark) => {
        if (has(name, remark))
            throw new Error(remark + " applies to " + name + ": " + aotRemarks(name).join(" | "));
    };
    let isGuessed = (name, ...properties) => {
        for (let property of properties)
            applies(name, "guessed-place:" + property);
        for (let reason of ["no-shape", "disagree", "slot-too-high", "no-family", "same-names-born-in-another-module"])
            doesNotApply(name, "no-guess:" + reason);
    };
    let isNotGuessed = (name, reason, ...properties) => {
        applies(name, "no-guess:" + reason);
        for (let property of properties)
            doesNotApply(name, "guessed-place:" + property);
    };
    let hasShapeWithoutFamily = (name, ...properties) => {
        isNotGuessed(name, "no-family", ...properties);
        doesNotApply(name, "no-guess:disagree");
    };
    isGuessed("readsThirdOfPoint", "pointZ");
    isGuessed("readsTwoOfPoint", "pointX", "pointY");
    isGuessed("writesSecondOfPoint", "pointY");
    isGuessed("readsSharedWithShort", "sharedName", "onlyInShort");
    isGuessed("readsSharedWithLong", "sharedName", "onlyInLong");
    isGuessed("readsChosenC", "chosenC");
    isGuessed("readsLastSlotWithName", "n23");
    isGuessed("readsThirdInPlain", "thirdInPlain");
    isNotGuessed("readsSwapped", "disagree", "swappedA", "swappedB");
    isNotGuessed("readsSharedAlone", "disagree", "sharedName");
    isNotGuessed("readsNameNobodyIsBornWith", "no-shape", "pointX");
    isNotGuessed("readsFirstSlotWithoutName", "slot-too-high", "n24");
    for (let [name, ...properties] of [
        ["readsFourthOfCounter", "label"], ["readsFieldsOfHolder", "first", "fourth", "fifth"], ["readsBuiltSecond", "builtSecond"],
        ["readsBehindComputed", "behindComputed"], ["readsAroundComputed", "aheadOfComputed", "farBehindComputed"], ["readsBehindIndexKeys", "behindIndex", "farBehindIndex"],
        ["readsAheadOfSpread", "aheadOfSpread"], ["readsAheadOfAccessor", "aheadOfAccessor"],
        ["readsMiddleStore", "middleStore"], ["readsSilentField", "silentField"], ["readsBehindRepeated", "rootSecond", "behindRepeated"], ["readsBehindOld", "behindOld"],
        ["readsLandsElsewhere", "landsElsewhere"], ["readsAheadOfLoop", "aheadOfLoop"],
    ]) {
        hasShapeWithoutFamily(name, ...properties);
        doesNotApply(name, "no-guess:no-shape");
    }
    hasShapeWithoutFamily("readsAndCallsCounter", "limit", "advance");
    hasShapeWithoutFamily("readsAllOfLeaf", "rootFirst", "rootSecond", "middleField", "middleStore", "leafStore");
    for (let [name, ...properties] of [
        ["readsBehindSpread", "behindSpread"], ["readsBehindAccessor", "behindAccessor"], ["readsBehindCall", "behindCall"],
        ["readsBehindError", "behindError"], ["readsBehindArray", "behindArray"], ["readsStoredOnOfNull", "storedOnOfNull"],
        ["readsBehindUnknownParent", "behindUnknownParent"], ["readsBehindUnknownSlots", "behindLoop", "behindUnknownSlots"],
    ]) {
        isNotGuessed(name, "no-shape", ...properties);
        doesNotApply(name, "no-guess:no-family");
    }
    isGuessed("readsAllOfPoint", "pointX", "pointY", "pointZ");
    isGuessed("writesAllOfPoint", "pointX", "pointY", "pointZ");
    isGuessed("readsAndWritesShort", "sharedName", "onlyInShort");
    isGuessed("readsAndWritesLong", "sharedName", "onlyInLong");
    isGuessed("readsAllChosen", "chosenA", "chosenB", "chosenC");
    isGuessed("readsLastThreeSlotsWithNames", "n21", "n22", "n23");
    for (let [name, ...guards] of [
        ["readsAllOfPoint", "read:pointX", "read:pointY", "read:pointZ"], ["writesAllOfPoint", "store:pointX", "store:pointY", "store:pointZ"],
        ["readsAndWritesShort", "read:sharedName", "store:onlyInShort", "read:onlyInShort"], ["readsAndWritesLong", "read:sharedName", "store:onlyInLong", "read:onlyInLong"],
        ["readsAllChosen", "read:chosenA", "read:chosenB", "read:chosenC"], ["readsLastThreeSlotsWithNames", "read:n21", "read:n22", "read:n23"],
    ]) {
        applies(name, "guards-over-whole-function");
        for (let guard of guards)
            applies(name, "family-guards-" + guard);
    }
    isNotGuessed("readsAndWritesSwapped", "disagree", "swappedA", "swappedB");
    isNotGuessed("readsFirstSlotsWithoutNames", "slot-too-high", "n24", "n25");
    hasShapeWithoutFamily("readsThreeOfCounter", "step", "limit", "label");
    for (let name of ["readsAndWritesSwapped", "readsFirstSlotsWithoutNames", "readsThreeOfCounter", "readsFieldsOfHolder", "readsAllOfLeaf", "readsTwoOfPoint"]) {
        doesNotApply(name, "guards-over-whole-function");
        applies(name, "no-guards-over-whole-function:too-few-places");
    }
}
