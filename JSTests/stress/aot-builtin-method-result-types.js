//@ runDefault("--compileMainScriptAheadOfTime=1", "--aotOverriddenMethodsPath=aot-builtin-method-result-types.js")
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
const typedMethod = "typed-call-of-builtin-method", typedSize = "typed-size-of-map-or-set", unlessNullish = "typed-builtin-call-unless-receiver-is-nullish";
const callsToBoolean = "calls:ToBoolean", comparesWithInt32 = "inline-relational-comparison-with-int32";

const methodsListedAsOverridden = [
Set.has
,
Set.size
,
Map.delete
];
class TextualSet extends Set {
    has(value) { return super.has(value) ? "present" : ""; }
    get size() { return super.size ? "some" : ""; }
}
class RefusingMap extends Map {
    delete(key) { return 0; }
}

function mapHas(key) { let map = new Map(); map.set(1, "one"); map.set(NaN, "not a number"); map.set("", "empty"); return map.has(key) ? "yes" : "no"; }
function chainedSets(key) { return new Map().set(1, "one").set(2, "two").has(key) ? "yes" : "no"; }
function chainedAdds(value) { return new Set().add(1).add(2).delete(value) ? "yes" : "no"; }
function optionalMapHas(given, key) { let map = given === true ? new Map().set(1, "one") : undefined; return map.has(key) ? "yes" : "no"; }
function weakMapHas(key, other) { let map = new WeakMap(); map.set(key, 1); return (map.has(key) ? "yes" : "no") + (map.has(other) ? "yes" : "no") + (map.delete(key) ? "yes" : "no") + (map.delete(key) ? "yes" : "no"); }
function weakSetHas(key, other) { let set = new WeakSet(); return (set.add(key).has(key) ? "yes" : "no") + (set.has(other) ? "yes" : "no") + (set.delete(other) ? "yes" : "no"); }
function hasDigits(text) { let pattern = /\d+/; return pattern.test(text) ? "yes" : "no"; }
function firstDigits(text) { let pattern = /(\d+)/; let match = pattern.exec(text); return match ? match[1] : "none"; }
function isAfterEpoch(ms) { let date = new Date(ms); return date.getTime() > 0 ? "after" : "not after"; }
function sizeOfMap(count) { let map = new Map(); for (let i = 0; i < count; ++i) map.set(i, i); return map.size ? "some" : "none"; }
function clearedMap() { let map = new Map().set(1, 1); let result = map.clear(); return result ? "something" : "nothing"; }
for (let f of [mapHas, chainedSets, chainedAdds, optionalMapHas, weakMapHas, weakSetHas, hasDigits, firstDigits, isAfterEpoch, sizeOfMap, clearedMap])
    noInline(f);

check([1, 1.0, 2, NaN, "", "1", undefined, -0].map(mapHas).join(), "yes,yes,no,yes,yes,no,no,no", "Map.prototype.has");
check([1, 2, 3].map(chainedSets).join(), "yes,yes,no", "has after set");
check([1, 2, 3].map(chainedAdds).join(), "yes,yes,no", "delete after add");
check(optionalMapHas(true, 1) + optionalMapHas(true, 2), "yesno", "has of a map that may be missing");
check(thrownBy(optionalMapHas, false, 1), "TypeError", "has of undefined");
check(weakMapHas({}, {}), "yesnoyesno", "WeakMap");
check(weakMapHas({}, 5), "yesnoyesno", "WeakMap and a key that is no object");
check(weakSetHas({}, {}), "yesnono", "WeakSet");
check(thrownBy(weakSetHas, 5, {}), "TypeError", "WeakSet.prototype.add of a number");
check(["a1", "abc", "", 7, undefined].map(hasDigits).join(), "yes,no,no,yes,no", "RegExp.prototype.test");
check(["a12b3", "abc", 45].map(firstDigits).join(), "12,none,45", "RegExp.prototype.exec");
check([1, 0, -1, NaN, 8.64e15, 8.64e15 + 1].map(isAfterEpoch).join(), "after,not after,not after,not after,after,not after", "Date.prototype.getTime");
check([0, 1, 3].map(sizeOfMap).join(), "none,some,some", "the size of a map");
check(clearedMap(), "nothing", "Map.prototype.clear");
for (let f of [mapHas, chainedSets, chainedAdds, optionalMapHas, weakMapHas, weakSetHas, hasDigits, firstDigits, clearedMap]) {
    applies(f, typedMethod);
    doesNotApply(f, callsToBoolean);
}
applies(optionalMapHas, unlessNullish);
applies(isAfterEpoch, typedMethod);
doesNotApply(isAfterEpoch, comparesWithInt32, "calls:Greater");
applies(sizeOfMap, typedSize);
doesNotApply(sizeOfMap, callsToBoolean);

function setHas(value) { let set = new Set(); set.add(1); return set.has(value) ? "yes" : "no"; }
function sizeOfSet(count) { let set = new Set(); for (let i = 0; i < count; ++i) set.add(i); return set.size ? "some" : "none"; }
function mapDeletes(key) { let map = new Map(); map.set(1, "one"); return map.delete(key) ? "yes" : "no"; }
function anythingHas(collection, value) { return collection.has(value) ? "yes" : "no"; }
function sizeOfAnything(collection) { return collection.size ? "some" : "none"; }
function anythingDeletes(collection, key) { return collection.delete(key) ? "yes" : "no"; }
function borrowsHas(value) { let map = new Map(); let set = new Set(); set.add(value); return map.has.call(set, value) ? "yes" : "no"; }
function mapOrSetHas(which, value) { let map = new Map(); map.set(1, 1); let collection = which ? map : new WeakMap(); return collection.has(value) ? "yes" : "no"; }
function arrayIncludes(value) { let list = [1, 2, NaN]; return list.includes(value) ? "yes" : "no"; }
function mapGets(key) { let map = new Map(); map.set(1, 0); map.set(2, "two"); return map.get(key) ? "yes" : "no"; }
function ownsProperty(o, name) { return Object.prototype.hasOwnProperty.call(o, name) ? "yes" : "no"; }
for (let f of [setHas, sizeOfSet, mapDeletes, anythingHas, sizeOfAnything, anythingDeletes, borrowsHas, mapOrSetHas, arrayIncludes, mapGets, ownsProperty])
    noInline(f);

check([1, 2].map(setHas).join(), "yes,no", "Set.prototype.has");
check([0, 2].map(sizeOfSet).join(), "none,some", "the size of a set");
check([1, 2].map(mapDeletes).join(), "yes,no", "Map.prototype.delete");
check(anythingHas(new Map([[1, 1]]), 1) + anythingHas(new Set([1]), 2), "yesno", "has of a map and of a set");
check(anythingHas(new TextualSet([1]), 1) + anythingHas(new TextualSet([1]), 2), "yesno", "has that returns strings");
check(anythingHas({ has() { return 0; } }, 1) + anythingHas({ has() { return {}; } }, 1) + anythingHas({ has() { return NaN; } }, 1), "noyesno", "has that returns what it likes");
check(sizeOfAnything(new Map()) + sizeOfAnything(new Set([1])) + sizeOfAnything(new TextualSet()) + sizeOfAnything(new TextualSet([1])) + sizeOfAnything({ size: -0 }), "nonesomenonesomenone", "the size of anything");
check(anythingDeletes(new Map([[1, 1]]), 1) + anythingDeletes(new RefusingMap([[1, 1]]), 1), "yesno", "delete that returns a number");
check(thrownBy(borrowsHas, 1), "TypeError", "Map.prototype.has of a set");
check(mapOrSetHas(true, 1) + mapOrSetHas(true, 2) + mapOrSetHas(false, {}), "yesnono", "has of one of two kinds");
check([1, 3, NaN, "1"].map(arrayIncludes).join(), "yes,no,yes,no", "Array.prototype.includes");
check([1, 2, 3].map(mapGets).join(), "no,yes,no", "Map.prototype.get");
check(ownsProperty({ a: 1 }, "a") + ownsProperty({ a: 1 }, "b") + ownsProperty([], "length") + ownsProperty("text", 0), "yesnoyesyes", "Object.prototype.hasOwnProperty");
check(thrownBy(ownsProperty, null, "a"), "TypeError", "hasOwnProperty of null");
for (let f of [setHas, mapDeletes, anythingHas, anythingDeletes, mapOrSetHas, arrayIncludes, mapGets])
    doesNotApply(f, typedMethod);
for (let f of [sizeOfSet, sizeOfAnything])
    doesNotApply(f, typedSize);
check(methodsListedAsOverridden.length, 3, "the list");
