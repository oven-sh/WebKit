//@ runDefault("-m", "--compileMainScriptAheadOfTime=1")
//@ runDefault("-m", "--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("-m", "--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
//@ runDefault("-m", "--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1")
//@ runDefault("-m")

function check(actual, expected, what)
{
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(Object.is(actual, -0) ? "-0" : actual) + " instead of " + String(Object.is(expected, -0) ? "-0" : expected));
}

function remarksOf(name)
{
    return typeof aotRemarks === "function" ? aotRemarks(name) : null;
}

function has(name, remark)
{
    const remarks = remarksOf(name);
    if (remarks && !remarks.includes(remark))
        throw new Error(name + " lacks " + remark);
}

function hasNot(name, remark)
{
    const remarks = remarksOf(name);
    if (remarks && remarks.includes(remark))
        throw new Error(name + " has " + remark);
}

function errorOf(run)
{
    try {
        run();
    } catch (error) {
        return error.constructor;
    }
    return null;
}
function test()
{
    Number.prototype.addedByTheProgram = function () { return "added to " + this; };
    String.prototype.addedByTheProgram = function () { return "added to " + this; };

    function fixed(x) { return x.toFixed(2); }
    function fixedOfField(item) { return item.price.toFixed(1); }
    function text(x) { return x.toString(); }
    function radix(x) { return x.toString(16); }
    function upper(s) { return s.toUpperCase(); }
    function code(s) { return s.charCodeAt(0); }
    function textOfBoolean(b) { return b.toString(); }
    function textOfSymbol(s) { return s.toString(); }
    function textOfBigInt(n) { return n.toString(); }
    function fixedOrNothing(x) { return x.toFixed(0); }
    function fixedOfAnything(x) { return x.toFixed(1); }
    function precisionOfAnything(x) { return x.toPrecision(2); }
    function textOfAnything(x) { return x.toString(); }
    function method(x) { return x.toFixed; }
    function methodOfString(s) { return s.indexOf; }
    function added(x) { return x.addedByTheProgram(); }
    function addedToString(s) { return s.addedByTheProgram(); }
    function absent(x) { return x.notThere; }
    function lengthOfString(s) { return s.length; }
    function constructorOfNumber(x) { return x.constructor; }
    function borrowed(x, y) { var f = x.toFixed; return f.call(y, 1); }

    const anything = [1.25, 3, { toFixed() { return "mine"; }, toPrecision() { return "mine"; }, toString() { return "mine"; } }, new Number(2.5)];
    for (let i = 0; i < 100; ++i) {
        check(fixed(+(i + 0.125)), (i + 0.125).toFixed(2), "toFixed of a number");
        check(fixed(i | 0), i + ".00", "toFixed of an integer");
        check(fixed(NaN), "NaN", "toFixed of NaN");
        check(fixed(-0), "0.00", "toFixed of -0");
        check(fixedOfField({ price: i + 0.5 }), i + ".5", "toFixed of a field");
        check(text(+i), String(i), "toString of a number");
        check(radix(255), "ff", "toString with a radix");
        check(upper("ab" + i), "AB" + i, "toUpperCase");
        check(code("A"), 65, "charCodeAt");
        check(textOfBoolean(i % 2 == 0), i % 2 == 0 ? "true" : "false", "toString of a boolean");
        check(textOfSymbol(Symbol("s")), "Symbol(s)", "toString of a symbol");
        check(textOfBigInt(10n), "10", "toString of a BigInt");
        check(fixedOrNothing(i & 1 ? 2.5 : 7), i & 1 ? "3" : "7", "toFixed of a number or nothing");
        check(fixedOfAnything(anything[i & 3]), ["1.3", "3.0", "mine", "2.5"][i & 3], "toFixed of anything");
        check(precisionOfAnything(anything[i & 3]), ["1.3", "3.0", "mine", "2.5"][i & 3], "toPrecision of anything");
        check(textOfAnything(anything[i & 3]), ["1.25", "3", "mine", "2.5"][i & 3], "toString of anything");
        check(method(1.5), Number.prototype.toFixed, "the method itself");
        check(methodOfString("a"), String.prototype.indexOf, "the method itself");
        check(added(1.5), "added to 1.5", "a method that the program added");
        check(addedToString("s"), "added to s", "a method that the program added");
        check(absent(1.5), undefined, "a name that is not there");
        check(lengthOfString("abc"), 3, "the length of a string");
        check(constructorOfNumber(1.5), Number, "the constructor");
        check(borrowed(1.5, 2.25), "2.3", "a method called on another number");
    }
    check(errorOf(() => fixedOrNothing(undefined)), TypeError, "toFixed of undefined");
    check(errorOf(() => fixedOrNothing(null)), TypeError, "toFixed of null");
    check(errorOf(() => fixedOfAnything("s")), TypeError, "toFixed of a string");
    check(errorOf(() => fixedOfAnything(undefined)), TypeError, "toFixed of undefined");
    check(errorOf(() => fixed(1.5).toFixed()), TypeError, "toFixed of its result");
    check(errorOf(() => borrowed(1.5, "s")), TypeError, "a method called on a string");

    has("fixed", "reads-method-of-primitive:toFixed");
    has("fixed", "direct-host-call");
    hasNot("fixed", "calls:GetById");
    hasNot("fixed", "cached-call");
    has("text", "lowered-builtin:Number.prototype.toString");
    has("radix", "lowered-builtin:Number.prototype.toString");
    has("upper", "lowered-builtin:String.prototype.toUpperCase");
    for (const name of ["text", "radix", "upper", "textOfBoolean", "textOfSymbol", "method", "methodOfString", "constructorOfNumber", "borrowed"]) {
        hasNot(name, "calls:GetById");
        hasNot(name, "cached-call");
    }
    has("textOfBoolean", "direct-host-call");
    has("textOfSymbol", "direct-host-call");
    has("borrowed", "reads-method-of-primitive:toFixed");
    has("borrowed", "direct-host-call");
    has("constructorOfNumber", "reads-method-of-primitive:constructor");
    has("textOfBoolean", "reads-method-of-primitive:toString");
    has("textOfSymbol", "reads-method-of-primitive:toString");
    has("fixedOrNothing", "reads-method-of-primitive:toFixed");
    has("method", "reads-method-of-primitive:toFixed");
    has("methodOfString", "reads-method-of-primitive:indexOf");
    has("fixedOfAnything", "reads-method-of-number-behind-test:toFixed");
    has("precisionOfAnything", "reads-method-of-number-behind-test:toPrecision");
    has("fixedOfField", "reads-method-of-number-behind-test:toFixed");
    for (const name of ["textOfAnything", "textOfBigInt", "added", "addedToString", "absent", "lengthOfString"]) {
        for (const remark of remarksOf(name) || []) {
            if (remark.startsWith("reads-method-of-"))
                throw new Error(name + " has " + remark);
        }
    }
}
test();
