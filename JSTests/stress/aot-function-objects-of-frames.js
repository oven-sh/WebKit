//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "-m")
function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
function namesIn(stack) { return stack.split("\n").map(line => line.split("@")[0]).join(); }
const top = namesIn(new Error("x").stack);

const Mixin = (Base) => class extends Base { constructor(...args) { super(...args); } };
const Once = Mixin(Error), Twice = Mixin(Once), Thrice = Mixin(Twice);
check(namesIn(new Once().stack), top, "a class made by a mixin");
check(namesIn(new Twice().stack), top, "the same mixin applied twice: both constructors are hidden");
check(namesIn(new Thrice().stack), top, "and three times");

class Nested extends Error {
    constructor(depth) {
        super();
        if (depth)
            this.inner = new Nested(depth - 1);
    }
}
check(namesIn(new Nested(1).inner.stack), "Nested," + top, "a constructor that constructs its own class: only the inner frame is hidden");

function makeStep() {
    return function step(next, upTo) {
        try {
            if (next)
                return next(null, upTo);
            let holder = {};
            Error.captureStackTrace(holder, upTo);
            return namesIn(holder.stack);
        } catch (error) {
            return String(error);
        }
    };
}
const outer = makeStep(), inner = makeStep();
check(outer(inner, inner), "step," + top, "two closures of one function: up to the inner one");
check(outer(inner, outer), top, "two closures of one function: up to the outer one");
check(outer(inner, makeStep()), "", "a closure of that function that is not running");

function makeNamed() { return function named() { try { return new Error("x").stack; } catch { } }; }
const named = makeNamed(), plain = makeNamed();
check(namesIn(named()), "named," + top, "no displayName yet");
named.displayName = "shown";
check(namesIn(named()), "shown," + top, "displayName");
check(namesIn(plain()), "named," + top, "another closure of the same function has none");
named.displayName = 1;
check(namesIn(named()), "named," + top, "a displayName that is not a string");
Object.defineProperty(named, "displayName", { get() { return "computed"; }, configurable: true });
check(namesIn(named()), "named," + top, "a displayName that is an accessor");
delete named.displayName;
check(namesIn(named()), "named," + top, "displayName was deleted");
let early = named();
named.displayName = "late";
check(namesIn(early), "named," + top, "the text of a trace that was read does not change");

function wrap(name) {
    let wrapper = function () { try { return new Error(name); } catch { } };
    wrapper.displayName = "wrapped(" + name + ")";
    return wrapper;
}
const first = wrap("first"), second = wrap("second");
check(namesIn(first().stack), "wrapped(first)," + top, "each closure has its own displayName");
check(namesIn(second().stack), "wrapped(second)," + top, "each closure has its own displayName");

class Shown { constructor() { try { this.stack = new Error("x").stack; } catch { } } }
Shown.displayName = "ShownAs";
check(namesIn(new Shown().stack), "ShownAs," + top, "displayName of a class");

let kept = [first(), second(), new Twice()];
gc();
fullGC();
check(namesIn(kept[0].stack), "wrapped(first)," + top, "a trace that the collector made text of");
check(namesIn(kept[1].stack), "wrapped(second)," + top, "a trace that the collector made text of");
check(namesIn(kept[2].stack), top, "a trace that the collector made text of");

(function () {
    const Local = (Base) => class extends Base { constructor() { super(); } };
    const A = Local(Error), B = Local(A);
    check(namesIn(new B().stack), "," + top, "the mixin inside a function");
    const outerStep = makeStep(), innerStep = makeStep();
    check(outerStep(innerStep, outerStep), "," + top, "closures called from a function");
    const local = wrap("local");
    check(namesIn(local().stack), "wrapped(local),," + top, "displayName of a closure called from a function");
})();
