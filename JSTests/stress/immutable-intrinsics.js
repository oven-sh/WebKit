//@ runDefault("--useImmutableIntrinsics=1")
//@ runDefault("--useImmutableIntrinsics=1", "--useAOT=1")
//@ runDefault("--useImmutableIntrinsics=1", "--useJIT=0")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function throwsTypeError(f, what) {
    let threw = false;
    try { f(); } catch (e) { threw = e instanceof TypeError; }
    check(threw, true, what + " throws a TypeError");
}
const global = globalThis;

// ---- What they have stays as it is.
const push = Array.prototype.push, floor = Math.floor, keys = Object.keys, then = Promise.prototype.then, toString = Object.prototype.toString;
(function sloppy() {
    Array.prototype.push = 1; Math.floor = 1; Object.keys = 1; Promise.prototype.then = 1; Object.prototype.toString = 1; Math.PI = 3;
    delete Array.prototype.push; delete Math.floor; delete Object.keys;
})();
check(Array.prototype.push, push, "Array.prototype.push"); check(Math.floor, floor, "Math.floor"); check(Object.keys, keys, "Object.keys");
check(Promise.prototype.then, then, "Promise.prototype.then"); check(Object.prototype.toString, toString, "Object.prototype.toString");
(function strict() {
    "use strict";
    throwsTypeError(() => { Array.prototype.push = 1; }, "assigning to Array.prototype.push");
    throwsTypeError(() => { Math.floor = 1; }, "assigning to Math.floor");
    throwsTypeError(() => { Object.prototype.toString = 1; }, "assigning to Object.prototype.toString");
    throwsTypeError(() => { delete Array.prototype.push; }, "deleting Array.prototype.push");
    throwsTypeError(() => { Object.defineProperty(Array.prototype, "push", { value: 1 }); }, "redefining Array.prototype.push");
    throwsTypeError(() => { Object.defineProperty(String.prototype, "slice", { get() { return 1; } }); }, "making String.prototype.slice an accessor");
    throwsTypeError(() => { Object.defineProperty(RegExp.prototype, "flags", { get() { return "x"; } }); }, "redefining an accessor");
})();
check(Reflect.set(Array.prototype, "push", 1), false, "Reflect.set on the intrinsic"); check(Reflect.deleteProperty(Math, "floor"), false, "Reflect.deleteProperty");
check(Reflect.defineProperty(JSON, "parse", { value: 1 }), false, "Reflect.defineProperty");
for (let [o, k] of [[Array.prototype, "push"], [Math, "floor"], [Object, "keys"], [Map.prototype, "get"], [Set.prototype, "has"], [JSON, "stringify"], [Reflect, "get"], [Function.prototype, "call"], [Number, "isInteger"], [Number.prototype, "toFixed"], [String, "fromCharCode"], [Symbol, "iterator"], [Boolean.prototype, "valueOf"], [Promise, "resolve"], [RegExp.prototype, "exec"]]) {
    let d = Object.getOwnPropertyDescriptor(o, k);
    check(d.writable, false, String(k) + " is not writable"); check(d.configurable, false, String(k) + " is not configurable");
}
for (let iterator of [[][Symbol.iterator](), new Map().entries(), new Set().values(), ""[Symbol.iterator]()])
    check(Object.getOwnPropertyDescriptor(Object.getPrototypeOf(iterator), "next").writable, false, "next of an iterator prototype");

// ---- The variables they are found in.
(function sloppy() { Math = 1; Object = 1; Array = 1; Promise = 1; global.JSON = 1; delete global.Map; })();
check(typeof Math, "object", "Math"); check(typeof Object, "function", "Object"); check(typeof Array, "function", "Array"); check(typeof JSON, "object", "JSON"); check(typeof Map, "function", "Map");
throwsTypeError(() => { "use strict"; Math = 1; }, "assigning to Math");
throwsTypeError(() => { Object.defineProperty(global, "Array", { value: 1 }); }, "redefining Array");

// ---- What inherits from them can be assigned to all the same.
(function strict() {
    "use strict";
    function Foo() { }
    Foo.prototype.toString = function () { return "a Foo"; };
    Foo.prototype.constructor = Foo; Foo.prototype.valueOf = () => 7; Foo.prototype.hasOwnProperty = () => "mine";
    check(String(new Foo), "a Foo", "toString of what inherits"); check(+new Foo, 7, "valueOf of what inherits"); check(new Foo().hasOwnProperty("x"), "mine", "hasOwnProperty");
    let o = {}; o.toString = () => "o"; check(`${o}`, "o", "a plain object's toString"); check(Object.hasOwn(o, "toString"), true, "its own");
    check(Object.getOwnPropertyDescriptor(o, "toString").writable, true, "and writable"); check(Object.getOwnPropertyDescriptor(o, "toString").enumerable, true, "and enumerable");
    let a = []; a.push = () => "pushed"; check(a.push(1), "pushed", "an array's push"); check(a.length, 0, "and nothing was");
    let f = function () { }; f.call = () => "called"; check(f.call(), "called", "a function's call"); f.bind = 1; check(f.bind, 1, "a function's bind");
    class MyArray extends Array { } MyArray.prototype.map = () => "mapped"; check(new MyArray().map(), "mapped", "a subclass's map"); MyArray.from = () => "from"; check(MyArray.from(), "from", "a subclass's from");
    class MyPromise extends Promise { } MyPromise.prototype.then = function () { return "then"; }; MyPromise.resolve = () => "resolved"; check(MyPromise.resolve(), "resolved", "a subclass's resolve");
    let m = new Map; m.get = () => "got"; check(m.get(1), "got", "a map's get");
    let heir = Object.create(Math); heir.floor = () => "floored"; check(heir.floor(1.5), "floored", "what inherits from Math"); check(Math.floor(1.5), 1, "and Math itself");
    check(Reflect.set(Array.prototype, "push", "theirs", a), true, "Reflect.set with another receiver"); check(a.push, "theirs", "lands on the receiver");
    // Over and over, so that whatever caches it does.
    function assign(o, v) { o.toString = v; o.valueOf = v; return o; }
    noInline(assign);
    for (let i = 0; i < 20000; ++i) {
        let r = assign(i & 1 ? {} : { x: 1 }, i);
        if (r.toString !== i || r.valueOf !== i) throw new Error("cached assignment at " + i);
    }
    throwsTypeError(() => assign(Object.prototype, 1), "the same code, given the intrinsic");
})();
check(Object.prototype.toString, toString, "Object.prototype.toString, still");

// ---- What is not a prototype can be given more, and that is as any other property.
Math.clamp2 = (x) => x; check(Math.clamp2(3), 3, "an addition to Math"); Object.groupBy2 = 1; check(Object.groupBy2, 1, "an addition to Object"); Promise.try2 = 1; check(Promise.try2, 1, "an addition to Promise");
Math.clamp2 = 2; check(Math.clamp2, 2, "which can be changed"); check(delete Math.clamp2, true, "and deleted"); check("clamp2" in Math, false, "and is gone"); delete Object.groupBy2; delete Promise.try2;
Math.floor.extra = 1; check(Math.floor.extra, 1, "an addition to a function"); check(delete Math.floor.extra, true, "and deleted");
Math[1] = "one"; check(Math[1], "one", "an element of Math"); check(delete Math[1], true, "and deleted");
throwsTypeError(() => { "use strict"; Intl.DateTimeFormat = 1; }, "assigning to what has not been asked for yet"); check(Reflect.deleteProperty(Intl, "PluralRules"), false, "or deleting it"); check(typeof Intl.PluralRules, "function", "it is there");
const typedArray = Object.getPrototypeOf(Uint8Array), generatorFunction = Object.getPrototypeOf(function* () { }), asyncGeneratorFunction = Object.getPrototypeOf(async function* () { });
for (let o of [Math, JSON, Reflect, Atomics, Intl, Object, Function, Array, String, Number, Boolean, Symbol, BigInt, Promise, RegExp, Date, Map, Set, WeakMap, WeakSet, WeakRef, FinalizationRegistry, Proxy, Iterator, Error, TypeError, AggregateError,
    ArrayBuffer, DataView, typedArray, Uint8Array, Float64Array, BigInt64Array, Intl.NumberFormat, Intl.Collator, generatorFunction.constructor, Math.floor, Array.prototype.push, parseInt, escape, eval]) {
    check(Object.isExtensible(o), true, "extensible"); check(Object.isFrozen(o), false, "so not frozen");
    throwsTypeError(() => { "use strict"; o.toString = 1; }, "shadowing what it inherits"); throwsTypeError(() => Object.setPrototypeOf(o, {}), "giving it another prototype");
    for (let k of Reflect.ownKeys(o)) {
        if (o === Error && k === "stackTraceLimit")
            continue;
        if (typeof o === "function" && (k === "name" || k === "length")) {
            check(Object.getOwnPropertyDescriptor(o, k).configurable, true, String(k) + " of a function is configurable");
            continue;
        }
        let d = Object.getOwnPropertyDescriptor(o, k);
        check(d.configurable, false, String(k) + " is not configurable"); check(d.writable === true && o !== RegExp, false, String(k) + " is not writable");
    }
}
{ let limit = Error.stackTraceLimit; Error.stackTraceLimit = 7; check(Error.stackTraceLimit, 7, "Error.stackTraceLimit is a setting"); Error.stackTraceLimit = limit; }
throwsTypeError(() => { "use strict"; Uint8Array.from = 1; }, "shadowing what a constructor inherits from another");

// ---- A prototype cannot.
for (let o of [Object, Function, Array, String, Number, Boolean, Symbol, BigInt, Promise, RegExp, Date, Map, Set, WeakMap, WeakSet, WeakRef, FinalizationRegistry, Iterator, Error, TypeError, RangeError, AggregateError,
    ArrayBuffer, DataView, typedArray, Uint8Array, Float64Array, BigInt64Array, Intl.NumberFormat, Intl.Collator, generatorFunction, asyncGeneratorFunction].map((o) => o.prototype).concat([generatorFunction, asyncGeneratorFunction,
    Object.getPrototypeOf(async function () { }), Object.getPrototypeOf(asyncGeneratorFunction.prototype), Object.getPrototypeOf([][Symbol.iterator]()), Object.getPrototypeOf(new Map().entries()), Object.getPrototypeOf(new Set().values()),
    Object.getPrototypeOf(""[Symbol.iterator]()), Object.getPrototypeOf("".matchAll(/a/g)), Object.getPrototypeOf([].values().map((v) => v))])) {
    check(Object.isExtensible(o), false, "not extensible"); check(Object.isFrozen(o), true, "and frozen");
    throwsTypeError(() => { "use strict"; o.somethingNew = 1; }, "adding to a prototype"); throwsTypeError(() => { "use strict"; o[1] = 1; }, "adding an element to a prototype");
    throwsTypeError(() => Object.defineProperty(o, "somethingNew", { value: 1 }), "defining on a prototype"); throwsTypeError(() => Object.setPrototypeOf(o, {}), "giving a prototype another prototype");
    check(Object.freeze(o), o, "freezing it changes nothing");
}
check("return" in [][Symbol.iterator](), false, "an iterator of the engine has no return"); check("return" in ""[Symbol.iterator](), false, "nor has that of a string");

// ---- The variables, made or not.
for (let k of ["Object", "Array", "Math", "Atomics", "Intl", "Iterator", "BigInt", "Int16Array", "Float32Array", "FinalizationRegistry", "WebAssembly", "escape", "unescape", "eval", "parseInt", "globalThis"]) {
    throwsTypeError(() => { "use strict"; global[k] = 1; }, "assigning to " + k); check(Reflect.deleteProperty(global, k), false, "deleting " + k);
    let d = Object.getOwnPropertyDescriptor(global, k);
    check(d.writable, false, k + " is not writable"); check(d.configurable, false, k + " is not configurable"); check(d.enumerable, false, k + " is not enumerable");
}
{ let d = Object.getOwnPropertyDescriptor(global, "console"); check(d.writable, true, "console is for a host to replace"); check(d.configurable, true, "or to take away"); }

// ---- An array can be given all an object can. It is no longer what the engine takes for a plain array then, and goes on working.
{
    const isPlain = typeof $vm !== "undefined" && $vm.indexingMode ? (a) => !$vm.indexingMode(a).includes("ArrayStorage") : () => undefined;
    function sum(a) { let s = ""; for (let i = 0; i < a.length; ++i) s += a[i]; for (let v of a) s += v; return s + a.indexOf(3) + a.includes(1) + a.join("") + JSON.stringify(a) + [...a].length + a.concat(a).length + a.slice(1).length; }
    noInline(sum);
    const expected = "1undefined31undefined32true13[1,null,3]362";
    for (let [what, change, afterwards] of [
        ["its own push", (a) => { a.push = () => "own"; }, (a) => a.push(4) === "own" && a.length === 3],
        ["its own toString, defined", (a) => { Object.defineProperty(a, "toString", { value: () => "own" }); }, (a) => String(a) === "own"],
        ["its own constructor", (a) => { a.constructor = Object; }, (a) => a.constructor === Object && Array.isArray(a.map((v) => v))],
        ["another prototype", (a) => { Object.setPrototypeOf(a, { __proto__: Array.prototype, push() { return "inherited"; } }); }, (a) => a.push(4) === "inherited" && a.length === 3],
        ["another prototype by __proto__", (a) => { a.__proto__ = Object.create(Array.prototype); }, (a) => a.push(4) === 4 && a.pop() === 4],
    ]) {
        let a = [1, , 3]; check(isPlain(a) !== false, true, "plain before " + what);
        for (let i = 0; i < 200; ++i) check(sum(a), expected, "before " + what);
        change(a); check(isPlain(a) !== true, true, "not after " + what);
        check(Array.isArray(a), true, "an array after " + what); check(afterwards(a), true, what);
        for (let i = 0; i < 200; ++i) check(sum(a), expected, "after " + what);
        a[5] = 6; check(a.length, 6, "it grows after " + what); a.length = 3; check(sum(a), expected, "and shrinks");
    }
    let a = [1, , 3]; a.extra = 1; a[Symbol.isConcatSpreadable] = true; Object.defineProperty(a, 0, { value: 1, writable: false }); check(sum(a), expected, "under another name, or with an element that is not writable");
    let got = 0; a = [1, , 3]; Object.defineProperty(a, 1, { get() { ++got; return 2; } }); check(a[1] + "" + a.indexOf(2) + a.join(""), "21123", "an accessor for an element"); check(got, 3, "is asked");
    a = [1, , 3]; Object.setPrototypeOf(a, { __proto__: Array.prototype, 1: "behind" }); check(a[1] + a.join(""), "behind1behind3", "an element that is inherited"); check(a[7], undefined, "and none");
    a = [1, , 3]; check(Object.setPrototypeOf(a, Array.prototype), a, "the prototype it has"); check(isPlain(a) !== false, true, "changes nothing");
    a = [1, , 3]; Object.setPrototypeOf(a, null); check(a.push, undefined, "without a prototype it has no methods"); check(Array.isArray(a), true, "and is an array"); check(a[1], undefined, "with a hole");
    // While it is being gone through.
    a = [1, 2, 3]; let seen = ""; for (let v of a) { seen += v; if (v === 1) a.push = 1; if (v === 2) Object.setPrototypeOf(a, null); } check(seen, "123", "changed in the middle of for of");
    a = [1, 2, 3]; seen = ""; for (let [i, v] of a.entries()) { seen += i + "" + v; if (!i) a.__proto__ = {}; } check(seen, "011223", "changed in the middle of entries()");
    class Loud extends Array { push() { return "loud"; } }
    let loud = new Loud(1, 2); check(loud.push(3), "loud", "a subclass's push"); check(loud.length, 2, "and nothing was"); check(Array.isArray(loud), true, "it is an array"); check(loud.map((v) => v) instanceof Loud, true, "of its species");
    loud.pop = () => "own"; check(loud.pop(), "own", "with properties of its own"); Object.defineProperty(loud, 0, { get() { return "got"; } }); check(loud[0], "got", "and accessors"); check(JSON.stringify(loud), '["got",2]', "that are asked");
    check(Object.setPrototypeOf(loud, Array.prototype), loud, "and any prototype"); check(loud.push(3), 3, "whose methods it then has");
    check(Reflect.construct(Array, [], Object).push, undefined, "constructed for another"); check(Reflect.construct(Array, [1, 2], Array).push(3), 3, "or for Array");
}
check(Array.prototype.push, push, "and what was there is still there");
check(Object.isExtensible(Object.prototype), false, "Object.prototype is not extensible");
(function sloppy() { Object.prototype.extra = 1; Object.prototype[3] = 1; Object.prototype[Symbol.iterator] = 1; })();
check({}.extra, undefined, "nothing was added to Object.prototype"); check({}[3], undefined, "nor an element"); check([, 1][0], undefined, "and holes are holes"); check({}[Symbol.iterator], undefined, "nor a symbol");
throwsTypeError(() => { "use strict"; Object.prototype.extra = 1; }, "adding to Object.prototype");
throwsTypeError(() => { "use strict"; Object.prototype[3] = 1; }, "adding an element to Object.prototype");
throwsTypeError(() => Object.defineProperty(Object.prototype, "extra", { value: 1 }), "defining on Object.prototype");
throwsTypeError(() => Object.setPrototypeOf(Object.prototype, {}), "giving Object.prototype a prototype");

// ---- And everything goes on working.
function work(n) {
    let a = [];
    for (let i = 0; i < n; ++i) a.push(Math.floor(i / 2));
    let sum = 0;
    for (let v of a) sum += v;
    let m = new Map(a.map((v, i) => [i, v])), s = new Set(a);
    for (let [k, v] of m) sum += k - v;
    for (let v of s) sum += v;
    let [x, y, ...rest] = a;
    let holey = [1, , 3]; sum += holey[1] === undefined ? 1 : 0; sum += a[n + 5] === undefined ? 1 : 0;
    return sum + x + y + rest.length + Object.keys({ a: 1, b: 2 }).length + JSON.stringify({ a }).length + "a-b".replace(/-/g, "+").length + [...a, ...s].length + a.slice(1).concat(a).length;
}
noInline(work);
let expected = work(50);
for (let i = 0; i < 3000; ++i) check(work(50), expected, "work");
let awaited = 0;
(async function () { for (let i = 0; i < 100; ++i) awaited += await i + await Promise.resolve(1) + await { then(r) { r(1); } }; })();
drainMicrotasks();
check(awaited, 4950 + 200, "await");
if (typeof createGlobalObject === "function") {
    let other = createGlobalObject();
    check(other.eval("Array.prototype.push = 1; typeof Array.prototype.push"), "function", "another realm's");
    check(other.eval("var o = {}; o.toString = 5; o.toString"), 5, "another realm's heirs");
}

// ---- Having refused an element, Object.prototype has nowhere to keep one.
throwsTypeError(() => Object.defineProperty(Object.prototype, 5, { value: 1 }), "defining an element on Object.prototype");
throwsTypeError(() => Object.defineProperty(Object.prototype, 6, { get() { return 1; } }), "defining an element with a getter on Object.prototype");
check(Reflect.set(Object.prototype, 7, 1), false, "Reflect.set of an element");
check([, 1][0], undefined, "holes are still holes");
if (typeof $vm !== "undefined" && $vm.indexingMode)
    check($vm.indexingMode(Object.prototype), "NonArray", "the indexing mode of Object.prototype");

// ---- Accessors, of either kind, go on being asked.
{
    let strictFunction = Function("'use strict'");
    throwsTypeError(() => { strictFunction.caller = 1; }, "assigning to caller of a strict function, from sloppy code");
    throwsTypeError(() => { strictFunction.arguments = 1; }, "assigning to arguments of a strict function");
    throwsTypeError(() => strictFunction.caller, "reading caller of a strict function");
    check(Object.hasOwn(strictFunction, "caller"), false, "and it has none of its own");
    throwsTypeError(() => { "use strict"; /a/.flags = "g"; }, "assigning to flags"); throwsTypeError(() => { "use strict"; new Map().size = 1; }, "assigning to size");
    /(b)/.exec("abc"); check(RegExp.$1, "b", "RegExp.$1"); RegExp.input = "given"; check(RegExp.input, "given", "RegExp.input can be given");
    let o = {}; o.__proto__ = Array.prototype; check(Object.getPrototypeOf(o), Array.prototype, "__proto__ is still a setter");
}
// What the language itself says cannot be written to refuses an heir's store too, as the book has it.
{
    let heir = { __proto__: new Set()[Symbol.iterator]() };
    heir[Symbol.toStringTag] = 25; check(heir[Symbol.toStringTag], "Set Iterator", "an heir's Symbol.toStringTag"); check(Object.hasOwn(heir, Symbol.toStringTag), false, "and it has none of its own");
    throwsTypeError(() => { "use strict"; heir[Symbol.toStringTag] = 25; }, "an heir's Symbol.toStringTag, strict");
}
