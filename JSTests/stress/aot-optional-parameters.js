//@ runDefault("--compileMainScriptAheadOfTime=1")
(function () {
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}

function scaled(a, b = 2) { return a * b; }
function callsScaled(a) { return scaled(a); }
function callsScaledWith(a, b) { return scaled(a, b); }
function sumsScaled(n) {
    let sum = 0;
    for (let i = 0; i < n; i++)
        sum += scaled(i);
    return sum;
}
function sumsScaledWithConstant(n) {
    let sum = 0;
    for (let i = 0; i < n; i++)
        sum += scaled(i, 3);
    return sum;
}
check(callsScaled(3), 6, "omitted");
check(callsScaledWith(3, undefined), 6, "undefined");
check(callsScaledWith(3, null), 0, "null");
check(callsScaledWith(3, 0), 0, "zero");
check(callsScaledWith(3, -0), -0, "negative zero");
check(callsScaledWith(3, false), 0, "false");
check(callsScaledWith(3, ""), 0, "the empty string");
check(callsScaledWith(3, "4"), 12, "a string");
check(callsScaledWith(3, 1.5), 4.5, "a double");
check(callsScaledWith(3, NaN), NaN, "NaN");
check(callsScaledWith(3, { valueOf() { return 5; } }), 15, "an object");
check(sumsScaled(10), 90, "omitted, in a loop");
check(sumsScaledWithConstant(10), 135, "a constant, in a loop");
check(scaled.length, 1, "the length of the function");
check(scaled(2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12), 6, "more arguments than registers");

function large(a, b = 2) {
    let sum = 0;
    for (let i = 0; i < 3; i++)
        sum += a * b + i;
    if (sum > 1e9)
        sum = 0;
    if (sum < -1e9)
        sum = 1;
    if (sum === 77)
        sum = 2;
    if (sum === 78)
        sum = 3;
    return sum;
}
function sumsLarge(n) {
    let sum = 0;
    for (let i = 0; i < n; i++)
        sum += large(i);
    return sum;
}
function sumsLargeWith(n, b) {
    let sum = 0;
    for (let i = 0; i < n; i++)
        sum += large(i, b);
    return sum;
}
check(sumsLarge(5), 75, "a function that is not inlined");
check(sumsLargeWith(5, 3), 105, "with an integer");
check(sumsLargeWith(5, 0.5), 30, "with a double");
check(sumsLargeWith(5, undefined), 75, "with undefined");
check(sumsLargeWith(2, "2"), 12, "with a string");

let order = [];
function note(what, value) { order.push(what); return value; }
function inOrder(a = note("a", 1), b = note("b", a + 1), c = note("c", a + b)) { return a + "," + b + "," + c; }
check(inOrder(), "1,2,3", "each from the ones before");
check(inOrder(5), "5,6,11", "the first is passed");
check(inOrder(undefined, 7), "1,7,8", "the second is passed");
check(inOrder(1, 2, 3), "1,2,3", "all are passed");
check(order.join(), "a,b,c,b,c,a,c", "only the ones that are needed, in order");

function countsArguments(a, b = 2) { return arguments.length * 10 + b; }
check(countsArguments(), 2, "arguments.length of none");
check(countsArguments(1), 12, "arguments.length of one");
check(countsArguments(1, undefined), 22, "arguments.length with undefined");
check(countsArguments(1, 5, 6), 35, "arguments.length of three");
function withRest(a, b = 2, ...rest) { return a + b + rest.length; }
check(withRest(1), 3, "a rest parameter, nothing for it");
check(withRest(1, undefined, 1, 1), 5, "a rest parameter");

function four(a, b, c, d = 4) { return a + b + c + d; }
function five(a, b, c, d, e = 5) { return a + b + c + d + e; }
function eight(a, b, c, d, e, f, g, h = 8) { return a + b + c + d + e + f + g + h; }
function nine(a, b, c, d, e, f, g, h, i = 9) { return a + b + c + d + e + f + g + h + i; }
function allOptional(a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8, i = 9, j = 10) { return a + b + c + d + e + f + g + h + i + j; }
check(four(1, 2, 3), 10, "the fourth");
check(four(1, 2, 3, 0), 6, "the fourth, passed");
check(five(1, 2, 3, 4), 15, "the fifth");
check(five(1, 2, 3, 4, 0), 10, "the fifth, passed");
check(eight(1, 2, 3, 4, 5, 6, 7), 36, "the eighth");
check(eight(1, 2, 3, 4, 5, 6, 7, 0), 28, "the eighth, passed");
check(nine(1, 2, 3, 4, 5, 6, 7, 8), 45, "the ninth");
check(nine(1, 2, 3, 4, 5, 6, 7, 8, 0), 36, "the ninth, passed");
check(allOptional(), 55, "ten, none passed");
check(allOptional(0, undefined, 0, undefined, 0, undefined, 0, undefined, 0, undefined), 30, "ten, every other one passed");

check(scaled.call(null, 3), 6, "call");
check(scaled.call(null, 3, 4), 12, "call, passed");
check(scaled.apply(null, [3]), 6, "apply");
check(scaled.apply(null, [3, 4]), 12, "apply, passed");
check(scaled.bind(null, 3)(), 6, "bind");
check(scaled.bind(null, 3, 4)(), 12, "bind, passed");
check(Reflect.apply(scaled, null, [3]), 6, "Reflect.apply");
check(scaled(...[3]), 6, "spread");
check(scaled(...[3, 4]), 12, "spread, passed");
check([1, 2, 3].map(function (value, index = 10, array = null) { return value + index + array.length; }).join(), "4,6,8", "called by a builtin that passes them");
check([1, 2, 3].map(Number).map(function (value, index, array, more = 7) { return value + more; }).join(), "8,9,10", "called by a builtin that does not");
check([3, 1, 2].sort(function (a, b, c = 0) { return a - b + c; }).join(), "1,2,3", "called by sort");

let kept = null;
function keeps(a, object = null) { kept = object; return a; }
function passesLiteralThatIsKept(v) { return keeps(1, { v }); }
passesLiteralThatIsKept(41);
check(kept.v, 41, "an object that an optional parameter lets out");
passesLiteralThatIsKept(42);
check(kept.v, 42, "another");
function reads(a, object = { v: 5 }) { return a + object.v; }
function passesLiteralThatIsRead(v) { return reads(1, { v }); }
check(passesLiteralThatIsRead(2), 3, "an object that an optional parameter only reads");
check(reads(1), 6, "the object that is its default");
function returnsDefault(options = { }) { return options; }
check(returnsDefault() === returnsDefault(), false, "a new object each time");

let sumTo = (function () {
    "use strict";
    return function sumTo(n, sum = 0) { return n ? sumTo(n - 1, sum + n) : sum; };
})();
check(sumTo(100), 5050, "a tail call that passes it");
check(sumTo(20000), 200010000, "many tail calls");

function Made(a, b = 2) { this.sum = a + b; }
class Class {
    constructor(a, b = 2) { this.sum = a + b; }
    method(a, b = this.sum) { return a + b; }
    static make(a, b = 3) { return new Class(a, b); }
}
check(new Made(1).sum, 3, "a constructor");
check(new Made(1, 5).sum, 6, "a constructor, passed");
check(new Class(1).sum, 3, "a class");
check(new Class(1, 5).sum, 6, "a class, passed");
check(new Class(1).method(1), 4, "a method whose default reads this");
check(Class.make(1).sum, 4, "a static method");
check(Reflect.construct(Class, [1]).sum, 3, "Reflect.construct");

let arrow = (a, b = 2) => a * b;
check(arrow(3), 6, "an arrow function");
check(arrow(3, 4), 12, "an arrow function, passed");
function destructures({ a = 1, b = 2 } = { }, [c = 3] = []) { return a + b + c; }
check(destructures(), 6, "patterns with defaults");
check(destructures({ a: 10 }, [20]), 32, "patterns with defaults, passed");

function hasOwnScope(a, b = () => a) { var a = 2; return b() + a; }
check(hasOwnScope(1), 3, "a default that closes over a parameter");
function capturesIt(a, b = 2) { return () => a * b; }
check(capturesIt(3)(), 6, "a closure over it");
check(capturesIt(3, 4)(), 12, "a closure over it, passed");
function assignsIt(a, b = 2) { b += a; return b; }
check(assignsIt(1), 3, "assigned to");

function catches(a, b = 2) {
    try {
        if (a > 1)
            throw new Error("thrown");
        return a + b;
    } catch {
        return b;
    } finally {
        order.length = 0;
    }
}
check(catches(1), 3, "in a function that catches");
check(catches(2), 2, "after a throw");
check(catches(2, 9), 9, "after a throw, passed");
function throwsInDefault(a, b = (() => { throw new RangeError("from the default"); })()) { return a + b; }
check(throwsInDefault(1, 2), 3, "a default that would throw");
let thrown = null;
try { throwsInDefault(1); } catch (error) { thrown = error; }
check(thrown instanceof RangeError, true, "a default that throws");

function* generates(a, b = 2) { yield a; yield b; }
check([...generates(1)].join(), "1,2", "a generator");
check([...generates(1, 5)].join(), "1,5", "a generator, passed");
let settled = [];
async function waits(a, b = 2) { await null; return a + b; }
waits(1).then(value => settled.push(value));
waits(1, 5).then(value => settled.push(value));
drainMicrotasks();
check(settled.join(), "3,6", "an async function");

function returnsEarly(a, b) { if (b === undefined) return -1; return a + b; }
function callsReturnsEarly(n) {
    let sum = 0;
    for (let i = 0; i < n; i++)
        sum += returnsEarly(i) + returnsEarly(i, 1);
    return sum;
}
check(callsReturnsEarly(4), 6, "a comparison that decides which return is reached");

if (aotRemarks("scaled")) {
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
    applies("takes-argument-list", "countsArguments", "withRest", "nine", "allOptional");
    doesNotApply("takes-argument-list", "scaled", "large", "inOrder", "four", "keeps", "reads", "sumTo", "Made", "method", "make", "catches", "capturesIt");
    applies("direct-call:large", "sumsLarge", "sumsLargeWith");
    applies("inlined-call:scaled", "sumsScaled", "sumsScaledWithConstant");
    applies("folded-comparison-of-argument", "sumsScaled", "sumsScaledWithConstant");
    doesNotApply("folded-comparison-of-argument", "sumsLarge", "callsScaledWith");
    doesNotApply("scalar-replaced-object", "passesLiteralThatIsKept");
}
})();
