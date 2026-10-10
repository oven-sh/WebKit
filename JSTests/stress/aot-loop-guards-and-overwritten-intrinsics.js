//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault

function shouldBe(actual, expected, what)
{
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}

function shouldThrow(run, constructor, what)
{
    let error = null;
    try {
        run();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof constructor))
        throw new Error(what + ": " + String(error) + " instead of a " + constructor.name);
}

const isImmutable = !Object.getOwnPropertyDescriptor(Math, "sqrt").writable;
for (const [holder, name] of [[Math, "floor"], [String.prototype, "charCodeAt"], [Array.prototype, "push"]])
    shouldBe(!Object.getOwnPropertyDescriptor(holder, name).writable, isImmutable, name + " is as sqrt is");
shouldBe(!Object.getOwnPropertyDescriptor(globalThis, "Math").configurable, isImmutable, "and so is the binding of Math");

function math(values)
{
    let s = 0;
    for (let i = 0; i < values.length; i++) {
        const v = values[i];
        s += Math.sqrt(v) + Math.abs(v) + Math.floor(v) + Math.ceil(v) + Math.trunc(v) + Math.fround(v) + Math.min(v, 2) + Math.max(v, 3) + Math.imul(v, 3);
    }
    return s;
}
const numbers = [0, 1, 2.5, 4, 9.75, 100];
const expected = math(numbers);
for (let i = 0; i < 100; i++)
    shouldBe(math(numbers), expected, "before anything is overwritten");
const withoutSqrt = 4 + 4 + 4 + 4 + 4 + 2 + 4 + 12;
shouldBe(math([4]), 2 + withoutSqrt, "before anything is overwritten");

const sqrt = Math.sqrt;
Math.sqrt = function (v) { return 1000; };
shouldBe(Math.sqrt === sqrt, isImmutable, "a store to Math.sqrt");
shouldBe(math([4]), (isImmutable ? 2 : 1000) + withoutSqrt, "Math.sqrt is another function");
Math.sqrt = Math.abs;
shouldBe(math([4]), (isImmutable ? 2 : 4) + withoutSqrt, "Math.sqrt is another intrinsic");
Math.sqrt = sqrt;
shouldBe(math(numbers), expected, "Math.sqrt is itself again");

function swapsHalfway(n)
{
    let s = 0;
    for (let i = 0; i < n; i++) {
        if (i === n >> 1)
            Math.floor = v => -1;
        s += Math.floor(i + 0.5);
    }
    return s;
}
const floor = Math.floor;
shouldBe(swapsHalfway(10), isImmutable ? 45 : 0 + 1 + 2 + 3 + 4 - 5, "Math.floor is overwritten in the middle of a loop");
Math.floor = floor;
shouldBe(Math.floor(2.5), 2, "Math.floor is itself again");

function codes(s, from, to) { let t = 0; for (let i = from; i < to; i++) t += s.charCodeAt(i); return t; }
for (let i = 0; i < 100; i++)
    shouldBe(codes("abc", 0, 3), 294, "before charCodeAt is overwritten");
const charCodeAt = String.prototype.charCodeAt;
String.prototype.charCodeAt = function () { return 1; };
shouldBe(codes("abc", 0, 3), isImmutable ? 294 : 3, "charCodeAt is another function");
String.prototype.charCodeAt = charCodeAt;
shouldBe(codes("abc", 0, 3), 294, "charCodeAt is itself again");

function pushes(a, n, v) { let last = 0; for (let i = 0; i < n; i++) last = a.push(v); return last + ":" + a.length + ":" + a[a.length - 1]; }
for (let i = 0; i < 100; i++)
    shouldBe(pushes([], 2, 1), "2:2:1", "before push is overwritten");
const push = Array.prototype.push;
Array.prototype.push = function () { return -1; };
shouldBe(pushes([], 2, 1), isImmutable ? "2:2:1" : "-1:0:undefined", "push is another function");
Array.prototype.push = push;
shouldBe(pushes([], 2, 1), "2:2:1", "push is itself again");

function overwritesInStrictCode() { "use strict"; Math.sqrt = sqrt; return true; }
function definesAgain() { Object.defineProperty(Math, "sqrt", { value: Math.abs }); return true; }
function removes() { "use strict"; return delete Array.prototype.push; }
if (isImmutable) {
    shouldThrow(overwritesInStrictCode, TypeError, "a store to Math.sqrt in strict code");
    shouldThrow(definesAgain, TypeError, "Math.sqrt is defined again");
    shouldThrow(removes, TypeError, "push is removed");
    shouldBe(math(numbers), expected, "nothing has changed");
    shouldBe(pushes([], 2, 1), "2:2:1", "nothing has changed");
} else
    shouldBe(overwritesInStrictCode(), true, "a store to Math.sqrt in strict code");

function usesGlobal(n)
{
    let s = 0;
    for (let i = 0; i < n; i++)
        s += Math.abs(-i);
    return s;
}
shouldBe(usesGlobal(5), 10, "before Math is shadowed");
shouldBe(usesGlobal(5), 10, "before Math is shadowed");
if (typeof $262 !== "undefined" && $262.evalScript) {
    const shadows = () => $262.evalScript("let Math = { abs() { return 1; } };");
    if (isImmutable) {
        shouldThrow(shadows, SyntaxError, "a script that shadows Math");
        shouldBe(usesGlobal(5), 10, "Math is not shadowed");
    } else {
        shadows();
        shouldBe(usesGlobal(5), 5, "Math is shadowed");
    }
}
