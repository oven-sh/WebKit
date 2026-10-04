//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function has(name, remark) {
    let remarks = typeof aotRemarks === "function" && aotRemarks(name);
    return remarks && remarks.length ? remarks.some(other => other === remark || other.startsWith(remark + ":")) : undefined;
}
function says(name, remark) { check(has(name, remark) !== false, true, name + " has " + remark); }
function doesNotSay(name, remark) { check(has(name, remark) !== true, true, name + " has no " + remark); }

(function () {
"use strict";
function takesKinds(i, d, b, s, o) { return (b ? i + d : i - d) + s.length + o.v; }
function readsThis(x) { return this.v + x; }
function takesList(a, b) { return arguments.length + a + b; }
function takesMany(a, b, c, d, e, f, g, h, i, j, k, l, m, n) { return a + b + c + d + e + f + g + h + i + j + k + l + m + n; }
function isPassedTooFew(a, b) { return b === undefined ? a : -1; }
function isPassedTooFewInList(a, b) { return arguments.length === 1 && b === undefined ? a : -1; }
function isPassedTooMany(a) { return a; }
function ignoresOne(a, b) { return a; }
function Made(x) { this.x = x; }
class Base { constructor(x) { this.x = x; } }
class Derived extends Base { constructor(x) { super(x + 1); } }
function* yields(x) { yield x; yield x + 1; }
async function awaits(x) { await 1; return x + 1; }
function hasDefault(a, b = a + 1) { return a + b; }
function hasRest(a, ...rest) { return a + rest.length; }
function mapped(v, i) { return v + i; }
function recurs(n, sum) { return n ? recurs(n - 1, sum + n) : sum; }
function tailCalled(x) { return x + 1; }
function tailCalls(x) { return tailCalled(x); }

let holder = { v: 10 };
let sum = 0;
for (let round = 0; round < 3; round++) {
    sum += takesKinds(round, 0.5, round === 1, "ab", holder);
    sum += readsThis.call(holder, round);
    sum += takesList(round, 1);
    sum += takesMany(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, round);
    sum += isPassedTooFew(round) + isPassedTooFewInList(round) + isPassedTooMany(round, "more", { });
    sum += ignoresOne(round, "never read");
    sum += new Made(round).x + new Derived(round).x;
    sum += [...yields(round)].length;
    sum += hasDefault(round) + hasDefault(round, 1) + hasRest(round) + hasRest(round, 1, 2);
    sum += [1, 2, 3].map(mapped).length;
    sum += recurs(3, round) + tailCalls(round);
}
let awaited = 0;
awaits(1).then(value => { awaited = value; });
drainMicrotasks();
check(sum, 449.5, "what the functions return");
check(awaited, 2, "what the async function returns");

let validates = typeof jscOptions === "function" && !!jscOptions().validateAOTInferredTypes;
let all = { takesKinds: 5, readsThis: 1, takesList: 2, takesMany: 14, isPassedTooFew: 2, isPassedTooFewInList: 2, isPassedTooMany: 1, ignoresOne: 2, Made: 1, Base: 1, Derived: 1, yields: 1, awaits: 1, hasDefault: 1, hasRest: 1, mapped: 2, recurs: 2, tailCalled: 1, tailCalls: 1 };
for (let name in all) {
    doesNotSay(name, "cannot-verify-argument");
    for (let parameter = 1; parameter <= all[name]; parameter++) {
        if (validates)
            says(name, "verifies-argument:" + parameter);
        else
            doesNotSay(name, "verifies-argument");
    }
}
if (validates) {
    for (let name of ["readsThis", "Made", "Base", "Derived"])
        says(name, "verifies-argument:0");
    for (let name of ["takesKinds", "takesList", "takesMany", "ignoresOne", "mapped", "recurs", "tailCalled"])
        doesNotSay(name, "verifies-argument:0");
    doesNotSay("isPassedTooMany", "verifies-argument:2");
}
says("takesList", "takes-argument-list");
says("takesMany", "takes-argument-list");
})();
