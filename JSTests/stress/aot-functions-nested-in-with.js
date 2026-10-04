//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}

function callsMethod(o) { with (o) { return function () { return push(4); }; } }
function readsLength(o) { with (o) { return function () { return length; }; } }
function strictInside(o) { with (o) { return function () { "use strict"; return [typeof length, lastIndexOf(4)]; }; } }
function twoLevels(o) { with (o) { return function () { return function () { return indexOf(2); }; }; } }
function assigns(o) { with (o) { return function (v) { length = v; }; } }
function returnsReceiver(o) { with (o) { return function () { return me(); }; } }
function arrow(o) { with (o) { return () => join("-"); } }

let array = [1, 2, 3];
check(callsMethod(array)(), 4, "a method of an array");
check(array.length, 4, "the array is the receiver");
check(readsLength(array)(), 4, "the length of an array");
check(readsLength(function (a, b) { })(), 2, "the length of a function");
check(readsLength(new Uint8Array(7))(), 7, "the length of a typed array");
check(readsLength("text")(), 4, "the length of a string object");
check(readsLength({ length: "own" })(), "own", "a plain object");
globalThis.length = "global";
check(readsLength(new Map([[1, 2]]))(), "global", "an object without the name: the global object answers");
check(strictInside(array).call(undefined).join(), "number,3", "a strict function in a with statement");
check(twoLevels(array)()(), 1, "two functions deep");
assigns(array)(2);
check(array.join(), "1,2", "an assignment through the with statement");
check(arrow(array)(), "1-2", "an arrow function");

for (let receiver of [[], function () { }, new Uint8Array(1), new Map, { }]) {
    receiver.me = function () { return this; };
    check(returnsReceiver(receiver)(), receiver, "the object of the with statement is the receiver");
}

function evaluates(code) { eval(code); return function () { return injected; }; }
check(evaluates("var injected = 5")(), 5, "a variable that eval adds");
