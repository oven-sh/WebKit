//@ runDefault("-m", "--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
function namesIn(stack) { return stack.split("\n").map(line => line.split("@")[0]).join(); }
function makesError() { return new Error("in a function"); }
function captures() { let holder = {}; Error.captureStackTrace(holder); return holder; }
class Thrower { get value() { throw new RangeError("in a getter"); } }

check(namesIn(new Error("at the top level").stack), "module code", "an error made by module code");
check(namesIn(makesError().stack), "makesError,module code", "an error made by a function that module code calls");
check(namesIn(captures().stack), "captures,module code", "a captured trace");
check(namesIn([1].map(() => new Error("in a callback"))[0].stack), ",map,module code", "an error made in a callback of a built-in function");

let caught;
try { null.property; } catch (error) { caught = error; }
check(namesIn(caught.stack), "module code", "an error thrown by an operation of module code");
try { new Thrower().value; } catch (error) { caught = error; }
check(namesIn(caught.stack), "value,module code", "an error thrown by a getter that module code reads");

let kept = [new Error("made text of by the collector"), makesError()];
gc();
fullGC();
check(namesIn(kept[0].stack), "module code", "a trace that the collector made text of");
check(namesIn(kept[1].stack), "makesError,module code", "the same below a function");

await 1;
check(namesIn(new Error("after an await").stack), "module code", "an error made by module code after an await");
check(namesIn(makesError().stack), "makesError,module code", "an error made by a function that module code calls after an await");
