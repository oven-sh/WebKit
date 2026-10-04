//@ runDefault("-m", "--compileMainScriptAheadOfTime=1")
//@ runDefault("-m", "--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("-m", "--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
//@ runDefault("-m", "--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1")
//@ runDefault("-m", "--useImmutableIntrinsics=1")
//@ runDefault("-m")

const isImmutable = !Object.getOwnPropertyDescriptor(Math, "floor").writable;
const failures = [];

function show(value)
{
    if (typeof value === "function")
        return "a function";
    try {
        return String(value);
    } catch {
        return typeof value;
    }
}

function check(actual, expected, what)
{
    if (!Object.is(actual, expected))
        failures.push(what + ": " + show(actual) + " instead of " + show(expected));
}

function outcomeOf(run)
{
    try {
        return run();
    } catch (error) {
        return error.constructor;
    }
}

function refuses(what, run)
{
    if (isImmutable)
        check(outcomeOf(run), TypeError, what);
}

function answersFalse(what, run)
{
    if (isImmutable)
        check(outcomeOf(run), false, what);
}

function allows(what, run, expected)
{
    check(outcomeOf(run), expected, what);
}

function mine() { return "mine"; }

refuses("writing a built-in property", () => { Math.ceil = mine; });
refuses("deleting a built-in property", () => delete Math.ceil);
refuses("redefining a built-in property", () => Object.defineProperty(Math, "ceil", { value: mine }));
refuses("redefining the name of a built-in function", () => Object.defineProperty(Math.floor, "name", { value: "other" }));
refuses("deleting the length of a built-in function", () => delete Math.floor.length);
for (const [path, untouched] of [["Math.hypot", Math.hypot], ["Array.prototype.forEach", Array.prototype.forEach], ["Array.prototype.flatMap", Array.prototype.flatMap], ["String.prototype.padEnd", String.prototype.padEnd], ["Object.entries", Object.entries], ["Promise.prototype.finally", Promise.prototype.finally]]) {
    refuses("deleting the name of " + path + " before anything read it", () => delete untouched.name);
    refuses("redefining the length of " + path + " before anything read it", () => Object.defineProperty(untouched, "length", { value: 99 }));
    refuses("writing the name of " + path, () => { untouched.name = "other"; });
    for (const key of ["name", "length"]) {
        const descriptor = Object.getOwnPropertyDescriptor(untouched, key);
        check([descriptor.writable, descriptor.enumerable, descriptor.configurable].join(), "false,false," + !isImmutable, "the attributes of the " + key + " of " + path);
    }
    check(untouched.name, path.slice(path.lastIndexOf(".") + 1), "the name of " + path);
    check(Reflect.ownKeys(untouched).sort().join(), "length,name", "the own keys of " + path);
    refuses("call on " + path, () => { untouched.call = mine; });
    allows("a new name on " + path, () => { untouched.added = 1; return untouched.added; }, 1);
}

refuses("call on a built-in function", () => { Math.floor.call = mine; });
refuses("apply on a built-in function", () => { Math.floor.apply = mine; });
refuses("bind on a built-in function", () => { Math.floor.bind = mine; });
refuses("toString on a built-in function", () => { Math.floor.toString = mine; });
refuses("hasOwnProperty on a built-in function", () => { Math.floor.hasOwnProperty = mine; });
refuses("call on a method", () => { Object.prototype.hasOwnProperty.call = mine; });
refuses("call on a global function", () => { parseInt.call = mine; });
refuses("call on a constructor", () => { Array.call = mine; });
refuses("defining call", () => Object.defineProperty(Object.prototype.hasOwnProperty, "call", { value: mine }));
refuses("defining call as an accessor", () => Object.defineProperty(Math.sin, "call", { get: mine }));
refuses("defining call by __defineGetter__", () => Math.tan.__defineGetter__("call", mine));
refuses("defining Symbol.hasInstance", () => Object.defineProperty(Math.exp, Symbol.hasInstance, { value: mine }));
refuses("assigning call by Object.assign", () => Object.assign(Math.cos, { call: mine }));
refuses("toString on a holder", () => { Math.toString = mine; });
refuses("hasOwnProperty on a holder", () => { JSON.hasOwnProperty = mine; });
refuses("constructor on a holder", () => { Reflect.constructor = mine; });
refuses("hasOwnProperty on a prototype", () => { Array.prototype.hasOwnProperty = mine; });
refuses("valueOf on a prototype", () => { Map.prototype.valueOf = mine; });
answersFalse("Reflect.set of call", () => Reflect.set(Math.abs, "call", mine));
answersFalse("Reflect.defineProperty of call", () => Reflect.defineProperty(Math.abs, "call", { value: mine }));

refuses("another prototype for a built-in function", () => Object.setPrototypeOf(Math.ceil, { call: mine }));
refuses("no prototype for a built-in function", () => Object.setPrototypeOf(Math.round, null));
refuses("__proto__ of a built-in function", () => { Math.trunc.__proto__ = { }; });
refuses("another prototype for a holder", () => Object.setPrototypeOf(Math, { }));
refuses("another prototype for a prototype", () => Object.setPrototypeOf(Array.prototype, { }));
refuses("another prototype for a constructor", () => Object.setPrototypeOf(Map, { }));
answersFalse("Reflect.setPrototypeOf", () => Reflect.setPrototypeOf(Math.sqrt, null));
allows("the same prototype again", () => Object.setPrototypeOf(Math.sqrt, Function.prototype) === Math.sqrt, true);
allows("the same prototype again, by Reflect", () => Reflect.setPrototypeOf(Math, Object.prototype), true);

allows("Math.floor.call is what it was", () => Math.floor.call(null, 1.5), 1);
allows("hasOwnProperty.call is what it was", () => Object.prototype.hasOwnProperty.call({ a: 1 }, "a"), true);
allows("Math.ceil.call is what it was", () => Math.ceil.call(null, 1.5), 2);
allows("typeof Math.round.call", () => typeof Math.round.call, "function");
allows("Math.floor.name", () => Math.floor.name, "floor");
allows("Math.floor.length", () => Math.floor.length, 1);

allows("a new name on a holder", () => { Math.added = 1; return Math.added; }, 1);
allows("a new name on a built-in function", () => { Math.floor.added = 2; return Math.floor.added; }, 2);
allows("writing it again", () => { Math.floor.added = 3; return Math.floor.added; }, 3);
allows("deleting it", () => delete Math.floor.added && !("added" in Math.floor), true);
allows("defining a new name on a built-in function", () => Object.defineProperty(Math.floor, "defined", { value: 4, configurable: true }).defined, 4);
allows("a new name on a prototype", () => { Object.defineProperty(Array.prototype, "added", { value: 5, configurable: true, writable: true }); return [].added; }, 5);
allows("deleting it from the prototype", () => delete Array.prototype.added && [].added, undefined);
allows("a new name on Function.prototype", () => { Function.prototype.addedToAll = 6; return Math.floor.addedToAll; }, 6);
allows("a built-in function shadows a name the program added", () => { Math.floor.addedToAll = 7; return Math.floor.addedToAll + Math.ceil.addedToAll; }, 13);
allows("deleting both", () => delete Math.floor.addedToAll && delete Function.prototype.addedToAll && Math.floor.addedToAll, undefined);
allows("many new names on a holder", () => { for (let i = 0; i < 200; ++i) JSON["added" + i] = i; return JSON.added199; }, 199);
refuses("toString on a holder with many names", () => { JSON.toString = mine; });
allows("Error.stackTraceLimit", () => { const old = Error.stackTraceLimit; Error.stackTraceLimit = 7; const seen = Error.stackTraceLimit; Error.stackTraceLimit = old; return seen; }, 7);

allows("an object shadows toString", () => { const object = { }; object.toString = mine; return object.toString(); }, "mine");
allows("an object defines hasOwnProperty", () => Object.defineProperty({ }, "hasOwnProperty", { value: mine }).hasOwnProperty(), "mine");
allows("an array shadows push", () => { const array = []; array.push = mine; return array.push(); }, "mine");
allows("a map shadows get", () => { const map = new Map; map.get = mine; return map.get(); }, "mine");
allows("a subclass overrides push", () => new (class extends Array { push() { return "mine"; } })().push(), "mine");
allows("a subclass has a static from", () => (class extends Array { static from() { return "mine"; } }).from(), "mine");
allows("an inheritor of a holder overrides", () => { const object = Object.create(Math); object.floor = mine; return object.floor(); }, "mine");
allows("a function of the program gets its own call", () => { function f() { return "f"; } f.call = mine; return f.call(); }, "mine");
allows("a function of the program defines its own apply", () => { function f() { return "f"; } Object.defineProperty(f, "apply", { value: mine }); return f.apply(); }, "mine");
allows("a class with a static call", () => (class { static call() { return "mine"; } }).call(), "mine");
allows("a function of the program gets another prototype", () => { function f() { } Object.setPrototypeOf(f, { call: mine }); return f.call(); }, "mine");
allows("a function of the program loses its prototype", () => { function f() { } Object.setPrototypeOf(f, null); return typeof f.call; }, "undefined");
allows("a bound built-in gets its own call", () => { const bound = Math.floor.bind(null); bound.call = mine; return bound.call(); }, "mine");

if (isImmutable) {
    check(new Function("Math.max.call = function () { return 'mine'; }; return Math.max.call(null, 1, 2);")(), 2, "a sloppy guest's assignment is ignored");
    check((0, eval)("Math.min.call = function () { return 'mine'; }; Math.min.call(null, 1, 2)"), 1, "a sloppy guest's assignment is ignored");
    check(outcomeOf(new Function("'use strict'; Math.max.call = function () { };")), TypeError, "a strict guest's assignment");
    check(outcomeOf(new Function("Object.defineProperty(Math.max, 'call', { value: 1 });")), TypeError, "a sloppy guest's definition");
    check(outcomeOf(new Function("Object.setPrototypeOf(Math.max, null);")), TypeError, "a sloppy guest's setPrototypeOf");
    check(outcomeOf(new Function("Math.max.__proto__ = null;")), TypeError, "a sloppy guest's __proto__");
    check(typeof Math.max.call, "function", "Math.max.call is what it was");
}

function remarksOf(name)
{
    return typeof aotRemarks === "function" ? aotRemarks(name) : null;
}

function has(name, remark)
{
    const remarks = remarksOf(name);
    if (remarks && !remarks.includes(remark))
        failures.push(name + " lacks " + remark);
}

function hasNot(name, remark)
{
    const remarks = remarksOf(name);
    if (remarks && remarks.includes(remark))
        failures.push(name + " has " + remark);
}

function folds()
{
    var floor = Math.floor;
    var hasOwnProperty = Object.prototype.hasOwnProperty;
    var slice = Array.prototype.slice;
    var max = Math.max;

    function program(x) { return x + 1; }
    function withOwnCall(x) { return x + 1; }
    withOwnCall.call = mine;

    function callByPath(x) { return Math.floor.call(null, x); }
    function callByAlias(x) { return floor.call(null, x); }
    function sliceByPath(array) { return Array.prototype.slice.call(array, 1); }
    function sliceByAlias(array) { return slice.call(array, 1); }
    function ownKeys(object) { var keys = ""; for (var key in object) { if (hasOwnProperty.call(object, key)) keys += key; } return keys; }
    function applyByPath(list) { return Math.max.apply(null, list); }
    function applyByAlias(list) { return max.apply(null, list); }
    function bindByPath() { return Math.floor.bind(null, 2.5); }
    function callOfProgramFunction(x) { return program.call(null, x); }
    function callOfFunctionWithOwnCall(x) { return withOwnCall.call(null, x); }
    function callOfAnything(f, x) { return f.call(null, x); }
    function callOfConstructorThatIsNotFixed() { return Error.call(null, "message").message; }
    function callBeforeInitialization() { return later.call(null, 1.5); }

    check(outcomeOf(callBeforeInitialization), TypeError, "call of a variable that has no value yet");
    var later = Math.floor;

    for (let i = 0; i < 100; ++i) {
        check(callByPath(i + 0.5), i, "call by path");
        check(callByAlias(i + 0.5), i, "call by an alias");
        check(sliceByPath([i, i + 1]).join(), String(i + 1), "slice.call by path");
        check(sliceByAlias([i, i + 1]).join(), String(i + 1), "slice.call by an alias");
        check(ownKeys({ a: 1, b: i }), "ab", "hasOwnProperty.call");
        check(applyByPath([i, i + 1]), i + 1, "apply by path");
        check(applyByAlias([i, i + 1]), i + 1, "apply by an alias");
        check(bindByPath()(), 2, "bind by path");
        check(callOfProgramFunction(i), i + 1, "call of a function of the program");
        check(callOfFunctionWithOwnCall(i), "mine", "call of a function with its own call");
        check(callOfAnything(i & 1 ? Math.ceil : program, 1.5), i & 1 ? 2 : 2.5, "call of anything");
        check(callOfConstructorThatIsNotFixed(), "message", "call of Error");
        check(callBeforeInitialization(), 1, "call of a variable that has its value");
    }

    for (const name of ["callByPath", "sliceByPath"]) {
        hasNot(name, "calls:GetById");
        hasNot(name, "cached-call");
    }
    for (const name of ["callByAlias", "sliceByAlias", "ownKeys", "callBeforeInitialization"]) {
        has(name, "reads-member-of-function-prototype:call");
        has(name, "folds-branch-by-type");
    }
    has("callByPath", "lowered-builtin:Math.floor");
    has("callByAlias", "lowered-builtin:Math.floor");
    has("callByAlias", "reads-member-of-function-prototype:call");
    has("callByAlias", "folds-branch-by-type");
    has("sliceByAlias", "lowered-builtin:Array.prototype.slice");
    has("ownKeys", "lowered-builtin:Object.prototype.hasOwnProperty");
    has("applyByAlias", "reads-member-of-function-prototype:apply");
    hasNot("applyByPath", "calls:GetById");
    hasNot("bindByPath", "calls:GetById");
    for (const name of ["callOfProgramFunction", "callOfFunctionWithOwnCall", "callOfAnything", "callOfConstructorThatIsNotFixed"])
        hasNot(name, "reads-member-of-function-prototype:call");
}
check(outcomeOf(folds), undefined, "the calls through call, apply and bind");

if (failures.length)
    throw new Error(failures.length + " failures:\n" + failures.join("\n"));
