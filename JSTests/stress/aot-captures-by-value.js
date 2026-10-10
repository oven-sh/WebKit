//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ run("few-function-numbers", "--maxAOTFunctionNumberInTypesForTesting=3", "--validateAOTInferredTypes=true")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(typeof f === "string" ? f : f.name);
    if (!remarks && typeof f !== "string" && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + (f.name || f) + ": " + remarks.join(" "));
    }
}
function doesNotApply(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + (f.name || f) + ": " + remarks.join(" "));
    }
}
const captures = "captures-by-value", reads = "reads-capture", makesEnvironment = "calls:operationAOTCreateLexicalEnvironment";
function identity(x) { return x; }
noInline(identity);
function fetchLike(url, options) { return url + ":" + options.signal; }
noInline(fetchLike);

function makesFetcher(signal) {
    const fetcher = (url) => fetchLike(url, { signal });
    return fetcher;
}
function capturesSeveral(a, b) {
    const sum = a + b, text = "t" + a;
    const several = (c) => a * 1000 + b * 100 + sum * 10 + c + text;
    return several;
}
function capturesObjects(n) {
    const kept = { v: n }, list = [n, n + 1];
    const readsObjects = () => kept.v + list[1];
    return readsObjects;
}
function twoClosures(a, b) {
    const first = () => a, second = () => a + b;
    return [first, second];
}
function inLoop(limit) {
    const made = [];
    for (let i = 0; i < limit; i++) {
        const square = i * i;
        const ofIteration = () => square + 1;
        made.push(ofIteration);
    }
    return made;
}
class Holder {
    constructor(v) { this.v = v; }
    capturesThis(x) {
        const readsThis = () => this.v + x;
        return readsThis;
    }
}
function ordinaryFunction(base) {
    const ordinary = function (x) { return base + x + (this === undefined ? 0 : this.extra || 0); };
    return ordinary;
}
function underScopeThatStays(start) {
    let counter = start;
    const bump = () => ++counter;
    function inner(fixed) {
        const readsBoth = () => fixed * 100 + counter;
        return readsBoth;
    }
    return [bump, inner(7)];
}
function parentOfScopeThatStays() {
    const kept = [];
    function keeps(f) { kept[0] = f; return 1; }
    function makesAndKeeps(signal) { return keeps((url) => url + signal); }
    return [makesAndKeeps, kept];
}
function callsWhatItCaptures(n) {
    const seesThis = function () { "use strict"; return this === undefined ? 1 : -1; };
    const callsIt = () => seesThis() + n;
    identity(callsIt);
    return seesThis() * 100 + callsIt();
}
function tailCallsWhatItCaptures(n) {
    "use strict";
    const target = (x) => x + n;
    const keeps = () => target(1);
    identity(keeps);
    identity(target);
    return target(2);
}
function sloppyCalleeSeesGlobal() {
    const seesGlobal = new Function("return this === globalThis");
    const alsoCalls = () => seesGlobal();
    identity(alsoCalls);
    return seesGlobal() && alsoCalls();
}
function storedBeforeOnly(flag) {
    let chosen;
    if (flag)
        chosen = "yes";
    else
        chosen = "no";
    const readsChosen = () => chosen;
    return readsChosen;
}
for (const [maker, count, closure, ...names] of [[makesFetcher, 1, "fetcher", "signal"], [capturesSeveral, 4, "several", "a", "b", "sum", "text"], [capturesObjects, 2, "readsObjects", "kept", "list"], [storedBeforeOnly, 1, "readsChosen", "chosen"]]) {
    noInline(maker);
    applies(maker, captures + ":" + count);
    doesNotApply(maker, makesEnvironment);
    for (const name of names)
        applies(closure, reads + ":" + name);
}
for (const f of [callsWhatItCaptures, tailCallsWhatItCaptures, sloppyCalleeSeesGlobal]) {
    noInline(f);
    applies(f, captures);
    doesNotApply(f, makesEnvironment);
}
applies(twoClosures, captures + ":1", captures + ":2");
doesNotApply(twoClosures, makesEnvironment);
applies("makesAndKeeps", reads + ":keeps");
applies("readsBoth", reads + ":fixed");
doesNotApply("readsBoth", reads + ":counter");

function assignedLater() { let n = 0; const readsAssignedLater = () => n; n = 5; return readsAssignedLater; }
function assignedInClosure() { let n = 0; const bumps = () => ++n; return bumps; }
function assignedInOtherClosure() { let n = 0; const sets = (v) => { n = v; }, gets = () => n; return [sets, gets]; }
function madeBeforeInitialized() { const early = () => later; const later = 3; return early; }
function readTooEarly() { const tooEarly = () => notYet; let result; try { result = tooEarly(); } catch (error) { result = error.constructor.name; } const notYet = 1; return result + tooEarly(); }
function hoistedDeclaration() { return declared; function declared() { return afterwards; } var afterwards; }
function readTwoLevelsDown(x) { const outerLevel = () => { const innerLevel = () => x; return innerLevel; }; return outerLevel; }
function asyncClosure(x) { const awaits = async () => x; return awaits; }
function generatorClosure(x) { const yields = function* () { yield x; }; return yields; }
function seenByEval(x, text) { const evaluated = () => eval(text); return evaluated; }
function loopVariableThatChanges() { const made = []; for (let i = 0; i < 3; i++) made.push(() => i); return made; }
function readThreeLevelsDown(x, y) {
    const levelOne = () => { const levelTwo = (z) => { const levelThree = () => x * 100 + y * 10 + z; return levelThree; }; return levelTwo; };
    return levelOne;
}
function holderIsInlined(x, y) {
    const passesOn = () => { const inlinedReader = () => y + 1; const escapes = () => x * 100 + y; identity(escapes); return (typeof inlinedReader).length * 100000 + inlinedReader() * 1000 + escapes(); };
    identity(passesOn);
    return passesOn;
}
async function asyncWithParameters(url, options) {
    const first = await url;
    return first + options;
}
const asyncArrowWithParameters = async (a, b) => { await null; return a * 10 + b; };
class WithAsyncMethod {
    constructor(base) { this.base = base; }
    async method(x) { await null; return this.base + x; }
}
function* generatorWithParameters(a, b) { yield a; yield a + b; }
function catchesWithoutScope(signal) {
    const inTry = () => signal + 1;
    try {
        return identity(inTry)();
    } catch {
        return -1;
    }
}
function throwsIf(flag) { if (flag) throw new Error("thrown"); return 0; }
noInline(throwsIf);
function handlerReadsVariable(signal, fails) {
    const beforeTry = () => signal + 1;
    identity(beforeTry);
    try {
        throwsIf(fails);
        return beforeTry();
    } catch {
        return signal + 100;
    }
}
function handlerMakesClosure(signal, fails) {
    const outside = () => signal + 1;
    identity(outside);
    try {
        throwsIf(fails);
        return outside();
    } catch (error) {
        const inHandler = () => signal + error.message;
        return identity(inHandler)();
    }
}
function finallyRuns(signal, fails) {
    const log = [];
    const notes = (x) => { log.push(x + signal); };
    identity(notes);
    try {
        notes("try");
        throwsIf(fails);
    } catch {
        notes("catch");
    } finally {
        notes("finally");
    }
    return log.join();
}
function appliesTimes(f, pattern, times) {
    const remarks = remarksOf(f);
    if (remarks && remarks.filter(remark => matches(remark, pattern)).length !== times)
        throw new Error(pattern + " does not apply " + times + " times to " + f.name + ": " + remarks.join(" "));
}
for (const [f, frames] of [[asyncWithParameters, 1], [asyncArrowWithParameters, 0], [generatorWithParameters, 1]]) {
    noInline(f);
    applies(f, captures);
    appliesTimes(f, makesEnvironment, frames);
}
noInline(catchesWithoutScope);
applies(catchesWithoutScope, captures);
doesNotApply(catchesWithoutScope, makesEnvironment);
for (const f of [handlerReadsVariable, handlerMakesClosure, finallyRuns])
    noInline(f);
const keptClosures = [];
function makesWalker() {
    function mapLike(array, f) { const result = []; for (let i = 0; i < array.length; i++) result.push(f(array[i], i)); return result; }
    let outermost = "first";
    function setOutermost(v) { outermost = v; }
    function hasDissolvedScope(copied, list, alsoCopied = copied) {
        let count = 0;
        return enters(list);
        function enters(list) { count++; return hasRealScope(alsoCopied, list); }
        function hasRealScope(own, list) {
            keptClosures.push(() => own);
            const result = mapLike(list, item => item + own + outermost);
            own = own;
            return result.join() + count;
        }
    }
    return { hasDissolvedScope, setOutermost };
}
function makesWalkerWithHandler() {
    let outermost = "first";
    function setOutermost(v) { outermost = v; }
    function callsIt(f) { return f(); }
    function hasDissolvedScope() {
        const hasHandler = (copied, written) => {
            try { keptClosures.push(() => { written = written + 1; }); } catch (e) { }
            { function inBlock(x) { return x + outermost; } return callsIt(() => inBlock(copied) + outermost); }
        };
        return callsIt(() => hasHandler("-", 1));
    }
    return { hasDissolvedScope, setOutermost };
}
function makesLoopBelowDissolvedScope() {
    let outermost = "first";
    function setOutermost(v) { outermost = v; }
    function callsIt(f) { return f(); }
    function hasDissolvedScope() {
        const loops = function () {
            let result = "";
            try {
                for (let i = 0; i < 3; i++) {
                    result += outermost + i;
                    const readsCounter = () => i;
                }
            } catch (e) {
                const readsException = function () { return e; };
                result += "caught " + e;
            }
            return result;
        };
        return callsIt(() => loops());
    }
    return { hasDissolvedScope, setOutermost };
}
function makesCatchBelowDissolvedScope() {
    let outermost = "first";
    function setOutermost(v) { outermost = v; }
    function callsIt(f) { return f(); }
    function throws() { throw 7; }
    function hasDissolvedScope() {
        const catches = function () {
            try {
                throws();
            } catch (e) {
                const writesException = () => { e = e; return e; };
                return outermost + e;
            }
        };
        return callsIt(() => catches());
    }
    return { hasDissolvedScope, setOutermost };
}
function looksUpFromRestoredScope(model) {
    let kept = model;
    const set = (v) => { kept = v; };
    const handler = async (A, E) => {
        let local = 0;
        const bump = () => { local++; };
        identity(bump);
        await null;
        bump();
        let viaInlined;
        {
            const t = A + 1;
            const inner = () => t + 1;
            viaInlined = inner();
        }
        return [kept, A, E, local, viaInlined].join();
    };
    return [set, handler];
}
noInline(looksUpFromRestoredScope);
function asyncReadsAfterAwait(x) { const awaitsFirst = async (y) => { await null; return x + y; }; return awaitsFirst; }
function asyncDeclaration(x) { async function declaredAsync() { const before = x; await null; return before + x; } return declaredAsync; }
function closureInAsync(x) { const makesInAsync = async () => { await null; const inAsync = () => x + 1; return inAsync; }; return makesInAsync; }
for (const [maker, ...holders] of [[readTwoLevelsDown, "outerLevel"], [readThreeLevelsDown, "levelOne"], [asyncClosure], [asyncReadsAfterAwait], [asyncDeclaration], [closureInAsync, "makesInAsync"]]) {
    noInline(maker);
    applies(maker, captures);
    doesNotApply(maker, makesEnvironment);
    for (const holder of holders)
        applies(holder, captures);
}
noInline(holderIsInlined);
applies(holderIsInlined, captures);
applies("passesOn", "inlined-closure:inlinedReader", reads + ":y", captures);
applies("innerLevel", reads + ":x");
applies("levelThree", reads + ":x", reads + ":y");
applies("awaits", reads + ":x");
applies("inAsync", reads + ":x");
for (const f of [assignedLater, assignedInClosure, assignedInOtherClosure, madeBeforeInitialized, readTooEarly, generatorClosure, seenByEval]) {
    noInline(f);
    doesNotApply(f, captures);
}

for (let i = 0; i < 200; i++) {
    check(makesFetcher("s" + i)("u"), "u:s" + i, "a parameter that a closure that is returned reads");
    check(capturesSeveral(1, 2)(4), "1234t1", "parameters and constants");
    const readsObjects = capturesObjects(i);
    if (i % 32 === 0)
        gc();
    check(readsObjects(), i + i + 1, "objects that only the closure refers to");
    const [first, second] = twoClosures(3, 4);
    check(first() * 10 + second(), 37, "two closures that read different variables");
    check(inLoop(4).map(f => f()).join(), "1,2,5,10", "a closure for each iteration");
    check(new Holder(5).capturesThis(2)(), 7, "this");
    const ordinary = ordinaryFunction(10);
    check(ordinary(1) + ordinary.call({ extra: 100 }, 2) + ordinary.apply(undefined, [3]) + ordinary.bind({ extra: 1000 })(4), 11 + 112 + 13 + 1014, "an ordinary function, called in each way");
    check(ordinary.name + ordinary.length + typeof ordinary + (ordinary instanceof Function), "ordinary1functiontrue", "what it is");
    check(typeof new ordinary(1), "object", "constructed");
    ordinary.property = i;
    check(ordinary.property, i, "a property of it");
    const [bump, readsBoth] = underScopeThatStays(1);
    check(readsBoth(), 701, "a capture and a variable of a scope that stays");
    bump();
    check(readsBoth(), 702, "which changes");
    const [makesAndKeeps, kept] = parentOfScopeThatStays();
    check(makesAndKeeps(5) + kept[0](1), 7, "a function that is captured from the parent of a scope that stays");
    check(callsWhatItCaptures(3), 104, "a function that calls by name what its closures capture");
    check(tailCallsWhatItCaptures(5), 7, "and in tail position");
    check(sloppyCalleeSeesGlobal(), true, "a sloppy function that is called by name sees the global object");
    check(storedBeforeOnly(true)() + storedBeforeOnly(false)(), "yesno", "a variable that is assigned on two paths before the closure is made");

    check(assignedLater()(), 5, "a variable that is assigned after the closure is made");
    const bumps = assignedInClosure();
    check(bumps() + bumps(), 3, "a variable that the closure assigns");
    const [sets, gets] = assignedInOtherClosure();
    sets(9);
    check(gets(), 9, "a variable that another closure assigns");
    check(madeBeforeInitialized()(), 3, "a closure that is made before what it reads is initialized");
    check(readTooEarly(), "ReferenceError1", "and is called too early");
    check(hoistedDeclaration()(), undefined, "a declaration that is hoisted");
    check(readTwoLevelsDown(6)()(), 6, "a variable that is read two levels down");
    check(readThreeLevelsDown(1, 2)()(3)(), 123, "variables that are read three levels down");
    check(holderIsInlined(1, 2)(), 803102, "a reader that is made part of the function that holds the variable for it");
    check([...generatorClosure(8)()].join(), "8", "a generator");
    check(seenByEval(11, "x + 1")(), 12, "a variable that evaluated code reads");
    check(loopVariableThatChanges().map(f => f()).join(), "0,1,2", "the variable of a loop");
}
function resultOf(promise) {
    let result;
    promise.then(value => { result = value; }, error => { result = "rejected: " + error; });
    drainMicrotasks();
    return result;
}
const AsyncFunction = (async () => { }).constructor;
for (let i = 0; i < 100; i++) {
    const awaits = asyncClosure(7);
    check(resultOf(awaits()), 7, "an async function");
    check(awaits.constructor === AsyncFunction && Object.getPrototypeOf(awaits) === AsyncFunction.prototype && awaits.name === "awaits" && typeof awaits === "function", true, "what it is");
    check(Object.prototype.toString.call(awaits), "[object AsyncFunction]", "what it says it is");
    check(resultOf(asyncReadsAfterAwait(i)(1)), i + 1, "a variable that is read after an await");
    const [setKept, handler] = looksUpFromRestoredScope("model");
    setKept("changed" + i);
    check(resultOf(handler(1, 2)), "changed" + i + ",1,2,1,3", "a variable that is looked up from a scope that was restored, past a scope that is in no chain");
    check(resultOf(asyncWithParameters(i, 2)), i + 2, "the parameters of an async function");
    check(resultOf(asyncWithParameters(Promise.resolve(i), "x")), i + "x", "one of which is a promise");
    check(resultOf(asyncWithParameters(Promise.reject("no"), 1)), "rejected: no", "or is rejected");
    check(resultOf(asyncArrowWithParameters(i, 3)), i * 10 + 3, "the parameters of an async arrow function");
    check(resultOf(new WithAsyncMethod(i).method(4)), i + 4, "this and a parameter of an async method");
    check([...generatorWithParameters(i, 5)].join(), i + "," + (i + 5), "the parameters of a generator");
    check(catchesWithoutScope(i), i + 1, "a closure that is made in a function that catches");
    check(handlerReadsVariable(i, false) + ":" + handlerReadsVariable(i, true), (i + 1) + ":" + (i + 100), "a handler that reads what a closure captures");
    check(handlerMakesClosure(i, false) + ":" + handlerMakesClosure(i, true), (i + 1) + ":" + i + "thrown", "a handler that makes a closure");
    check(finallyRuns(i, false) + ";" + finallyRuns(i, true), "try" + i + ",finally" + i + ";try" + i + ",catch" + i + ",finally" + i, "try, catch and finally");
    {
        const walker = makesWalker();
        walker.setOutermost("second");
        check(walker.hasDissolvedScope("-", ["a", "b"]), "a-second,b-second1", "an inlined closure that reads a variable beyond a scope that is never made");
    }
    {
        const walker = makesWalkerWithHandler();
        walker.setOutermost("second");
        check(walker.hasDissolvedScope(), "-secondsecond", "a variable beyond a scope that is never made, looked up after a handler");
    }
    {
        const looper = makesLoopBelowDissolvedScope();
        looper.setOutermost("x");
        check(looper.hasDissolvedScope(), "x0x1x2", "a loop with a scope for each iteration, right below a scope that is never made");
    }
    {
        const catcher = makesCatchBelowDissolvedScope();
        catcher.setOutermost("x");
        check(catcher.hasDissolvedScope(), "x7", "a variable beyond a scope that is never made, looked up in a handler");
    }
    check(resultOf(asyncDeclaration(i)()), i + i, "an async function that is declared");
    check(resultOf(closureInAsync(i)())(), i + 1, "a closure that is made in an async function after an await");
}
