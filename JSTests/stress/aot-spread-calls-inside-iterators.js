//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
function take(...all) { return all.length; }
function sum(...all) { let total = 0; for (let x of all) total += x; return total; }

const inner = { *[Symbol.iterator]() { yield 1; yield 2; } };
const outer = { *[Symbol.iterator]() { yield take(...inner); yield take(0, ...inner, ...inner); } };
check(sum(...outer), 7, "a spread call in the iterator of a spread call");
check(sum(...outer, ...outer), 14, "twice in one call");
check(new Array(...outer).join(), "2,5", "in a construction");
const constructs = { *[Symbol.iterator]() { yield new Array(...inner).length; } };
check(sum(...constructs), 2, "a construction in the iterator of a spread call");
check(Math.max(...(function* () { yield Math.max(...new Set([1, 5, 3])); yield Math.min(...new Set([9, 8])); })()), 8, "built-in functions and sets");

function nest(depth) { return depth ? sum(1, ...(function* () { yield nest(depth - 1); })()) : 0; }
check(nest(3), 3, "three levels");
check(nest(60), 60, "sixty levels, all at one call site");

function* throws() { yield 1; throw new RangeError("from the iterator"); }
function* catches() { try { take(...throws()); } catch (error) { yield error.constructor.name.length; } yield take(...inner); }
check(sum(...catches()), 12, "the inner spread throws, the outer one goes on");
let caught = "nothing";
try { take(...(function* () { yield take(...throws()); })()); } catch (error) { caught = error.message; }
check(caught, "from the iterator", "the inner spread throws through the outer one");
check(sum(...outer), 7, "afterwards");

function positionsOf(stack, name) { return stack.split("\n").filter(line => line.startsWith(name + "@")).map(line => line.split(":").slice(-2).join(":")).join(" "); }
function capture(...all) { return new Error("x").stack; }
function run(items) {
    return capture(
        0,
        ...items);
}
let seen;
function* captures() { seen = new Error("x").stack; yield 1; }
const atCall = positionsOf(run([1, 2]), "run");
run(captures());
const atSpread = positionsOf(seen, "run");
check(atCall.split(" ").length + atSpread.split(" ").length, 2, "one frame each");
check(atCall === atSpread, false, "the call and the spread have positions of their own");

function* callsAgain() { seen = run([1, 2]); yield 1; }
run(callsAgain());
check(positionsOf(seen, "run"), atCall + " " + atSpread, "the inner activation is in the call, the outer one in its spread");
function* spreadsAgain() { run(captures()); yield 1; }
run(spreadsAgain());
check(positionsOf(seen, "run"), atSpread + " " + atSpread, "both activations are in their spreads");
function* spreadsThenCaptures() { run(captures()); seen = new Error("x").stack; yield 1; }
run(spreadsThenCaptures());
check(positionsOf(seen, "run"), atSpread, "the outer activation is still in its spread when the inner one is over");
check(positionsOf(run([1, 2]), "run"), atCall, "nothing is left behind");
