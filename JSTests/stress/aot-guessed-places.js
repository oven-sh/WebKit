//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=0")
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
