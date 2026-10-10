//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
function outcome(f) {
    try {
        let value = f();
        return value === null ? "null" : typeof value;
    } catch (error) {
        return error.constructor.name;
    }
}
function both(f) { return outcome(() => f.arguments) + " " + outcome(() => f.caller); }
function stores(f) { return outcome(() => { f.arguments = 1; return 1; }) + " " + outcome(() => { f.caller = 1; return 1; }); }

function probe() { }
const compiled = isAOTCompiled(probe);
const refused = "TypeError TypeError";

function target(a, b) { return both(target); }
function callsTarget() { return target(1, 2); }
check(callsTarget(), compiled ? refused : "object function", "a sloppy function while it runs");
check(both(target), compiled ? refused : "null null", "a sloppy function while it does not run");
check(stores(target), compiled ? refused : "number number", "stores to a sloppy function");

function neverCalled() { }
check(both(neverCalled), compiled ? refused : "null null", "a sloppy function that was never called");

function makesClosure(x) { return function closure() { return x ? both(closure) : 0; }; }
function callsClosure() { return makesClosure(1)(); }
check(callsClosure(), compiled ? refused : "object function", "a closure while it runs");

function strict() { "use strict"; return both(strict); }
const arrow = () => both(arrow);
const holder = { method() { return both(holder.method); } };
class Class { static run() { return both(Class); } }
check(strict(), refused, "a strict function");
check(arrow(), refused, "an arrow function");
check(holder.method(), refused, "a method");
check(Class.run(), refused, "a class");
check(both(Math.max), refused, "a native function");
check(both([].map), refused, "a built-in function");

globalThis.guest = new Function("return both(guest);");
function callsGuest() { return guest(); }
check(callsGuest(), compiled ? "object null" : "object function", "a function made at run time, called by the program");
check(both(guest), "null null", "a function made at run time while it does not run");
check(stores(guest), "number number", "stores to a function made at run time");

function calledByGuest() { return both(calledByGuest); }
check(new Function("return calledByGuest();")(), compiled ? refused : "object function", "a function of the program called by one made at run time");
globalThis.inner = new Function("return both(inner);");
check(new Function("return inner();")(), "object function", "functions made at run time among themselves");
