//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function thrownBy(f, ...args) {
    try {
        f(...args);
    } catch (error) {
        return error instanceof Error ? error.constructor.name : "value " + String(error);
    }
    return "nothing";
}
const other = createGlobalObject();
const rounds = 300;

function describe() {
    let text = typeof this === "object" && this !== null && "tag" in this ? this.tag + ":" : "";
    text += arguments.length;
    for (let i = 0; i < arguments.length; ++i)
        text += "," + String(arguments[i]);
    return text;
}
function describeThree(a, b, c) { return String(a) + "|" + String(b) + "|" + String(c) + "|" + arguments.length; }
class Described {
    constructor() { this.text = describe.apply(undefined, arguments); this.target = new.target; }
}
class DerivedFromDescribed extends Described {
    constructor(...args) { super(...args); }
}
class DerivedWithPrefix extends Described {
    constructor(...args) { super("prefix", ...args, "suffix"); }
}

function spreads(list) { return describe(...list); }
function spreadsToThree(list) { return describeThree(...list); }
function spreadsWithThis(o, list) { return o.method(...list); }
function spreadsBetween(a, list, b) { return describe(a, ...list, b); }
function spreadsTwo(first, second) { return describe(...first, ...second); }
function spreadsMany(a, b, c, d) { return describe(0, ...a, 1, ...b, 2, ...c, 3, ...d, 4); }
function spreadsSameTwice(list) { return describe(...list, ...list); }
function applies(list) { return describe.apply(undefined, list); }
function appliesWithThis(o, list) { return describe.apply(o, list); }
function constructs(list) { return new Described(...list); }
function constructsBetween(a, list) { return new Described(a, ...list); }
function constructsDerived(list) { return new DerivedFromDescribed(...list); }
function constructsWithPrefix(list) { return new DerivedWithPrefix(...list); }
function forwardsRest(...rest) { return describe(...rest); }
function forwardsRestAfterFirst(first, ...rest) { return describe(...rest); }
function forwardsRestAndList(list, ...rest) { return describe(...rest, ...list, ...rest); }
function forwardsArguments() { return describe.apply(undefined, arguments); }
function spreadsInTailPosition(list) { "use strict"; return describe(...list); }
function spreadsBetweenInTailPosition(a, list) { "use strict"; return describe(a, ...list, a); }
function appliesInTailPosition(list) { "use strict"; return describe.apply(undefined, list); }
function spreadsToHostFunction(list) { return Math.max(...list); }
function spreadsToPush(target, list) { return target.push(...list); }
function spreadsToUnknown(f, list) { return f(...list); }
for (let f of [describe, describeThree, spreads, spreadsToThree, spreadsWithThis, spreadsBetween, spreadsTwo, spreadsMany, spreadsSameTwice, applies, appliesWithThis, constructs, constructsBetween, constructsDerived, constructsWithPrefix,
    forwardsRest, forwardsRestAfterFirst, forwardsRestAndList, forwardsArguments, spreadsInTailPosition, spreadsBetweenInTailPosition, appliesInTailPosition, spreadsToHostFunction, spreadsToPush, spreadsToUnknown])
    noInline(f);

let object = { tag: "object", method: describe };
for (let i = 0; i < rounds; ++i) {
    check(spreads([]), "0", "an empty array");
    check(spreads([i]), "1," + i, "one integer");
    check(spreads([1, 2, 3]), "3,1,2,3", "an array literal of integers");
    check(spreads([i, "two", null, undefined, object.tag]), "5," + i + ",two,null,undefined,object", "values of several types");
    check(spreads([1.5, i + 0.25]), "2,1.5," + (i + 0.25), "an array of doubles");
    check(spreads([1, , 3]), "3,1,undefined,3", "an array with a hole");
    check(spreads(["a", , , "d"]), "4,a,undefined,undefined,d", "an array of strings with holes");
    check(spreads(new Array(3)), "3,undefined,undefined,undefined", "an array of holes");
    check(spreads(Array.from({ length: 20 }, (unused, index) => index + i)).split(",").length, 21, "twenty elements");
    check(spreadsToThree([]), "undefined|undefined|undefined|0", "fewer arguments than parameters");
    check(spreadsToThree([1]), "1|undefined|undefined|1", "fewer arguments than parameters");
    check(spreadsToThree([1, 2, 3]), "1|2|3|3", "as many arguments as parameters");
    check(spreadsToThree([1, 2, 3, 4, 5]), "1|2|3|5", "more arguments than parameters");
    check(spreadsWithThis(object, [i, 2]), "object:2," + i + ",2", "a method");
    check(spreadsBetween("a", [i, 2], "b"), "4,a," + i + ",2,b", "values around an array");
    check(spreadsBetween("a", [], "b"), "2,a,b", "values around an empty array");
    check(spreadsTwo([1, 2], ["x", i]), "4,1,2,x," + i, "two arrays");
    check(spreadsTwo([], []), "0", "two empty arrays");
    check(spreadsMany([i], [], ["b", "c"], [1.5]), "9,0," + i + ",1,2,b,c,3,1.5,4", "four arrays between values");
    check(spreadsSameTwice([i, "x"]), "4," + i + ",x," + i + ",x", "the same array twice");
    check(applies([i, 2, 3]), "3," + i + ",2,3", "apply");
    check(applies([]), "0", "apply of an empty array");
    check(applies([1, , 3]), "3,1,undefined,3", "apply of an array with a hole");
    check(applies(undefined), "0", "apply of undefined");
    check(applies(null), "0", "apply of null");
    check(applies({ length: 2, 0: "a", 1: i }), "2,a," + i, "apply of an object that is like an array");
    check(appliesWithThis(object, [i]), "object:1," + i, "apply with a receiver");
    check(constructs([i, 2]).text, "2," + i + ",2", "construction");
    check(constructs([]).target, Described, "new.target of a construction");
    check(constructsBetween("a", [i]).text, "2,a," + i, "construction with a value and an array");
    check(constructsDerived([i, "x"]).text, "2," + i + ",x", "super with the rest parameter");
    check(constructsDerived([]).target, DerivedFromDescribed, "new.target below super");
    check(constructsWithPrefix([i]).text, "3,prefix," + i + ",suffix", "super with values around the rest parameter");
    check(forwardsRest(), "0", "no rest arguments");
    check(forwardsRest(i, 2, 3), "3," + i + ",2,3", "the rest parameter");
    check(forwardsRest(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12), "12,1,2,3,4,5,6,7,8,9,10,11,12", "a long rest parameter");
    check(forwardsRestAfterFirst(), "0", "the rest parameter when nothing is passed");
    check(forwardsRestAfterFirst(1), "0", "the rest parameter when it is empty");
    check(forwardsRestAfterFirst(1, i, 3), "2," + i + ",3", "the rest parameter after another one");
    check(forwardsRestAndList(["x", i], 1, 2), "6,1,2,x," + i + ",1,2", "the rest parameter around an array");
    check(forwardsArguments(i, "b"), "2," + i + ",b", "apply of arguments");
    check(spreadsInTailPosition([i, 2]), "2," + i + ",2", "a tail call");
    check(spreadsInTailPosition([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]), "12,1,2,3,4,5,6,7,8,9,10,11,12", "a tail call with more arguments than its caller has");
    check(spreadsBetweenInTailPosition("a", [i]), "3,a," + i + ",a", "a tail call with values around an array");
    check(appliesInTailPosition([i, , 3]), "3," + i + ",undefined,3", "apply in tail position");
    check(spreadsToHostFunction([1, i, 3]), Math.max(3, i), "a host function");
    check(spreadsToHostFunction([]), -Infinity, "a host function without arguments");
    let target = [0];
    check(spreadsToPush(target, [i, "x"]), 3, "push");
    check(target.join(), "0," + i + ",x", "what push has appended");
    check(spreadsToUnknown(describe.bind(object, "bound"), [i]), "object:2,bound," + i, "a bound function");
    check(spreadsToUnknown(new Proxy(describe, {}), [i]), "1," + i, "a proxy");
    check(thrownBy(spreadsToUnknown, undefined, [i]), "TypeError", "a callee that is undefined");
    check(thrownBy(spreadsToUnknown, {}, [i]), "TypeError", "a callee that is not callable");
    check(thrownBy(constructs.bind(null), 1), "TypeError", "spreading a number");
}

class Subclass extends Array { }
class SubclassWithIterator extends Array { *[Symbol.iterator]() { yield "from"; yield "iterator"; } }
let withOwnIterator = [1, 2, 3];
withOwnIterator[Symbol.iterator] = function* () { yield "own"; };
let withOtherPrototype = Object.setPrototypeOf([1, 2], { [Symbol.iterator]: function* () { yield "other prototype"; } });
let withInheritedElement = Object.setPrototypeOf([, 1], { __proto__: Array.prototype, 0: "own prototype" });
let withProperty = [1, 2];
withProperty.extra = true;
let frozen = Object.freeze([1, "frozen"]);
let sparse = [];
sparse[2000] = "far";
let order = [];
function* generates() { order.push("start"); yield 1; order.push("middle"); yield 2; order.push("end"); }
for (let i = 0; i < rounds; ++i) {
    check(spreads(Subclass.from([i, 2])), "2," + i + ",2", "an instance of a subclass of Array");
    check(spreads(new SubclassWithIterator(1, 2, 3)), "2,from,iterator", "a subclass with its own iterator");
    check(applies(new SubclassWithIterator(1, 2, 3)), "3,1,2,3", "apply does not iterate");
    check(spreads(withOwnIterator), "1,own", "an array with its own iterator");
    check(applies(withOwnIterator), "3,1,2,3", "apply of an array with its own iterator");
    check(spreads(withOtherPrototype), "1,other prototype", "an array with another prototype");
    check(spreads(withInheritedElement), "2,own prototype,1", "a hole when the prototype of the array has an element");
    check(applies(withInheritedElement), "2,own prototype,1", "apply of a hole when the prototype of the array has an element");
    check(spreadsTwo([i], withInheritedElement), "3," + i + ",own prototype,1", "an ordinary array and a hole when the prototype of the array has an element");
    check(spreads(withProperty), "2,1,2", "an array with a named property");
    check(spreads(frozen), "2,1,frozen", "a frozen array");
    check(spreads(other.Array.of(i, "x")), "2," + i + ",x", "an array of another realm");
    check(applies(other.Array.of(i, "x")), "2," + i + ",x", "apply of an array of another realm");
    check(spreads("text"), "4,t,e,x,t", "a string");
    check(spreads(new Set([i, "x", i])), "2," + i + ",x", "a Set");
    check(spreads(new Map([[i, "x"]])), "1," + i + ",x", "a Map");
    check(spreads(new Uint8Array([1, 2, 3])), "3,1,2,3", "a typed array");
    check(spreadsBetween("a", generates(), "b"), "4,a,1,2,b", "a generator");
    check(order.join(), "start,middle,end", "the order in which a generator runs");
    order = [];
    check(spreadsTwo([i], generates()), "3," + i + ",1,2", "an array and a generator");
    order = [];
    check(thrownBy(spreads, undefined), "TypeError", "spreading undefined");
    check(thrownBy(spreads, null), "TypeError", "spreading null");
    check(thrownBy(spreads, {}), "TypeError", "spreading an object that is not iterable");
    check(thrownBy(spreads, { [Symbol.iterator]() { throw new RangeError("no iterator"); } }), "RangeError", "an iterator method that throws");
    check(thrownBy(spreadsTwo, [1], { [Symbol.iterator]() { return { next() { throw "next"; } }; } }), "value next", "a next method that throws");
    check(thrownBy(applies, 1), "TypeError", "apply of a number");
}
check(spreads(sparse).split(",").length, 2002, "a sparse array");
check(spreads(sparse).endsWith(",undefined,far"), true, "the end of a sparse array");

function changesWhileEvaluated(list) { return describe(...list, list.push("pushed"), ...list); }
noInline(changesWhileEvaluated);
for (let i = 0; i < rounds; ++i)
    check(changesWhileEvaluated([i]), "4," + i + ",2," + i + ",pushed", "an array that changes between two spreads");

function sum() { let total = 0; for (let i = 0; i < arguments.length; ++i) total += arguments[i]; return total; }
noInline(sum);
function spreadsToSum(list) { return sum(...list); }
function appliesToSum(list) { return sum.apply(undefined, list); }
noInline(spreadsToSum);
noInline(appliesToSum);
for (let length of [1, 2, 7, 8, 9, 100, 1000, 10000]) {
    let list = Array.from({ length }, (unused, index) => index);
    check(spreadsToSum(list), length * (length - 1) / 2, "the sum of " + length + " arguments");
    check(appliesToSum(list), length * (length - 1) / 2, "the sum of " + length + " arguments passed by apply");
}
let huge = new Array(0x100001).fill(1);
for (let round = 0; round < 3; ++round) {
    check(thrownBy(spreadsToSum, huge), "RangeError", "more arguments than are allowed");
    check(thrownBy(appliesToSum, huge), "RangeError", "more arguments than are allowed, by apply");
    check(thrownBy(spreadsTwo, huge, huge), "RangeError", "two arrays that are too long together");
    check(spreadsToSum([1, 2, 3]), 6, "a call after one that failed");
}
function recurses(list) { return recurses(...list) + 1; }
noInline(recurses);
let containsItself = [];
containsItself.push(containsItself, 2, 3);
for (let round = 0; round < 3; ++round) {
    check(thrownBy(recurses, containsItself), "RangeError", "recursion through a spread call");
    check(spreads([round]), "1," + round, "a call after the stack has overflowed");
}

let kept = [];
for (let round = 0; round < 4; ++round) {
    let list = Array.from({ length: 50 }, (unused, index) => ({ index }));
    function keeps() { gc(); return Array.prototype.slice.call(arguments); }
    kept.push((function (list) { return keeps(...list, { index: "last" }); })(list));
}
check(kept.every(list => list.length === 51 && list[49].index === 49 && list[50].index === "last"), true, "arguments survive a collection in the callee");

Array.prototype[1] = "array prototype";
for (let i = 0; i < 3; ++i) {
    check(spreads([0, , 2]), "3,0,array prototype,2", "a hole when Array.prototype has an element");
    check(applies([0, , 2]), "3,0,array prototype,2", "apply of a hole when Array.prototype has an element");
    check(spreadsBetween("a", ["x", , ], "b"), "4,a,x,array prototype,b", "a hole at the end when Array.prototype has an element");
    check(spreads([0, 1, 2]), "3,0,1,2", "no hole when Array.prototype has an element");
}
delete Array.prototype[1];
Object.prototype[0] = "object prototype";
check(0 in Object.prototype, false, "an element that was assigned to Object.prototype");
check(Reflect.set(Object.prototype, 0, "object prototype"), false, "setting an element of Object.prototype");
check(thrownBy(function () { "use strict"; Object.prototype[0] = "object prototype"; }), "TypeError", "assigning to an element of Object.prototype in strict code");
check(thrownBy(Object.defineProperty, Object.prototype, 0, { value: "object prototype" }), "TypeError", "defining an element of Object.prototype");
for (let i = 0; i < 3; ++i) {
    check(spreads([, 1]), "2,undefined,1", "a hole after Object.prototype has refused an element");
    check(applies([, 1]), "2,undefined,1", "apply of a hole after Object.prototype has refused an element");
    check(spreads(withInheritedElement), "2,own prototype,1", "a hole when the prototype of the array has an element, after Array.prototype had one");
    check(applies(withInheritedElement), "2,own prototype,1", "apply of a hole when the prototype of the array has an element, after Array.prototype had one");
}
