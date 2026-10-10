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
const typedMethod = "typed-call-of-builtin-method", plainArray = "plain-array-for-original-receiver", callsGetLength = "calls:GetLength";
let kept;
function keep(value) { kept = value; return value; }
noInline(keep);
function describe(value) { return Array.isArray(value) ? "[" + value.join() + "]" + (Object.getPrototypeOf(value) === Array.prototype ? "" : " of a subclass") : typeof value + ":" + String(value.kind); }
noInline(describe);

function mapsLiteral(a, b, f) { let result = [a, b].map(f); return result.length + describe(result); }
function mapsLiteralWithClosure(a, b) { let result = [a, b].map(x => x + 1); return result.length + describe(result); }
function filtersLiteral(a, b, f) { let result = [a, b, a].filter(f); return result.length + describe(result); }
function filtersLiteralWithClosure(a, b) { let result = [a, b, a].filter(x => x > 1); return result.length + describe(result); }
function slicesLiteral(a, b) { let result = [a, b, a].slice(1); return result.length + describe(result); }
function concatenatesLiteral(a, b) { let result = [a].concat(b, [a, b]); return result.length + describe(result); }
function mapsSpread(list, f) { let result = [...list].map(f); return result.length + describe(result); }
function mapsRest(f, ...rest) { let result = rest.map(f); return result.length + describe(result); }
function mapsConstructed(n, f) { let result = new Array(n | 0).map(f); return result.length + describe(result); }
const typed = [mapsLiteral, mapsLiteralWithClosure, filtersLiteral, filtersLiteralWithClosure, slicesLiteral, concatenatesLiteral, mapsSpread, mapsRest];
for (let f of [...typed, mapsConstructed])
    noInline(f);

check(mapsLiteral(1, 2, x => x * 2), "2[2,4]", "map of a literal");
check(mapsLiteral(1, 2, (x, i, all) => { all.constructor = { [Symbol.species]: Object }; all.map = () => 42; return i; }), "2[0,1]", "map of a literal that the callback changes");
check(mapsLiteralWithClosure(1, 2), "2[2,3]", "map of a literal with a closure");
check(filtersLiteral(1, 2, x => x === 1), "2[1,1]", "filter of a literal");
check(filtersLiteralWithClosure(1, 2), "1[2]", "filter of a literal with a closure");
check(slicesLiteral(1, 2), "2[2,1]", "slice of a literal");
check(concatenatesLiteral(1, 2), "4[1,2,1,2]", "concat of a literal");
check(concatenatesLiteral(1, [2, 3]), "5[1,2,3,1,2,3]", "concat of a literal and an array");
check(mapsSpread(new Set([1, 2, 3]), x => x + 1), "3[2,3,4]", "map of a spread");
check(mapsRest(x => x + 1, 1, 2), "2[2,3]", "map of the rest");
check(mapsRest(x => x + 1), "0[]", "map of an empty rest");
check(mapsConstructed(3, x => 1), "3[,,]", "map of holes");
for (let f of typed) {
    applies(f, typedMethod);
    doesNotApply(f, callsGetLength);
}
applies(mapsLiteralWithClosure, plainArray);
applies(filtersLiteralWithClosure, plainArray);

class Listed extends Array { }
class Counted extends Array { static get [Symbol.species]() { return Array; } }
function Odd(length) { this.kind = "odd"; this.length = 0; }
function mapsAnything(list, f) { let result = list.map(f); return result.length + describe(result); }
function filtersAnything(list, f) { let result = list.filter(f); return result.length + describe(result); }
function slicesAnything(list) { let result = list.slice(1); return result.length + describe(result); }
function concatenatesAnything(list, other) { let result = list.concat(other); return result.length + describe(result); }
function mapsAnythingWithClosure(list) { let result = list.map(x => x + 1); return result.length + describe(result); }
function ownConstructor(a, b) { let list = [a, b]; list.constructor = { [Symbol.species]: Odd }; let result = list.map(x => x + 1); return result.length + describe(result); }
function ownMap(a, b) { let list = [a, b]; list.map = () => ({ length: "none", kind: "own" }); let result = list.map(x => x + 1); return result.length + describe(result); }
function otherPrototype(a, b) { let list = [a, b]; Object.setPrototypeOf(list, Listed.prototype); let result = list.slice(0); return result.length + describe(result); }
function passedOn(a, b) { let list = keep([a, b]); let result = list.map(x => x + 1); return result.length + describe(result); }
function usedTwice(a, b) { let list = [a, b]; let first = list.map(x => x + 1); let second = list.map(x => x + 2); return first.length + second.length + describe(second); }
function usedInLoop(a, count) { let list = [a]; let total = 0; for (let i = 0; i < count; ++i) total += list.map((x, j, all) => { all.map = () => ({ length: 100 }); return x; }).length; return total; }
function secondOfChain(list, f) { let result = list.filter(f).map(f); return result.length + describe(result); }
function mapCalledOn(list, f) { let result = Array.prototype.map.call(list, f); return result.length + describe(result); }
function constructsSubclass(a, b) { let result = new Listed(a, b).map(x => x + 1); return result.length + describe(result); }
const untyped = [mapsAnything, filtersAnything, slicesAnything, concatenatesAnything, mapsAnythingWithClosure, ownConstructor, ownMap, otherPrototype, passedOn, usedTwice, usedInLoop, secondOfChain, mapCalledOn, constructsSubclass];
for (let f of untyped)
    noInline(f);

let withSpecies = [1, 2];
withSpecies.constructor = { [Symbol.species]: Odd };
let withOwnMethods = [1, 2];
for (let name of ["map", "filter", "slice", "concat"])
    withOwnMethods[name] = () => ({ length: "none", kind: "own " + name });
for (let [f, ...rest] of [[mapsAnything, x => x], [filtersAnything, x => true], [slicesAnything], [concatenatesAnything, [3]]]) {
    check(f([1, 2], ...rest).slice(1, 2), "[", f.name + " of an array");
    check(f(new Listed(1, 2), ...rest).endsWith(" of a subclass"), true, f.name + " of an instance of a subclass");
    check(f(new Counted(1, 2), ...rest).endsWith("]"), true, f.name + " of an instance of a subclass whose species is Array");
    check(f(withSpecies, ...rest).endsWith("object:odd"), true, f.name + " of an array with its own constructor");
    check(f(withOwnMethods, ...rest).startsWith("noneobject:own "), true, f.name + " of an array with its own methods");
}
check(mapsAnythingWithClosure([1, 2]), "2[2,3]", "map of an array with a closure");
check(mapsAnythingWithClosure(new Listed(1, 2)), "2[2,3] of a subclass", "map of an instance of a subclass with a closure");
check(mapsAnythingWithClosure(withSpecies), "0object:odd", "map of an array with its own constructor and a closure");
check(mapsAnythingWithClosure(withOwnMethods), "noneobject:own map", "an own map and a closure");
check(ownConstructor(1, 2), "0object:odd", "an own constructor");
check(ownMap(1, 2), "noneobject:own", "an own map");
check(otherPrototype(1, 2), "2[1,2] of a subclass", "another prototype");
check(passedOn(1, 2), "2[2,3]", "an array that was passed on");
check(usedTwice(1, 2), "4[3,4]", "an array that is used twice");
check(usedInLoop(1, 3), 201, "an array that the callback changes before the next call");
check(secondOfChain([1, 2], x => x), "2[1,2]", "the second call of a chain");
check(secondOfChain(new Listed(1, 2), x => x), "2[1,2] of a subclass", "the second call of a chain on an instance of a subclass");
check(mapCalledOn({ length: 1, 0: 5 }, x => x) + mapCalledOn(withSpecies, x => x), "1[5]0object:odd", "Array.prototype.map called on an object and on an array with its own constructor");
check(constructsSubclass(1, 2), "2[2,3] of a subclass", "map of a new instance of a subclass");
for (let f of untyped)
    doesNotApply(f, typedMethod, plainArray);

function extendsArray() { return class extends Array { }; }
function extendsMapByName() { class Named extends Map { } return Named; }
function extendsProgramClass() { class First { } class Second extends First { } return Second; }
function extendsFunction() { function First() { } return class extends First { }; }
function extendsNull() { return class extends null { }; }
function extendsError() { return class extends Error { }; }
function extendsParameter(Base) { return class extends Base { }; }
function extendsResult() { return class extends keep(Array) { }; }
function constructsForOther(Other) { return Reflect.construct(Promise, [() => { }], Other); }
function constructsPlainly() { return Reflect.construct(Set, [[1]]); }
function passesReflectConstruct() { return keep(Reflect.construct); }
function definesNoClass(prototype) { return { __proto__: prototype }; }
for (let f of [extendsArray, extendsMapByName, extendsProgramClass, extendsFunction, extendsNull, extendsError, extendsParameter, extendsResult, constructsForOther, constructsPlainly, passesReflectConstruct, definesNoClass])
    noInline(f);
check(Array.isArray(new (extendsArray())), true, "a class that extends Array");
check(new (extendsMapByName())() instanceof Map, true, "a class that extends Map");
check(typeof new (extendsProgramClass()), "object", "a class that extends a class of the program");
check(typeof new (extendsFunction()), "object", "a class that extends a function of the program");
check(typeof extendsNull(), "function", "a class that extends null");
check(new (extendsError())() instanceof Error, true, "a class that extends Error");
check(new (extendsParameter(RegExp))("a").test("a"), true, "a class that extends a parameter");
check(Array.isArray(new (extendsResult())), true, "a class that extends the result of a call");
check(constructsForOther(class extends Promise { }) instanceof Promise, true, "Reflect.construct for another constructor");
check(constructsPlainly().has(1), true, "Reflect.construct without another constructor");
check(passesReflectConstruct(), Reflect.construct, "Reflect.construct as a value");
check(Object.getPrototypeOf(definesNoClass(Array.prototype)), Array.prototype, "an object literal with a prototype");
applies(extendsArray, "extends-builtin:Array");
applies(extendsMapByName, "extends-builtin:Map");
applies(extendsParameter, "extends-builtin:unknown");
applies(extendsResult, "extends-builtin:unknown");
applies(constructsForOther, "extends-builtin:Promise");
applies(passesReflectConstruct, "extends-builtin:unknown");
for (let f of [extendsProgramClass, extendsFunction, extendsNull, extendsError, constructsPlainly, definesNoClass, mapsLiteral])
    doesNotApply(f, "extends-builtin");
doesNotApply(extendsArray, "extends-builtin:unknown", "extends-builtin:Map");
