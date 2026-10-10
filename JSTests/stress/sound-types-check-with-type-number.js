//@ requireOptions("--useSoundTypes=1")
//@ runDefault
//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useTypeTags=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function outcomeOf(f) {
    try {
        return "returns " + String(f());
    } catch (error) {
        return error.constructor.name;
    }
}
const object = { a: 1 };
function numberWithType(v) { return $$t(v, 8, 5); }
function numberWithTypeZero(v) { return $$t(v, 8, 0); }
function numberWithLargestType(v) { return $$t(v, 8, 4294967295); }
function objectWithType(v) { return $$t(v, 512, 7); }
function objectOrUndefinedWithType(v) { return $$t(v, 513, 7); }
function inConstructor(v) {
    class C { n; constructor(n) { this.n = $$t(n, 8, 5); } }
    return new C(v).n;
}

for (let i = 0; i < 200; i++) {
    check(numberWithType(i), i, "a number that is checked with a type number");
    check(numberWithTypeZero(i + 0.5), i + 0.5, "a number that is checked with the type number 0");
    check(numberWithLargestType(i), i, "a number that is checked with the largest type number");
    check(objectWithType(object), object, "an object that is checked with a type number");
    check(objectOrUndefinedWithType(undefined), undefined, "undefined where the mask admits it");
    check(objectOrUndefinedWithType(object), object, "an object where the mask admits undefined too");
    check(inConstructor(i), i, "a check with a type number in a constructor");
    check(outcomeOf(() => numberWithType("text")), "TypeError", "a string where a number is expected");
    check(outcomeOf(() => numberWithTypeZero(undefined)), "TypeError", "undefined where a number is expected");
    check(outcomeOf(() => objectWithType(undefined)), "TypeError", "undefined where the mask has only objects");
    check(outcomeOf(() => objectWithType(1)), "TypeError", "a number where an object is expected");
    check(outcomeOf(() => inConstructor("text")), "TypeError", "a string in a constructor that expects a number");
}

check(outcomeOf(() => $$t(1, 8, 5, 6)), "ReferenceError", "four arguments are an ordinary call");
check(outcomeOf(() => $$t(1, 8, 5.5)), "ReferenceError", "a type number that is no integer makes an ordinary call");
check(outcomeOf(() => $$t(1, 8, -1)), "ReferenceError", "a negative type number makes an ordinary call");
check(outcomeOf(() => $$t(1, 8, 4294967296)), "ReferenceError", "a type number that is too large makes an ordinary call");
check(outcomeOf(() => $$t(1, 8, "5")), "ReferenceError", "a type number that is a string makes an ordinary call");
check(outcomeOf(() => $$t(1, 8, object.a)), "ReferenceError", "a type number that is no literal makes an ordinary call");
check(outcomeOf(() => $$t(1, 0, 5)), "ReferenceError", "a mask of 0 makes an ordinary call whatever the type number is");
