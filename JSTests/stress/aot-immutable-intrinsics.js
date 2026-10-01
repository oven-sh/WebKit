//@ runDefault("--useImmutableIntrinsics=1", "--compileMainScriptAheadOfTime=1", "--aotUseLiveScopes=1")
//@ runDefault("--useImmutableIntrinsics=1", "--compileMainScriptAheadOfTime=1", "--aotUseLiveScopes=1", "--useDFGJIT=0")
//@ runDefault("--useImmutableIntrinsics=1", "--compileMainScriptAheadOfTime=1")
// Code that is compiled ahead of time takes the built-in objects for what they are. This is about when it may, and when it may not.
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function repeat(f, ...args) { let r; for (let i = 0; i < 300; ++i) r = f(...args); return r; }

// ---- What it may.
function statics(x, y) { return Math.floor(x) + Math.max(x, y) + Math.min(x, y) + Math.abs(-x) + Math.sqrt(16) + Math.ceil(x) + Math.trunc(y) + Math.imul(3, 4) + Math.round(x) + Math.pow(2, 3) + Math.hypot(3, 4) + Math.sign(-y); }
check(repeat(statics, 1.5, 2.5), 1 + 2.5 + 1.5 + 1.5 + 4 + 2 + 2 + 12 + 2 + 8 + 5 - 1, "Math");
function constants() { return [Math.PI, Math.E, Number.MAX_SAFE_INTEGER, Number.EPSILON, Number.MIN_VALUE, Number.NaN, Number.POSITIVE_INFINITY, Math.SQRT2].join(); }
check(repeat(constants), [3.141592653589793, 2.718281828459045, 9007199254740991, 2.220446049250313e-16, 5e-324, NaN, Infinity, 1.4142135623730951].join(), "constants");
function objects(o) { return Object.keys(o).join() + Object.values(o).join() + Object.entries(o).length + JSON.stringify(Object.assign({}, o, { z: 1 })) + Object.getPrototypeOf(o).constructor.name + Object.hasOwn(o, "a") + Array.isArray(o) + Array.isArray([o]) + Array.from("ab").length + Array.of(1, 2).length; }
check(repeat(objects, { a: 1, b: 2 }), 'a,b1,22{"a":1,"b":2,"z":1}Objecttruefalsetrue22', "Object and Array");
function borrowed(a, o) { return Array.prototype.slice.call(a, 1).join() + Object.prototype.hasOwnProperty.call(o, "x") + Object.prototype.toString.call(a) + Function.prototype.call.call(function () { return this.x; }, o) + String.prototype.toUpperCase.call("ab") + Array.prototype.map.call("ab", c => c + c).join(""); }
check(repeat(borrowed, [1, 2, 3], { x: 5 }), "2,3true[object Array]5ABaabb", "methods borrowed from prototypes");
function called(v) { return String(v) + Number("4") + Boolean(v) + typeof Symbol("s") + Object(v).constructor.name + Array(3).length + typeof Function("return 1") + RegExp("a+").source; }
check(repeat(called, 7), "74truesymbolNumber3functiona+", "called as functions");
function constructed(v) { let m = new Map([[1, v]]), s = new Set([v, v]), p = new Promise(r => r(v)), a = new Array(2), o = new Object, r = new RegExp("b", "g"), n = new Number(v), t = new String("s"); return "" + m.get(1) + s.size + (p instanceof Promise) + a.length + typeof o + r.flags + n + t + (m instanceof Map) + (s instanceof Set) + ([] instanceof Array) + (r instanceof RegExp) + (o instanceof Object) + (constructed instanceof Function); }
check(repeat(constructed, 3), "31true2objectg3struetruetruetruetruetrue", "constructed");
function symbols(a) { let it = a[Symbol.iterator](); return typeof Symbol.iterator + String(Symbol.asyncIterator) + it.next().value + (Symbol.for("k") === Symbol.for("k")) + Symbol.keyFor(Symbol.for("k")) + typeof a[Symbol.unscopables]; }
check(repeat(symbols, [9]), "symbolSymbol(Symbol.asyncIterator)9truekobject", "symbols");
function others(o) { return JSON.stringify(o) + JSON.parse("[1]")[0] + Reflect.ownKeys(o).join() + Reflect.has(o, "a") + Reflect.get(o, "a") + Number.isInteger(4) + Number.parseFloat("1.5") + Number.isNaN(NaN) + String.fromCharCode(65) + typeof Promise.resolve(1).then + typeof Promise.all; }
check(repeat(others, { a: 1 }), '{"a":1}1atrue1true1.5trueAfunctionfunction', "JSON, Reflect, Number, String, Promise");
function identity() { return Math === globalThis.Math && Object.prototype === Object.getPrototypeOf({}) && Array.prototype === Object.getPrototypeOf([]) && Array.prototype.constructor === Array && Function.prototype === Object.getPrototypeOf(identity) && Math.floor === globalThis.Math.floor && typeof Math === "object" && typeof Map === "function"; }
check(repeat(identity), true, "they are the ones everybody else sees");
function loops(n) { let t = 0, a = []; for (let i = 0; i < n; ++i) { t += Math.floor(i / 3) + Math.max(i, 5) + Math.sqrt(i * i) + Math.abs(-i) + Math.PI; if (Number.isInteger(i / 2)) a.push(Math.min(i, 7)); } return Math.round(t) + a.length; }
check(repeat(loops, 100), (() => { let t = 0, c = 0; for (let i = 0; i < 100; ++i) { t += Math.floor(i / 3) + Math.max(i, 5) + i + i + Math.PI; if (i % 2 == 0) c++; } return Math.round(t) + c; })(), "in loops");
function notNumbers(a, b) { return Math.floor(a) + Math.max(a, b) + Math.abs(b); }
check(repeat(notNumbers, "3.7", { valueOf() { return -2; } }), 3 + 3.7 + 2, "arguments that are not numbers"); check(repeat(notNumbers, undefined, 1), NaN, "nor anything like them");
function thisOfCall() { return [String, Number].map(f => f(1)).join() + (0, Math.floor)(2.5) + Math["floor"](3.5); }
check(repeat(thisOfCall), "1,123", "however they are got at");

// ---- What it may not: the name is somebody else's.
function parameter(Math) { return Math.floor(1.5); }
check(repeat(parameter, { floor: () => "parameter" }), "parameter", "a parameter");
function local() { let Math = { floor: () => "local" }, Object = { keys: () => "local keys" }; return Math.floor(1.5) + Object.keys({}); }
check(repeat(local), "locallocal keys", "a local");
function closure() { const Array = { isArray: () => "closure" }, JSON = { stringify: () => "closed over" }; return () => Array.isArray([]) + JSON.stringify(1); }
check(repeat(closure()), "closureclosed over", "a variable of an enclosing function");
function inner() { function Map() { this.mine = true; } class Set { get size() { return "mine"; } } return new Map().mine + new Set().size; }
check(repeat(inner), "truemine", "a function or a class declared inside");
function caught() { try { throw { floor: () => "caught" }; } catch (Math) { return Math.floor(1); } }
check(repeat(caught), "caught", "the parameter of a catch");
function withStatement(o) { with (o) { return Math.floor(1.5) + String(Object.keys({ a: 1 })); } }
check(repeat(withStatement, { Math: { floor: () => "with" } }), "witha", "with"); check(repeat(withStatement, {}), "1a", "with, without");
function evalInjects(source) { eval(source); return Math.floor(1.5); }
check(repeat(evalInjects, "var Math = { floor: () => 'eval' }"), "eval", "eval declares it"); check(repeat(evalInjects, ""), 1, "eval does not");
function evalInjectsOutside(source) { eval(source); return (() => Math.floor(1.5))(); }
check(repeat(evalInjectsOutside, "var Math = { floor: () => 'outer eval' }"), "outer eval", "eval declares it in the function around");
function defaulted(Math = { floor: () => "default" }) { return Math.floor(1.5); }
check(repeat(defaulted), "default", "a parameter with a default");
function destructured({ Math, Object: O }) { return Math.floor(1.5) + O; }
check(repeat(destructured, { Math: { floor: () => "destructured" }, Object: 1 }), "destructured1", "destructured");
function tdz() { try { return Math.floor(1); let Math; } catch (e) { return e.constructor.name; } }
check(repeat(tdz), "ReferenceError", "before its declaration");

// ---- What is not fixed is looked up like anything else.
function added() { return Math.added; }
check(repeat(added), undefined, "not there"); Math.added = 1; check(repeat(added), 1, "added"); Math.added = 2; check(repeat(added), 2, "changed"); delete Math.added; check(repeat(added), undefined, "deleted");
function addedMethod(a) { return Array.prototype.addedMethod ? a.addedMethod() : "none"; }
check(repeat(addedMethod, [1]), "none", "no such method"); Array.prototype.addedMethod = function () { return this.length; }; check(repeat(addedMethod, [1]), 1, "a method that was added"); delete Array.prototype.addedMethod;
function accessors() { return typeof Object.getOwnPropertyDescriptor(RegExp.prototype, "flags").get + RegExp.prototype.source + Map.prototype[Symbol.toStringTag] + Array[Symbol.species].name; }
check(repeat(accessors), "function(?:)MapArray", "accessors are called");
function inheritedByIntrinsic() { return Math.toString() + typeof Math.hasOwnProperty + Array.name + Array.length + typeof Array.call + Math.floor.name + Math.floor.length; }
check(repeat(inheritedByIntrinsic), "[object Math]functionArray1functionfloor1", "what they inherit, and what their functions have");
function ownOfFunction() { return Math.floor.extra; }
check(repeat(ownOfFunction), undefined, "the functions are not fixed"); Math.floor.extra = 5; check(repeat(ownOfFunction), 5, "and can be given properties"); delete Math.floor.extra;
function otherGlobals() { return typeof print + typeof undefinedGlobal + typeof Intl + typeof Float64Array; }
check(repeat(otherGlobals), "functionundefinedobjectfunction", "globals that are not fixed");
// ---- Of some, only the variable is fixed.
function dates() { let d = new Date(0); return typeof Date.now() + d.getTime() + Date.UTC(1970, 0, 1) + (d instanceof Date) + Date.prototype.getTime.call(d) + typeof Date.parse; }
check(repeat(dates), "number00true0function", "Date");
function errors(m) { let all = [new Error(m), new TypeError(m), RangeError(m), new SyntaxError(m), new ReferenceError(m), new EvalError(m), new URIError(m), new AggregateError([], m)]; return all.map(e => e.name + e.message + (e instanceof Error)).join(); }
check(repeat(errors, "!"), "Error!true,TypeError!true,RangeError!true,SyntaxError!true,ReferenceError!true,EvalError!true,URIError!true,AggregateError!true", "errors");
function thrower(x) { if (x) throw new TypeError("thrown"); return 1; } function catches(x) { try { return thrower(x); } catch (e) { return e instanceof TypeError && e.constructor === TypeError && e.message; } }
check(repeat(catches, true), "thrown", "thrown"); check(repeat(catches, false), 1, "not thrown");
function errorStatics() { return Error.stackTraceLimit; }
let limit = Error.stackTraceLimit; check(repeat(errorStatics), limit, "Error.stackTraceLimit"); Error.stackTraceLimit = 3; check(repeat(errorStatics), 3, "can be assigned to"); Error.stackTraceLimit = limit;
Error.prepareStackTrace = 1; check(Error.prepareStackTrace, 1, "and Error.prepareStackTrace"); delete Error.prepareStackTrace;
function functions() { return "" + parseInt("12px") + parseFloat("1.5") + isNaN("x") + isFinite(1) + encodeURIComponent("a b") + decodeURIComponent("%20") + encodeURI("a b") + decodeURI("a") + (globalThis === this0) + typeof BigInt(1) + new Uint8Array(2).length + new WeakMap().has({}) + new WeakSet().has({}) + new ArrayBuffer(4).byteLength + typeof new Proxy({}, {}) + new DataView(new ArrayBuffer(1)).byteLength + typeof new WeakRef({}).deref(); }
var this0 = this;
check(repeat(functions), "121.5truetruea%20b a%20batruebigint2falsefalse4object1object", "functions and the rest");
function storesToVariables() { Error = 1; Date = 2; parseInt = 3; delete this0.TypeError; return typeof Error + typeof Date + typeof parseInt + typeof TypeError; }
check(repeat(storesToVariables), "functionfunctionfunctionfunction", "their variables stay");
globalThis.mutableGlobal = 1; function readsMutable() { return mutableGlobal; } check(repeat(readsMutable), 1, "a global"); mutableGlobal = 2; check(repeat(readsMutable), 2, "a global that changed");

// ---- Stores are refused as they would be anyway.
function stores() { Math = 1; Math.floor = 2; Object.keys = 3; delete Math.floor; return typeof Math + typeof Math.floor + typeof Object.keys; }
check(repeat(stores), "objectfunctionfunction", "stores, sloppy");
function strictStores(which) { "use strict"; try { if (which == 0) Math = 1; else if (which == 1) Math.floor = 2; else delete Object.keys; return "no exception"; } catch (e) { return e.constructor.name; } }
for (let i = 0; i < 3; ++i) check(repeat(strictStores, i), "TypeError", "stores, strict");

// ---- A later script cannot take the name either.
if (typeof loadString === "function") {
    for (let source of ["let Math = 1;", "const Object = 1;", "class Map { }", "function Array() { }"]) {
        let threw = false; try { loadString(source); } catch (e) { threw = true; } check(threw, true, source + " is refused");
    }
    loadString("var Math; var JSON;"); check(typeof Math, "object", "var does nothing");
    check(repeat(statics, 1.5, 2.5), 40.5, "and it is all as it was");
}

// ---- Another realm has its own.
if (typeof createGlobalObject === "function") {
    let other = createGlobalObject();
    let theirs = other.eval("(" + identity.toString() + ")"), theirArray = other.eval("(function () { return [Array, Math, Object.prototype, Math.floor, new Map().constructor, Symbol.iterator]; })");
    check(repeat(theirs), true, "another realm's are its own");
    let [A, M, OP, floor, MapOfTheirs, iterator] = repeat(theirArray);
    check(A === other.Array && A !== Array, true, "Array"); check(M === other.Math && M !== Math, true, "Math"); check(OP === other.Object.prototype && OP !== Object.prototype, true, "Object.prototype");
    check(floor === other.Math.floor && floor !== Math.floor, true, "Math.floor"); check(MapOfTheirs === other.Map, true, "Map"); check(iterator, Symbol.iterator, "well-known symbols are everybody's");
}
gc();
check(repeat(statics, 1.5, 2.5), 40.5, "after a collection"); check(repeat(borrowed, [1, 2, 3], { x: 5 }), "2,3true[object Array]5ABaabb", "after a collection");
