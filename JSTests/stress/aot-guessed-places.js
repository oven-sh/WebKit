//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}

function makesPoint(x, y) { return { pointX: x, pointY: y, pointZ: x + y }; }
function readsThirdOfPoint(o) { return o.pointZ; }
function readsTwoOfPoint(o) { return o.pointX * 100 + o.pointY; }
function writesSecondOfPoint(o, v) { o.pointY = v; return o.pointY; }

function makesFirstOrder(a, b) { return { swappedA: a, swappedB: b }; }
function makesSecondOrder(a, b) { return { swappedB: b, swappedA: a }; }
function readsSwapped(o) { return o.swappedA * 10 + o.swappedB; }

function makesShort(v) { return { sharedName: v, onlyInShort: 1 }; }
function makesLong(v) { return { onlyInLong: 2, sharedName: v }; }
function readsSharedAlone(o) { return o.sharedName; }
function readsSharedWithShort(o) { return o.sharedName + o.onlyInShort; }
function readsSharedWithLong(o) { return o.sharedName + o.onlyInLong; }

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

function manyNames() {
    return {
        n0: 0, n1: 1, n2: 2, n3: 3, n4: 4, n5: 5, n6: 6, n7: 7, n8: 8, n9: 9, n10: 10, n11: 11, n12: 12, n13: 13, n14: 14, n15: 15,
        n16: 16, n17: 17, n18: 18, n19: 19, n20: 20, n21: 21, n22: 22, n23: 23, n24: 24, n25: 25,
    };
}
function readsLastSlotWithName(o) { return o.n23; }
function readsFirstSlotWithoutName(o) { return o.n24; }

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

function readsNameBornNowhere(o) { return o.bornNowhereButRead; }
function writesNameBornNowhere(o) { o.bornNowhereButWritten = 2; }
function asksForNameBornNowhere(o) { return "bornNowhereButAskedFor" in o; }

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
    let countOf = detail => typeof aotOperationCount === "function" && aotOperationCount("operationAOTCountGuessedPlace:" + detail) || 0;
    let before = [countOf("hit"), countOf("another-slot")];
    for (let i = 0; i < 100; i++) {
        check(readsBehindComputed(makesComputedFirst(i)), i, "behind a computed key");
        check(readsAroundComputed(makesComputedInTheMiddle(i)), i + 1, "around a computed key");
        check(readsBehindIndexKeys(makesWithIndexKeys(i)), 2 * i, "behind index keys");
        check(readsThirdInPlain(makesPlainThree(i)), i, "a name that another literal has behind a spread");
    }
    if (countOf("hit") > before[0]) {
        check(countOf("hit") - before[0] >= 500, true, "guessed reads of objects as they were born hit");
        check(countOf("another-slot") - before[1], 0, "guessed reads of objects as they were born that find the name in another slot");
    }
    let { Leaf, Silent, SilentWithField, RepeatsName, OfOld, OfReturnsAnother } = classes;
    before = [countOf("hit"), countOf("another-slot")];
    for (let i = 0; i < 100; i++) {
        readsAllOfLeaf(new Leaf(i));
        readsMiddleStore(new Silent(i));
        readsSilentField(new SilentWithField(i));
        readsBehindRepeated(new RepeatsName(i));
        readsBehindOld(new OfOld(i));
    }
    if (countOf("hit") > before[0]) {
        check(countOf("hit") - before[0] >= 1000, true, "guessed reads of instances of derived classes hit");
        check(countOf("another-slot") - before[1], 0, "guessed reads of instances of derived classes that find the name in another slot");
        before = countOf("hit");
        for (let i = 0; i < 100; i++)
            readsLandsElsewhere(new OfReturnsAnother(i));
        check(countOf("hit") - before, 0, "guessed reads of what a parent returned instead hit");
    }
}
{
    let countOf = detail => typeof aotOperationCount === "function" && aotOperationCount("propertyNameIDIfKnown:" + detail) || 0;
    let before = [countOf("known"), countOf("unknown")];
    let parsed = JSON.parse('{"bornNowhereButRead":1,"bornNowhereButWritten":0,"bornNowhereButAskedFor":3}');
    let namesAreLookedUp = countOf("known") + countOf("unknown") > before[0] + before[1];
    if (namesAreLookedUp) {
        check(countOf("known") - before[0], 3, "names that the program only reads, writes or asks for and that have an ID");
        check(countOf("unknown") - before[1], 0, "names that the program only reads, writes or asks for and that have no ID");
    }
    writesNameBornNowhere(parsed);
    check(readsNameBornNowhere(parsed) + parsed.bornNowhereButWritten, 3, "names that come from elsewhere");
    check(asksForNameBornNowhere(parsed), true, "a name that comes from elsewhere and is asked for");
    before = [countOf("known"), countOf("unknown")];
    let unnamed = JSON.parse('{"inNoCodeAtAll":1,"7":2}');
    if (namesAreLookedUp) {
        check(countOf("known") - before[0], 0, "names that stand in no code and that have an ID");
        check(countOf("unknown") - before[1] > 0, true, "a name that stands in no code has no ID");
    }
    check(unnamed["inNoCode" + "AtAll"] + unnamed[7], 3, "names that stand in no code");
}
let deleted = makesPoint(1, 2);
delete deleted.pointX;
check(readsThirdOfPoint(deleted), 3, "after a deletion");
let frozen = Object.freeze(makesPoint(1, 2));
check(writesSecondOfPoint(frozen, 9), 2, "a store to a frozen object");

if (aotRemarks("readsThirdOfPoint") && aotRemarks("readsThirdOfPoint").some(remark => remark.startsWith("guessed-place") || remark.startsWith("no-guess"))) {
    let has = (name, remark) => aotRemarks(name).includes(remark);
    let applies = (name, remark) => {
        if (!has(name, remark))
            throw new Error(remark + " does not apply to " + name + ": " + aotRemarks(name).join(" | "));
    };
    let doesNotApply = (name, remark) => {
        if (has(name, remark))
            throw new Error(remark + " applies to " + name + ": " + aotRemarks(name).join(" | "));
    };
    applies("readsThirdOfPoint", "guessed-place:pointZ");
    applies("readsTwoOfPoint", "guessed-place:pointX");
    applies("readsTwoOfPoint", "guessed-place:pointY");
    applies("readsSharedWithShort", "guessed-place:sharedName");
    applies("readsSharedWithLong", "guessed-place:sharedName");
    applies("readsFourthOfCounter", "guessed-place:label");
    applies("readsAndCallsCounter", "guessed-place:limit");
    applies("readsFieldsOfHolder", "guessed-place:first");
    applies("readsFieldsOfHolder", "guessed-place:fourth");
    applies("readsFieldsOfHolder", "guessed-place:fifth");
    applies("readsBuiltSecond", "guessed-place:builtSecond");
    applies("readsChosenC", "guessed-place:chosenC");
    applies("readsLastSlotWithName", "guessed-place:n23");
    applies("readsBehindComputed", "guessed-place:behindComputed");
    applies("readsAroundComputed", "guessed-place:aheadOfComputed");
    applies("readsAroundComputed", "guessed-place:farBehindComputed");
    applies("readsBehindIndexKeys", "guessed-place:behindIndex");
    applies("readsBehindIndexKeys", "guessed-place:farBehindIndex");
    applies("readsAheadOfSpread", "guessed-place:aheadOfSpread");
    applies("readsAheadOfAccessor", "guessed-place:aheadOfAccessor");
    applies("readsThirdInPlain", "guessed-place:thirdInPlain");
    applies("readsBehindSpread", "no-guess:no-shape");
    applies("readsBehindAccessor", "no-guess:no-shape");
    applies("readsBehindCall", "no-guess:no-shape");
    doesNotApply("readsBehindSpread", "guessed-place:behindSpread");
    doesNotApply("readsBehindAccessor", "guessed-place:behindAccessor");
    doesNotApply("readsBehindCall", "guessed-place:behindCall");
    doesNotApply("readsThirdInPlain", "no-guess:disagree");
    for (let name of ["rootFirst", "rootSecond", "middleField", "middleStore", "leafStore"])
        applies("readsAllOfLeaf", "guessed-place:" + name);
    applies("readsSilentField", "guessed-place:silentField");
    applies("readsBehindRepeated", "guessed-place:behindRepeated");
    applies("readsBehindOld", "guessed-place:behindOld");
    applies("readsLandsElsewhere", "guessed-place:landsElsewhere");
    applies("readsMiddleStore", "guessed-place:middleStore");
    applies("readsBehindError", "no-guess:no-shape");
    applies("readsBehindArray", "no-guess:no-shape");
    applies("readsStoredOnOfNull", "no-guess:no-shape");
    applies("readsBehindUnknownParent", "no-guess:no-shape");
    applies("readsAheadOfLoop", "guessed-place:aheadOfLoop");
    applies("readsBehindUnknownSlots", "no-guess:no-shape");
    doesNotApply("readsBehindUnknownSlots", "guessed-place:behindLoop");
    doesNotApply("readsBehindUnknownSlots", "guessed-place:behindUnknownSlots");
    doesNotApply("readsBehindError", "guessed-place:behindError");
    doesNotApply("readsBehindArray", "guessed-place:behindArray");
    doesNotApply("readsStoredOnOfNull", "guessed-place:storedOnOfNull");
    doesNotApply("readsBehindUnknownParent", "guessed-place:behindUnknownParent");
    applies("readsThirdOfPoint", "guessed-place-of-one-shape:pointZ");
    applies("readsAllOfLeaf", "guessed-place-of-one-shape:leafStore");
    applies("readsSharedWithShort", "guessed-place-of-one-shape:sharedName");
    doesNotApply("readsMiddleStore", "guessed-place-of-one-shape:middleStore");
    doesNotApply("readsSwapped", "guessed-place-of-one-shape:swappedA");
    applies("readsNameBornNowhere", "no-guess:no-shape");
    applies("readsSwapped", "no-guess:disagree");
    applies("readsSharedAlone", "no-guess:disagree");
    applies("readsNameNobodyIsBornWith", "no-guess:no-shape");
    applies("readsFirstSlotWithoutName", "no-guess:slot-too-high");
    doesNotApply("readsSwapped", "guessed-place:swappedA");
    doesNotApply("readsSwapped", "guessed-place:swappedB");
    doesNotApply("readsSharedAlone", "guessed-place:sharedName");
    doesNotApply("readsNameNobodyIsBornWith", "guessed-place:pointX");
    doesNotApply("readsFirstSlotWithoutName", "guessed-place:n24");
    doesNotApply("readsAndCallsCounter", "guessed-place:advance");
}
