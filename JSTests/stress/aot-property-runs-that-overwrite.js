//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useDollarVM=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1", "--useAOTInlining=0", "--verifyGC=1", "--scribbleFreeCells=1", "--useZombieMode=1", "--slowPathAllocsBetweenGCs=10")

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function describe(o) { return Object.keys(o).map(key => key + "=" + o[key]).join(); }
function remarksOf(f) { return typeof aotRemarks === "function" && isAOTCompiled(f) ? aotRemarks(f.name) : null; }
function applies(f, pattern) {
    let remarks = remarksOf(f);
    if (remarks && !remarks.includes(pattern))
        throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
}
const counts = typeof aotOperationCount === "function" && aotOperationCount("operationAOTPutProperties") !== null && isAOTCompiled(overwrites);
function arrivals() { return counts ? aotOperationCount("operationAOTPutProperties") : 0; }

function overwrites(o, x, y) { o.a = x; o.b = y; o.c = x; o.d = y; return o; }
function overwritesStrictly(o, x, y) { "use strict"; o.a = x; o.b = y; o.c = x; o.d = y; return o; }
function adds(o, x, y) { o.a = x; o.b = y; o.c = x; o.d = y; return o; }
function overwritesResultOfCall(x, y) { let o = makes(); o.a = x; o.b = y; o.c = x; o.d = y; return o; }
function makes() { return { a: 0, b: 0, c: 0, d: 0 }; }
function overwritesField(holder, x, y) { let o = holder.inner; o.a = x; o.b = y; o.c = x; o.d = y; return o; }
function runsTwice(o, x, y) { o.a = x; o.b = y; o.c = x; o.d = y; return o; }
function twoRuns(o, p, x, y) { o.a = x; o.b = x; o.c = x; o.d = x; p.a = y; p.b = y; p.c = y; p.d = y; return o; }
for (let f of [overwrites, overwritesStrictly, adds, overwritesResultOfCall, makes, overwritesField, runsTwice, twoRuns])
    noInline(f);

const calls = 20000;
const bound = 200;
function arrivesSeldom(what, call) {
    let before = arrivals();
    for (let i = 0; i < calls; ++i)
        call(i);
    let arrived = arrivals() - before;
    if (counts && arrived > bound)
        throw new Error(what + ": " + arrived + " arrivals in " + calls + " calls, not at most " + bound);
}

let target = { a: 0, b: 0, c: 0, d: 0 };
arrivesSeldom("stores that overwrite", i => check(describe(overwrites(target, i, -i)), "a=" + i + ",b=" + -i + ",c=" + i + ",d=" + -i, "stores that overwrite"));
arrivesSeldom("stores that overwrite, in strict code", i => check(overwritesStrictly(target, i, i + 3).d, i + 3, "stores that overwrite, in strict code"));
arrivesSeldom("stores to the result of a call", i => check(describe(overwritesResultOfCall(i, "y")), "a=" + i + ",b=y,c=" + i + ",d=y", "stores to the result of a call"));
let holder = { inner: { d: 0, c: 0, b: 0, a: 0 } };
arrivesSeldom("stores to the value of a field", i => check(describe(overwritesField(holder, i, "y")), "d=y,c=" + i + ",b=y,a=" + i, "stores to the value of a field"));

let structuresBehind = o => $vm.getStructureTransitionList(o).length / 5;
{
    let before = arrivals();
    for (let i = 0; i < calls; ++i) {
        let made = adds({}, i, "y");
        if (!(i % 1000)) {
            check(describe(made), "a=" + i + ",b=y,c=" + i + ",d=y", "stores that add");
            if (isAOTCompiled(adds))
                check(structuresBehind(made), 1, "stores that add make one structure");
        }
    }
    if (counts)
        check(arrivals() - before, calls, "stores that add arrive each time");
    check(describe(adds({ z: 1 }, 5, 6)), "z=1,a=5,b=6,c=5,d=6", "stores that add to another shape");
}

for (let round = 0; round < 2; ++round)
    check(describe(runsTwice({ a: 0, b: 0, c: 0, d: 0 }, round, "y")), "a=" + round + ",b=y,c=" + round + ",d=y", "a function that runs twice");

{
    let fresh = 0;
    arrivesSeldom("the second of two runs overwrites, the first adds", i => { if (i < bound / 4) { twoRuns({}, target, i, i); ++fresh; } else twoRuns(target, target, i, i); });
    check(describe(twoRuns({}, target, 7, 8)) + "|" + describe(target), "a=7,b=7,c=7,d=7|a=8,b=8,c=8,d=8", "two runs after both have switched");
}

function afterTheSwitch(f, isStrict) {
    let log = [];
    check(describe(f({}, 1, 2)), "a=1,b=2,c=1,d=2", "an empty object");
    check(describe(f({ c: 0 }, 1, 2)), "c=1,a=1,b=2,d=2", "one of the names is there");
    check(describe(f(Object.create(null), 1, 2)), "a=1,b=2,c=1,d=2", "no prototype");
    check(describe(f(Object.create({ set b(v) { log.push("b" + v); }, set d(v) { log.push("d" + v); } }), 1, 2)) + log.join(), "a=1,c=1b2,d2", "setters on the prototype");
    log = [];
    f(new Proxy({}, { set(o, key, value) { log.push(key + value); return true; } }), 1, 2);
    check(log.join(), "a1,b2,c1,d2", "a proxy sees every store in order");
    let thrown = null, partly = Object.preventExtensions({ a: 0, b: 0 });
    try { f(partly, 1, 2); } catch (e) { thrown = e; }
    check(thrown instanceof TypeError, isStrict, "an object that cannot be extended");
    check(describe(partly), "a=1,b=2", "what was stored before");
    thrown = null;
    let halfWay = { set c(v) { throw new RangeError("c"); } };
    try { f(halfWay, 1, 2); } catch (e) { thrown = e; }
    check(thrown instanceof RangeError, true, "a setter that throws");
    check(Object.keys(halfWay).join(), "c,a,b", "the stores before it happened, the one after it did not");
    thrown = null;
    try { f(null, 1, 2); } catch (e) { thrown = e; }
    check(thrown instanceof TypeError, true, "null");
    check(f(5, 1, 2), 5, "a number");
    let array = f([1, 2], 1, 2);
    check(array.length + describe(array), "20=1,1=2,a=1,b=2,c=1,d=2", "an array");
    let dictionary = {};
    for (let i = 0; i < 300; ++i)
        dictionary["p" + i] = i;
    check(f(dictionary, 1, 2).d + Object.keys(dictionary).length, 2 + 304, "an object with many properties");
}
afterTheSwitch(overwrites, false);
afterTheSwitch((o, x, y) => { try { return overwritesStrictly(o, x, y); } catch (e) { if (typeof o === "number") return o; throw e; } }, true);
gc();
afterTheSwitch(overwrites, false);
arrivesSeldom("after other shapes and a collection", i => overwrites(target, i, i));

for (let f of [overwrites, overwritesStrictly, adds, overwritesResultOfCall, overwritesField, runsTwice, twoRuns])
    applies(f, "property-run:4");
