//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1")
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
function repeat(f, ...args) {
    let result;
    for (let i = 0; i < 100; i++)
        result = f(...args);
    return result;
}
function callOnce(f, ...args) { return f(...args); }
function isUncacheableDictionary(object) { return describe(object).includes("UncacheableDictionary"); }
function compact(object, ...deleted) {
    $vm.toUncacheableDictionary(object);
    for (const name of deleted)
        delete object[name];
    $vm.flattenDictionaryObject(object);
}
function readsProperty(o) { return o.property; }
readsProperty({ property: 1 });
const usesDataStubs = (remarksOf(readsProperty) || []).includes("calls:GetById");

Object.defineProperty(Array.prototype, "addedFirst", { value: "first" });
Array.prototype.deletable = 1;
Object.defineProperty(Array.prototype, "afterDeletable", { value: "after" });
Array.prototype.last = "last";
function readAddedFirst(x) { return x.addedFirst; }
function readAfterDeletable(x) { return x.afterDeletable; }
function readSlice(x) { return x.slice; }
const array = [1, 2, 3];
check(repeat(readAddedFirst, array), "first", "a constant added to a prototype whose properties cannot be deleted");
check(repeat(readAfterDeletable, array), "after", "a constant added after a property that can be deleted");
check(repeat(readSlice, array), Array.prototype.slice, "a built-in method");
compact(Array.prototype, "deletable");
check(readAfterDeletable(array), "after", "the constant moved when the property before it was deleted");
check(readAddedFirst(array), "first", "the constant before it did not move");
check(readSlice(array), Array.prototype.slice, "a built-in method did not move");

String.prototype.deletable = 1;
Object.defineProperty(String.prototype, "shout", { get() { "use strict"; return this + "!"; } });
Object.defineProperty(String.prototype, "ask", { get() { "use strict"; return this + "?"; } });
function readShout(x) { return x.shout; }
check(repeat(readShout, "a"), "a!", "a getter added after a property that can be deleted");
compact(String.prototype, "deletable");
check(readShout("b"), "b!", "the getter moved, another one took its place");

Map.prototype.deletable = 1;
Object.defineProperty(Map.prototype, "lastOfAll", { value: "last" });
function readLastOfAll(x) { return x.lastOfAll; }
const map = new Map;
check(repeat(readLastOfAll, map), "last", "the last property");
compact(Map.prototype, "deletable");
check(readLastOfAll(map), "last", "the last property moved, nothing took its place");

const names = [];
for (let i = 0; i < 300; i++) {
    names.push("deletable" + i);
    Set.prototype[names[i]] = i;
}
Object.defineProperty(Set.prototype, "afterMany", { value: "many" });
function readAfterMany(x) { return x.afterMany; }
const set = new Set;
check(repeat(readAfterMany, set), "many", "a constant after many properties");
compact(Set.prototype, ...names);
check(readAfterMany(set), "many", "the storage shrank");

WeakMap.prototype.deletable = 1;
Object.defineProperty(WeakMap.prototype, "afterDeletable", { value: "after" });
WeakMap.prototype.last = "last";
function readFromWeakMap(x) { return x.afterDeletable; }
function readLast(x) { return x.last; }
const weakMap = new WeakMap;
check(repeat(readFromWeakMap, weakMap), "after", "a constant of a prototype that becomes a dictionary by itself");
for (let i = 0; i < 100; i++) {
    WeakMap.prototype.temporary = i;
    delete WeakMap.prototype.temporary;
}
delete WeakMap.prototype.deletable;
check(repeat(readLast, weakMap), "last", "caching another read makes the prototype compact");
check(readFromWeakMap(weakMap), "after", "the constant moved without any help");

if (usesDataStubs) {
    Object.defineProperty(RegExp.prototype, "addedFirst", { value: "first" });
    RegExp.prototype.deletable = 1;
    Object.defineProperty(RegExp.prototype, "afterDeletable", { value: "after" });
    function readExec(x) { return x.exec; }
    function readFirstOfRegExp(x) { return x.addedFirst; }
    function readAfterOfRegExp(x) { return x.afterDeletable; }
    const regExp = /a/;
    const exec = RegExp.prototype.exec;
    repeat(readExec, regExp);
    repeat(readFirstOfRegExp, regExp);
    repeat(readAfterOfRegExp, regExp);
    $vm.toUncacheableDictionary(RegExp.prototype);
    check(callOnce(readExec, regExp), exec, "a built-in method of a dictionary");
    check(isUncacheableDictionary(RegExp.prototype), true, "a built-in method stays cached");
    check(callOnce(readFirstOfRegExp, regExp), "first", "a constant that cannot move, of a dictionary");
    check(isUncacheableDictionary(RegExp.prototype), true, "a constant that cannot move stays cached");
    check(callOnce(readAfterOfRegExp, regExp), "after", "a constant that can move, of a dictionary");
    check(isUncacheableDictionary(RegExp.prototype), false, "a constant that can move is looked up again");
}
