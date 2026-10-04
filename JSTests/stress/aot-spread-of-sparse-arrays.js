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

function describe() {
    let text = String(arguments.length);
    for (let i = 0; i < arguments.length; ++i)
        text += "," + String(arguments[i]);
    return text;
}
function describeEnds() { return arguments.length + "," + String(arguments[0]) + "," + String(arguments[arguments.length - 2]) + "," + String(arguments[arguments.length - 1]); }
class Described {
    constructor() { this.text = describe.apply(undefined, arguments); }
}
class DerivedFromDescribed extends Described {
    constructor(list) { super("super", ...list); }
}

function spreadsIntoCall(list) { let result = describe(...list); return result; }
function spreadsIntoCallBetween(a, list, b) { let result = describe(a, ...list, b); return result; }
function spreadsTwoIntoCall(first, second) { let result = describe(...first, ...second); return result; }
function spreadsIntoMethodCall(o, list) { let result = o.method(...list); return result; }
function spreadsIntoConstruction(list) { return new Described(...list).text; }
function spreadsIntoSuper(list) { return new DerivedFromDescribed(list).text; }
function spreadsIntoTailCall(list) { "use strict"; return describe(...list); }
function spreadsIntoHostCall(list) { let result = Math.max(0, ...list); return result; }
function spreadsEndsIntoCall(list) { let result = describeEnds(...list); return result; }
function makesArray(list) { return [0, ...list]; }
function makesArrayBetween(a, list, b) { return [a, ...list, b]; }
function makesArrayOfTwo(first, second) { return [...first, ...second]; }
function makesArrayOfOne(list) { return [...list]; }
function makesArrayAroundCall(list, f) { return [...list, f(), ...list]; }
function passesByApply(list) { return describe.apply(undefined, list); }
function passesLiteralByApply(list) { return describe.apply(undefined, [0, ...list]); }
function spreadsIntoArray(list) { return passesByApply(makesArray(list)); }
function spreadsIntoArrayBetween(a, list, b) { return passesByApply(makesArrayBetween(a, list, b)); }
function spreadsTwoIntoArray(first, second) { return passesByApply(makesArrayOfTwo(first, second)); }
function spreadsEndsIntoArray(list) { return describeEnds.apply(undefined, makesArrayBetween("first", list, "last").slice(0, -1)); }
function spreadsAlone(list) { return passesByApply(makesArrayOfOne(list)); }
function spreadsAroundCall(list, f) { return passesByApply(makesArrayAroundCall(list, f)); }
const foldedIntoCall = [spreadsIntoCall, spreadsIntoCallBetween, spreadsTwoIntoCall, spreadsIntoMethodCall, spreadsIntoConstruction, spreadsIntoTailCall, spreadsIntoHostCall, spreadsEndsIntoCall];
const foldedIntoArray = [makesArray, makesArrayBetween, makesArrayOfTwo];
for (let f of [describe, describeEnds, spreadsIntoSuper, makesArrayOfOne, makesArrayAroundCall, passesByApply, passesLiteralByApply, ...foldedIntoCall, ...foldedIntoArray])
    noInline(f);
for (let f of foldedIntoCall)
    doesNotApply(f, "calls:operationAOTSpread", "calls:operationAOTNewArrayWithSpread");
for (let f of foldedIntoArray) {
    doesNotApply(f, "calls:operationAOTSpread");
    applies(f, "calls:operationAOTNewArrayWithSpread");
}
applies(makesArrayOfOne, "calls:operationAOTSpread");
applies(makesArrayAroundCall, "calls:operationAOTSpread");

function withHiddenElement() {
    let list = [1, 2, 3];
    Object.defineProperty(list, 1, { value: 7, writable: true, configurable: true, enumerable: false });
    return list;
}
function withHiddenElementAndHole() {
    let list = [1, , 3];
    Object.defineProperty(list, 0, { value: "hidden", writable: true, configurable: true, enumerable: false });
    return list;
}
function withDeletedElement() {
    let list = withHiddenElement();
    delete list[2];
    return list;
}
function withPermanentElement() {
    let list = ["a", "b"];
    Object.defineProperty(list, 0, { value: "permanent", writable: true, configurable: false, enumerable: true });
    return list;
}
function withElementBeyondVector() {
    let list = [1, 2, 3];
    ensureArrayStorage(list);
    list[100] = "beyond";
    return list;
}
function withFarElement() {
    let list = ["near"];
    list[20000] = "far";
    return list;
}
function inArrayStorage(list) {
    ensureArrayStorage(list);
    return list;
}
function withLongerLength() {
    let list = inArrayStorage([1, 2]);
    list.length = 4;
    return list;
}
function withReadOnlyElement() {
    let list = [1, 2, 3];
    Object.defineProperty(list, 1, { value: "read only", writable: false });
    return list;
}
let log = [];
function withGetter() {
    let list = [1, 2, 3];
    Object.defineProperty(list, 1, { get() { log.push("getter"); return "got"; } });
    return list;
}
function* changes(list) {
    list[0] = "changed";
    yield "generated";
}

const other = createGlobalObject();
const object = { method: describe };
for (let i = 0; i < 60; ++i) {
    check(spreadsIntoCall(withHiddenElement()), "3,1,7,3", "a call, an element that is not enumerable");
    check(spreadsIntoCallBetween("a", withHiddenElement(), i), "5,a,1,7,3," + i, "a call with values around it");
    check(spreadsTwoIntoCall([i, "x"], withHiddenElement()), "5," + i + ",x,1,7,3", "a call, after an ordinary array");
    check(spreadsTwoIntoCall(withHiddenElement(), [i, "x"]), "5,1,7,3," + i + ",x", "a call, before an ordinary array");
    check(spreadsTwoIntoCall(withHiddenElement(), withHiddenElement()), "6,1,7,3,1,7,3", "a call, twice");
    check(spreadsIntoMethodCall(object, withHiddenElement()), "3,1,7,3", "a method call");
    check(spreadsIntoConstruction(withHiddenElement()), "3,1,7,3", "a construction");
    check(spreadsIntoSuper(withHiddenElement()), "4,super,1,7,3", "a call of super");
    check(spreadsIntoTailCall(withHiddenElement()), "3,1,7,3", "a tail call");
    check(spreadsIntoHostCall(withHiddenElement()), 7, "a call of a host function");
    check(spreadsIntoArray(withHiddenElement()), "4,0,1,7,3", "an array literal, an element that is not enumerable");
    check(spreadsIntoArrayBetween("a", withHiddenElement(), i), "5,a,1,7,3," + i, "an array literal with values around it");
    check(spreadsTwoIntoArray([i, "x"], withHiddenElement()), "5," + i + ",x,1,7,3", "an array literal, after an ordinary array");
    check(spreadsTwoIntoArray(withHiddenElement(), [i, "x"]), "5,1,7,3," + i + ",x", "an array literal, before an ordinary array");
    check(spreadsAlone(withHiddenElement()), "3,1,7,3", "an array literal of nothing else");
    check(spreadsAroundCall(withHiddenElement(), () => i), "7,1,7,3," + i + ",1,7,3", "an array literal with a call in the middle");
    check(passesByApply(withHiddenElement()), "3,1,7,3", "apply");
    check(passesLiteralByApply(withHiddenElement()), "4,0,1,7,3", "apply of an array literal");

    check(spreadsIntoCall(withHiddenElementAndHole()), "3,hidden,undefined,3", "a call, a hole next to it");
    check(spreadsIntoArray(withHiddenElementAndHole()), "4,0,hidden,undefined,3", "an array literal, a hole next to it");
    check(spreadsIntoCall(withDeletedElement()), "3,1,7,undefined", "a call, an element that was deleted");
    check(spreadsIntoArray(withDeletedElement()), "4,0,1,7,undefined", "an array literal, an element that was deleted");
    check(spreadsIntoCall(withPermanentElement()), "2,permanent,b", "a call, an element that is not configurable");
    check(spreadsIntoArray(withPermanentElement()), "3,0,permanent,b", "an array literal, an element that is not configurable");

    check(spreadsIntoCall(Object.preventExtensions([i, "two"])), "2," + i + ",two", "a call, an array that is not extensible");
    check(spreadsIntoArray(Object.preventExtensions([i, "two"])), "3,0," + i + ",two", "an array literal, an array that is not extensible");
    check(spreadsIntoCall(Object.preventExtensions([i, , 1.5])), "3," + i + ",undefined,1.5", "a call, an array with a hole that is not extensible");
    check(spreadsIntoCall(Object.seal([i, "two"])), "2," + i + ",two", "a call, a sealed array");
    check(spreadsIntoArray(Object.seal([i, "two"])), "3,0," + i + ",two", "an array literal, a sealed array");
    check(spreadsIntoCall(Object.freeze([i, "two"])), "2," + i + ",two", "a call, a frozen array");
    check(spreadsIntoArray(Object.freeze([i, "two"])), "3,0," + i + ",two", "an array literal, a frozen array");

    check(spreadsEndsIntoCall(withElementBeyondVector()), "101,1,undefined,beyond", "a call, elements in the vector and beyond it");
    check(spreadsEndsIntoArray(withElementBeyondVector()), "102,first,undefined,beyond", "an array literal, elements in the vector and beyond it");
    check(spreadsIntoCallBetween("a", withElementBeyondVector(), "b").startsWith("103,a,1,2,3,undefined,"), true, "a call, the elements in the vector");

    check(spreadsIntoCall(inArrayStorage([i, 2, 3])), "3," + i + ",2,3", "a call, array storage");
    check(spreadsIntoArray(inArrayStorage([i, 2, 3])), "4,0," + i + ",2,3", "an array literal, array storage");
    check(spreadsIntoCall(inArrayStorage([i, , 3])), "3," + i + ",undefined,3", "a call, array storage with a hole");
    check(spreadsIntoArray(inArrayStorage([i, , 3])), "4,0," + i + ",undefined,3", "an array literal, array storage with a hole");
    check(spreadsIntoCall(withLongerLength()), "4,1,2,undefined,undefined", "a call, array storage that is shorter than the length");
    check(spreadsIntoArray(withLongerLength()), "5,0,1,2,undefined,undefined", "an array literal, array storage that is shorter than the length");

    check(spreadsIntoCall(withReadOnlyElement()), "3,1,read only,3", "a call, an element that is read only");
    check(spreadsIntoArray(withReadOnlyElement()), "4,0,1,read only,3", "an array literal, an element that is read only");
    check(spreadsIntoCall(withGetter()), "3,1,got,3", "a call, a getter");
    check(spreadsIntoArray(withGetter()), "4,0,1,got,3", "an array literal, a getter");
    check(log.join(), "getter,getter", "how often the getters ran");
    log = [];

    let changed = withHiddenElement();
    check(spreadsTwoIntoCall(changed, changes(changed)), "4,1,7,3,generated", "a call, an iterator that changes the array before it");
    changed = withHiddenElement();
    check(spreadsTwoIntoArray(changed, changes(changed)), "4,1,7,3,generated", "an array literal, an iterator that changes the array before it");
    changed = withHiddenElement();
    check(spreadsTwoIntoCall(changes(changed), changed), "4,generated,changed,7,3", "a call, an iterator that changes the array after it");
    changed = withHiddenElement();
    check(spreadsTwoIntoArray(changes(changed), changed), "4,generated,changed,7,3", "an array literal, an iterator that changes the array after it");

    let foreign = other.Array.of(i, 2, 3);
    Object.defineProperty(foreign, 1, { value: "foreign", writable: true, configurable: true, enumerable: false });
    check(spreadsIntoCall(foreign), "3," + i + ",foreign,3", "a call, an array of another realm");
    check(spreadsIntoArray(foreign), "4,0," + i + ",foreign,3", "an array literal, an array of another realm");

    check(spreadsIntoCall([i, 2, 3]), "3," + i + ",2,3", "a call, an ordinary array");
    check(spreadsIntoCall([i + 0.5, , 3.5]), "3," + (i + 0.5) + ",undefined,3.5", "a call, doubles and a hole");
    check(spreadsIntoCall(new Array(2)), "2,undefined,undefined", "a call, nothing but holes");
    check(spreadsIntoArray([i, , "x"]), "4,0," + i + ",undefined,x", "an array literal, an ordinary array with a hole");
    check(spreadsIntoArray([i + 0.5, , 3.5]), "4,0," + (i + 0.5) + ",undefined,3.5", "an array literal, doubles and a hole");
    check(spreadsIntoArray(new Array(2)), "3,0,undefined,undefined", "an array literal, nothing but holes");
}

for (let i = 0; i < 3; ++i) {
    check(spreadsEndsIntoCall(withFarElement()), "20001,near,undefined,far", "a call, an element far from the others");
    check(spreadsEndsIntoArray(withFarElement()), "20002,first,undefined,far", "an array literal, an element far from the others");
}

if (typeof $vm === "object") {
    for (let i = 0; i < 3; ++i) {
        check(spreadsIntoCall($vm.createRuntimeArray(i, 2, 3)), "3," + i + ",2,3", "a call, an array that keeps its elements elsewhere");
        check(spreadsIntoArray($vm.createRuntimeArray(i, 2, 3)), "4,0," + i + ",2,3", "an array literal, an array that keeps its elements elsewhere");
        check(spreadsTwoIntoCall([i], $vm.createRuntimeArray(1, 2)), "3," + i + ",1,2", "a call, after an ordinary array");
    }
}

Array.prototype[1] = "inherited";
for (let i = 0; i < 3; ++i) {
    check(spreadsIntoCall(withHiddenElementAndHole()), "3,hidden,inherited,3", "a call, a hole when Array.prototype has an element");
    check(spreadsIntoArray(withHiddenElementAndHole()), "4,0,hidden,inherited,3", "an array literal, a hole when Array.prototype has an element");
    check(spreadsIntoCall(inArrayStorage([i, , 3])), "3," + i + ",inherited,3", "a call, array storage with a hole when Array.prototype has an element");
    check(spreadsIntoArray(inArrayStorage([i, , 3])), "4,0," + i + ",inherited,3", "an array literal, array storage with a hole when Array.prototype has an element");
    check(spreadsIntoCall(withHiddenElement()), "3,1,7,3", "a call, no hole when Array.prototype has an element");
    check(spreadsIntoArray(withHiddenElement()), "4,0,1,7,3", "an array literal, no hole when Array.prototype has an element");
}
delete Array.prototype[1];
check(spreadsIntoCall(withHiddenElementAndHole()), "3,hidden,undefined,3", "a call, after the inherited element is gone");
check(spreadsIntoArray(withHiddenElementAndHole()), "4,0,hidden,undefined,3", "an array literal, after the inherited element is gone");
