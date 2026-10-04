//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(f.name);
    if (!remarks && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
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
function thrownBy(f, ...parameters) {
    try {
        f(...parameters);
    } catch (error) {
        return error.constructor.name;
    }
    return "nothing";
}
const typedMethod = "typed-call-of-builtin-method", typedSize = "typed-size-of-map-or-set";
const comparesWithInt32 = "inline-relational-comparison-with-int32";

class TextualSet extends Set {
    has(value) { return super.has(value) ? "present" : ""; }
    get size() { return super.size ? "some" : ""; }
}
class RefusingMap extends Map {
    delete(key) { return 0; }
}
let kept;
function keep(value) { kept = value; return value; }
noInline(keep);

function text(value) { return value === true ? "yes" : value === false ? "no" : "other:" + String(value); }
noInline(text);

function mapHas(key) { let map = new Map(); map.set(1, "one"); map.set(NaN, "not a number"); map.set("", "empty"); return text(map.has(key)); }
function chainedSets(key) { return text(new Map().set(1, "one").set(2, "two").has(key)); }
function chainedAdds(value) { return text(new Set().add(1).add(2).delete(value)); }
function setHas(value) { let set = new Set(); set.add(1); return text(set.has(value)); }
function mapDeletes(key) { let map = new Map(); map.set(1, "one"); return text(map.delete(key)); }
function mapGetsAndHas(key) { let map = new Map(); map.set(1, 0); let found = map.get(key); return text(map.has(key)) + String(found); }
function weakMapHas(key, other) { let map = new WeakMap(); map.set(key, 1); return text(map.has(key)) + text(map.has(other)) + text(map.delete(key)) + text(map.delete(key)); }
function weakSetHas(key, other) { let set = new WeakSet(); return text(set.add(key).has(key)) + text(set.has(other)) + text(set.delete(other)); }
function hasDigits(string) { let pattern = /\d+/; return text(pattern.test(string)); }
function hasDigitsTwice(first, second) { let pattern = /\d+/; return text(pattern.test(first)) + text(pattern.test(second)); }
function firstDigits(string) { let pattern = /(\d+)/; let match = pattern.exec(string); return match === null ? "none" : match[1]; }
function isAfterEpoch(ms) { let date = new Date(ms); return date.getTime() > 0 ? "after" : "not after"; }
function sizeOfMap(count) { let map = new Map(); for (let i = 0; i < count; ++i) map.set(i, i); return map.size > 0 ? "some" : "none"; }
function sizeOfSet(count) { let set = new Set(); for (let i = 0; i < count; ++i) set.add(i); return set.size > 0 ? "some" : "none"; }
function seenTwice(list) { const seen = new Set(); let twice = 0; for (let i = 0; i < list.length; ++i) { if (seen.has(list[i]) === true) ++twice; seen.add(list[i]); } return twice; }
function clearedMap() { let map = new Map().set(1, 1); let result = map.clear(); return result === undefined ? "nothing" : "something"; }
function iteratesKeys() { let map = new Map(); map.set(1, 1); let keys = map.keys(); return text(map.has(1)) + keys.next().value; }
function literalIncludes(value) { return text([1, 2, NaN].includes(value)); }
function literalJoins(a, b) { let joined = [a, b].join("-"); return joined === "" ? "empty" : joined; }
const typed = [mapHas, chainedSets, chainedAdds, setHas, mapDeletes, mapGetsAndHas, weakMapHas, weakSetHas, hasDigits, hasDigitsTwice, firstDigits, isAfterEpoch, seenTwice, clearedMap, iteratesKeys, literalIncludes, literalJoins];
for (let f of [...typed, sizeOfMap, sizeOfSet])
    noInline(f);

check([1, 1.0, 2, NaN, "", "1", undefined, -0].map(mapHas).join(), "yes,yes,no,yes,yes,no,no,no", "Map.prototype.has");
check([1, 2, 3].map(chainedSets).join(), "yes,yes,no", "has after set");
check([1, 2, 3].map(chainedAdds).join(), "yes,yes,no", "delete after add");
check([1, 2].map(setHas).join(), "yes,no", "Set.prototype.has");
check([1, 2].map(mapDeletes).join(), "yes,no", "Map.prototype.delete");
check([1, 2].map(mapGetsAndHas).join(), "yes0,noundefined", "Map.prototype.get and has");
check(weakMapHas({}, {}), "yesnoyesno", "WeakMap");
check(weakMapHas({}, 5), "yesnoyesno", "WeakMap and a key that is no object");
check(weakSetHas({}, {}), "yesnono", "WeakSet");
check(thrownBy(weakSetHas, 5, {}), "TypeError", "WeakSet.prototype.add of a number");
check(["a1", "abc", "", 7, undefined].map(hasDigits).join(), "yes,no,no,yes,no", "RegExp.prototype.test");
check(hasDigitsTwice("a1", "b"), "yesno", "RegExp.prototype.test twice");
check(["a12b3", "abc", 45].map(firstDigits).join(), "12,none,45", "RegExp.prototype.exec");
check([1, 0, -1, NaN, 8.64e15, 8.64e15 + 1].map(isAfterEpoch).join(), "after,not after,not after,not after,after,not after", "Date.prototype.getTime");
check([0, 1, 3].map(sizeOfMap).join(), "none,some,some", "the size of a map");
check([0, 2].map(sizeOfSet).join(), "none,some", "the size of a set");
check(seenTwice([1, 2, 1, 1, "1"]), 2, "a set of what was seen");
check(clearedMap(), "nothing", "Map.prototype.clear");
check(iteratesKeys(), "yes1", "Map.prototype.keys");
check([1, 3, NaN, "1"].map(literalIncludes).join(), "yes,no,yes,no", "Array.prototype.includes of a literal");
check(literalJoins(1, 2) + literalJoins("", ""), "1-2-", "Array.prototype.join of a literal");
for (let f of typed)
    applies(f, typedMethod);
doesNotApply(isAfterEpoch, comparesWithInt32, "calls:Greater");
for (let f of [sizeOfMap, sizeOfSet]) {
    applies(f, typedSize);
    doesNotApply(f, comparesWithInt32, "calls:Greater");
}

Object.defineProperty(Map.prototype, "shadowsItsHas", { get() { this.has = () => "own, by a getter"; return 1; } });
function ownMethod() { const map = new Map(); map.has = () => 42; return text(map.has(1)); }
function ownMethodByValue(name) { const map = new Map(); map[name] = () => 42; return text(map.has(1)); }
function ownMethodDefined() { const map = new Map(); Object.defineProperty(map, "has", { value: () => 42 }); return text(map.has(1)); }
function ownSize() { const map = new Map(); Object.defineProperty(map, "size", { value: "big" }); return map.size > 0 ? "some" : "none:" + map.size; }
function otherPrototype() { const map = new Map(); Object.setPrototypeOf(map, { has() { return "inherited"; } }); return text(map.has(1)); }
function otherPrototypeByStore() { const map = new Map(); map.__proto__ = { has() { return "inherited"; } }; return text(map.has(1)); }
function changedByGetter() { const map = new Map(); map.shadowsItsHas; return text(map.has(1)); }
function changedByCallback() { const map = new Map(); map.set(1, 1); map.forEach((value, key, itself) => { itself.has = () => "own, by a callback"; }); return text(map.has(1)); }
function changedByCallee() { const map = new Map(); keep(map).has = () => "own, by a callee"; return text(map.has(1)); }
function changedByClosure() { const map = new Map(); const change = () => { map.has = () => "own, by a closure"; }; change(); return text(map.has(1)); }
function changedThroughItself() { const map = new Map(); map.set(map, 1); for (let key of map.keys()) key.has = () => "own, through a key"; return text(map.has(1)); }
function changedLater(count) { const map = new Map(); let results = ""; for (let i = 0; i < count; ++i) { results += text(map.has(1)); map.has = () => 42; } return results; }
function returnedBySet() { const map = new Map(); keep(map.set(1, 1)).has = () => "own, through set"; return text(map.has(1)); }
function subclassHas(value) { const set = new TextualSet(); set.add(1); return text(set.has(value)); }
function subclassDeletes() { const map = new RefusingMap(); map.set(1, 1); return text(map.delete(1)); }
function constructedForSubclass(value) { const set = Reflect.construct(Set, [[1]], TextualSet); return text(set.has(value)); }
function optionalMapHas(given, key) { let map = given === true ? new Map().set(1, "one") : undefined; return text(map.has(key)); }
function mapOrWeakMapHas(which, value) { let map = new Map(); map.set(1, 1); let collection = which === true ? map : new WeakMap(); return text(collection.has(value)); }
function anythingHas(collection, value) { return text(collection.has(value)); }
function sizeOfAnything(collection) { return collection.size ? "some" : "none"; }
function anythingDeletes(collection, key) { return text(collection.delete(key)); }
function borrowsHas(value) { let map = new Map(); let set = new Set(); set.add(value); return text(map.has.call(set, value)); }
function mapGets(key) { let map = new Map(); map.set(1, 0); map.set(2, "two"); return map.get(key) ? "yes" : "no"; }
function arrayUsedTwice(value) { let list = [1, 2, NaN]; list.includes = () => "own"; return text(list.includes(value)); }
function arrayInVariable(value) { let list = [1, 2, NaN]; let first = list[0]; return text(list.includes(value)) + first; }
function changedRegExp(string) { let pattern = /\d+/; pattern.test = () => "own"; return text(pattern.test(string)); }
function ownsProperty(o, name) { return Object.prototype.hasOwnProperty.call(o, name) ? "yes" : "no"; }
const untyped = [ownMethod, ownMethodByValue, ownMethodDefined, otherPrototype, otherPrototypeByStore, changedByGetter, changedByCallback, changedByCallee, changedByClosure, changedThroughItself, changedLater, returnedBySet,
    subclassHas, subclassDeletes, constructedForSubclass, mapOrWeakMapHas, anythingHas, anythingDeletes, borrowsHas, mapGets, arrayUsedTwice, arrayInVariable, changedRegExp];
for (let f of [...untyped, optionalMapHas, ownSize, sizeOfAnything, ownsProperty])
    noInline(f);

check(ownMethod(), "other:42", "an own method");
check(ownMethodByValue("has"), "other:42", "an own method stored by value");
check(ownMethodDefined(), "other:42", "an own method that is defined");
check(ownSize(), "none:big", "an own size");
check(otherPrototype(), "other:inherited", "another prototype");
check(otherPrototypeByStore(), "other:inherited", "another prototype, stored");
check(changedByGetter(), "other:own, by a getter", "a getter added to Map.prototype");
check(changedByCallback(), "other:own, by a callback", "the third parameter of a callback");
check(changedByCallee(), "other:own, by a callee", "a map that was passed on");
check(changedByClosure(), "other:own, by a closure", "a map that a closure sees");
check(changedThroughItself(), "other:own, through a key", "a map that is its own key");
check(changedLater(3), "noother:42other:42", "a map that is changed after the first call");
check(returnedBySet(), "other:own, through set", "what set returns");
check(subclassHas(1) + subclassHas(2), "other:presentother:", "has of a subclass");
check(subclassDeletes(), "other:0", "delete of a subclass");
check(constructedForSubclass(1), "other:present", "a set constructed for a subclass");
check(optionalMapHas(true, 1) + optionalMapHas(true, 2), "yesno", "has of a map that may be missing");
check(thrownBy(optionalMapHas, false, 1), "TypeError", "has of undefined");
check(mapOrWeakMapHas(true, 1) + mapOrWeakMapHas(true, 2) + mapOrWeakMapHas(false, {}), "yesnono", "has of one of two kinds");
check(anythingHas(new Map([[1, 1]]), 1) + anythingHas(new Set([1]), 2), "yesno", "has of a map and of a set");
check(anythingHas(new TextualSet([1]), 1) + anythingHas(new TextualSet([1]), 2), "other:presentother:", "has that returns strings");
check(anythingHas({ has() { return 0; } }, 1) + anythingHas({ has() { return NaN; } }, 1), "other:0other:NaN", "has that returns what it likes");
check(sizeOfAnything(new Map()) + sizeOfAnything(new Set([1])) + sizeOfAnything(new TextualSet()) + sizeOfAnything(new TextualSet([1])) + sizeOfAnything({ size: -0 }), "nonesomenonesomenone", "the size of anything");
check(anythingDeletes(new Map([[1, 1]]), 1) + anythingDeletes(new RefusingMap([[1, 1]]), 1), "yesother:0", "delete that returns a number");
check(thrownBy(borrowsHas, 1), "TypeError", "Map.prototype.has of a set");
check([1, 2, 3].map(mapGets).join(), "no,yes,no", "Map.prototype.get");
check(arrayUsedTwice(1), "other:own", "an own method of an array");
check(arrayInVariable(1) + arrayInVariable(3), "yes1no1", "an array that is also read");
check(changedRegExp("1"), "other:own", "an own method of a RegExp");
check(ownsProperty({ a: 1 }, "a") + ownsProperty({ a: 1 }, "b") + ownsProperty([], "length") + ownsProperty("text", 0), "yesnoyesyes", "Object.prototype.hasOwnProperty");
check(thrownBy(ownsProperty, null, "a"), "TypeError", "hasOwnProperty of null");
for (let f of untyped)
    doesNotApply(f, typedMethod);
for (let f of [ownSize, sizeOfAnything])
    doesNotApply(f, typedSize);
