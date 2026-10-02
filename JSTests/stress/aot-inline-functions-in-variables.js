//@ runDefault("--compileMainScriptAheadOfTime=1")
(function () {
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function checkThrows(f, type, message, what) {
    try {
        f();
    } catch (error) {
        if (!(error instanceof type) || (message && error.message !== message))
            throw new Error(what + ": threw " + error);
        return error;
    }
    throw new Error(what + ": did not throw");
}
let globalObject = this;

const twice = x => x * 2;
function sumsConst(n) { let s = 0; for (let i = 0; i < n; i++) s += twice(i); return s; }
check(sumsConst(5), 20, "an arrow function in a const");

var inVar = function (x) { return x + 1; };
function sumsVar(n) { let s = 0; for (let i = 0; i < n; i++) s += inVar(i); return s; }
check(sumsVar(5), 15, "a function expression in a var");

let assignedLater;
function sumsAssignedLater(n) { let s = 0; for (let i = 0; i < n; i++) s += assignedLater(i); return s; }
checkThrows(() => sumsAssignedLater(1), TypeError, "assignedLater is not a function. (In 'assignedLater(i)', 'assignedLater' is undefined)", "a variable that has no function yet");
check(sumsAssignedLater(0), 0, "a call that is not reached");
assignedLater = x => x + 3;
check(sumsAssignedLater(5), 25, "the variable once it has its function");

function sumsInDeadZone(n) { let s = 0; for (let i = 0; i < n; i++) s += declaredBelow(i); return s; }
checkThrows(() => sumsInDeadZone(1), ReferenceError, null, "a const that is not initialized yet");
const declaredBelow = x => x + 4;
check(sumsInDeadZone(5), 30, "the const once it is initialized");

let base = 10;
const addsBase = x => x + base;
function sumsClosedOver(n) { let s = 0; for (let i = 0; i < n; i++) s += addsBase(i); return s; }
check(sumsClosedOver(5), 60, "a variable that the function closes over");
base = 20;
check(sumsClosedOver(5), 110, "after the variable changes");
let calls = 0;
const counts = () => ++calls;
function callsCounts(n) { for (let i = 0; i < n; i++) counts(); return calls; }
check(callsCounts(5), 5, "a variable that the function writes");
check(calls, 5, "as the outer function sees it");

function makeAdder(k) {
    const add = x => x + k;
    return function sumsPerActivation(n) { let s = 0; for (let i = 0; i < n; i++) s += add(i); return s; };
}
let addsOne = makeAdder(1);
let addsTen = makeAdder(10);
check(addsOne(5), 15, "a function made once for each activation");
check(addsTen(5), 60, "the one of another activation");
check(addsOne(5), 15, "the first again");

var sloppyThis = function () { return this; };
var strictThis = function () { "use strict"; return this; };
function readsSloppyThis() { let last; for (let i = 0; i < 2; i++) last = sloppyThis(); return last; }
function readsStrictThis() { let last = 1; for (let i = 0; i < 2; i++) last = strictThis(); return last; }
check(readsSloppyThis(), globalObject, "this in a sloppy function");
check(readsStrictThis(), undefined, "this in a strict function");
let holder = {
    value: 7,
    sums(n) {
        const readsThis = () => this.value;
        let s = 0;
        for (let i = 0; i < n; i++)
            s += readsThis();
        return s;
    },
};
check(holder.sums(3), 21, "this in an arrow function");

const factorial = function self(n) { return n ? n * self(n - 1) : 1; };
function sumsFactorials(n) { let s = 0; for (let i = 0; i < n; i++) s += factorial(i); return s; }
check(sumsFactorials(5), 34, "a function expression that calls itself by its own name");
const fibonacci = n => n < 2 ? n : fibonacci(n - 1) + fibonacci(n - 2);
check(fibonacci(15), 610, "an arrow function that calls itself");

const readsNewTarget = function () { return new.target; };
function callsReadsNewTarget() { let last = 1; for (let i = 0; i < 2; i++) last = readsNewTarget(); return last; }
check(callsReadsNewTarget(), undefined, "new.target in a call");

const countsArguments = function () { return arguments.length; };
function callsCountsArguments() { let s = 0; for (let i = 0; i < 2; i++) s += countsArguments(i, i, i); return s; }
check(callsCountsArguments(), 6, "a function that counts its arguments");
const readsArguments = function () { return arguments[1] + arguments.length; };
function callsReadsArguments() { let s = 0; for (let i = 0; i < 2; i++) s += readsArguments(i, 5, i); return s; }
check(callsReadsArguments(), 16, "a function that reads its arguments object");

let either = x => x + 1;
function sumsEither(n) { let s = 0; for (let i = 0; i < n; i++) s += either(i); return s; }
check(sumsEither(5), 15, "a variable before it is given another function");
either = x => x + 2;
check(sumsEither(5), 20, "a variable after it is given another function");

function sumsParameter(f, n) { let s = 0; for (let i = 0; i < n; i++) s += f(i); return s; }
check(sumsParameter(twice, 5), 20, "a function that is a parameter");
check(sumsParameter(inVar, 5), 15, "another");

const tailCallee = (n, sum) => n ? sum + n : sum;
const makesTailCall = (function () {
    "use strict";
    return function makesTailCall(n) { return tailCallee(n, 1); };
})();
check(makesTailCall(2), 3, "a tail call");

const throwsAt = x => {
    if (x === 3)
        throw new Error("thrown by the callee");
    return x;
};
function sumsThrowsAt(n) { let s = 0; for (let i = 0; i < n; i++) s += throwsAt(i); return s; }
check(sumsThrowsAt(3), 3, "a callee that does not throw yet");
let frames = checkThrows(() => sumsThrowsAt(5), Error, "thrown by the callee", "a callee that throws").stack.split("\n").map(line => line.split("@")[0]);
check(frames[0], "throwsAt", "the innermost frame");
check(frames[1], "sumsThrowsAt", "the frame of the caller");
function catchesCallee(n) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        try {
            s += throwsAt(i);
        } catch {
            s += 100;
        }
    }
    return s;
}
check(catchesCallee(5), 107, "a caller that catches what the callee throws");

const waits = async x => x + 1;
let settled = [];
function callsWaits(n) { for (let i = 0; i < n; i++) waits(i).then(value => settled.push(value)); }
callsWaits(3);
drainMicrotasks();
check(settled.join(), "1,2,3", "an async arrow function");
const generates = function* (x) { yield x; yield x + 1; };
function sumsGenerates(n) { let s = 0; for (let i = 0; i < n; i++) for (let value of generates(i)) s += value; return s; }
check(sumsGenerates(3), 9, "a generator");

check(twice.name, "twice", "the name of the function");
check(typeof twice, "function", "its type");

if (aotRemarks("sumsConst")) {
    let has = (name, remark) => aotRemarks(name).some(other => other === remark || other.startsWith(remark + ":"));
    let applies = (remark, ...names) => {
        for (let name of names) {
            if (!has(name, remark))
                throw new Error(remark + " does not apply to " + name + ": " + aotRemarks(name).join(" "));
        }
    };
    let doesNotApply = (remark, ...names) => {
        for (let name of names) {
            if (has(name, remark))
                throw new Error(remark + " applies to " + name);
        }
    };
    applies("inlined-call:twice", "sumsConst");
    applies("inlined-call:inVar", "sumsVar");
    applies("inlined-call:assignedLater", "sumsAssignedLater");
    applies("inlined-call:declaredBelow", "sumsInDeadZone");
    applies("inlined-call:addsBase", "sumsClosedOver");
    applies("inlined-call:counts", "callsCounts");
    applies("inlined-call:sloppyThis", "readsSloppyThis");
    applies("inlined-call:strictThis", "readsStrictThis");
    applies("inlined-call:throwsAt", "sumsThrowsAt");
    applies("inlined-call:countsArguments", "callsCountsArguments");
    doesNotApply("inlined-call", "sumsEither", "sumsParameter", "makesTailCall", "callsReadsArguments", "callsWaits", "sumsGenerates", "fibonacci");
    doesNotApply("direct-call", "sumsEither", "sumsParameter");
}
})();
