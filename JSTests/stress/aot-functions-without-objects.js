//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "-m")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useMiniVMModeWithoutJIT=0", "--useConcurrentGC=1", "--collectContinuously=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useMiniVMModeWithoutJIT=0", "--slowPathAllocsBetweenGCs=7")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useMiniVMModeWithoutJIT=0", "--slowPathAllocsBetweenGCs=13")

function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
function thrownBy(f) {
    try {
        f();
    } catch (error) {
        return error.constructor.name;
    }
    return "nothing";
}
const codeIsSloppy = (function () { return !!this; })();
const unknown = { call(f, ...rest) { return f(...rest); }, keep(x) { this.kept = x; return x; } };
const drain = typeof drainMicrotasks === "function" ? drainMicrotasks : () => { };
const collect = typeof gc === "function" ? gc : () => { };

const withoutObject = [];
const withObject = [];

(function () {
    "use strict";

    function callsDeclarations(n) {
        let x = n;
        function addsOne() { try { return x + 1; } catch { } }
        function addsTwo() { try { return x + 2; } catch { } }
        return addsOne() * 100 + addsTwo();
    }
    check(callsDeclarations(1), 203, "declarations that are only called");
    check(callsDeclarations(5), 607, "declarations that are only called, again");
    withoutObject.push("addsOne", "addsTwo");

    function callsSiblings(n) {
        let total = n;
        function first() { try { total += 1; return second() + 1; } catch { } }
        function second() { try { total += 10; return third() + 2; } catch { } }
        function third() { try { total += 100; return total; } catch { } }
        return first();
    }
    check(callsSiblings(0), 114, "declarations that call later siblings");
    check(callsSiblings(1000), 1114, "declarations that call later siblings, again");
    withoutObject.push("first", "second", "third");

    function recurses(n) {
        let steps = 0;
        function down(k) { try { steps++; return k ? down(k - 1) + k : 0; } catch { } }
        return down(n) * 1000 + steps;
    }
    check(recurses(4), 10005, "a function that calls itself by name");
    withoutObject.push("down");

    function nestsDeeply(n) {
        let outermost = n;
        function middle(a) {
            let own = a * 2;
            function innermost(b) { try { outermost++; own++; return outermost * 100 + own * 10 + b; } catch { } }
            try { return innermost(1) + innermost(2); } catch { }
        }
        return middle(1) + outermost;
    }
    check(nestsDeeply(0), 100 + 30 + 1 + 200 + 40 + 2 + 2, "functions without objects inside one another");
    withoutObject.push("middle", "innermost");

    function callsArrow(n) {
        let count = 0;
        const bumps = (by) => { try { count += by; return count; } catch { } };
        bumps(n);
        bumps(n);
        return count;
    }
    check(callsArrow(21), 42, "an arrow function that is only called");
    withoutObject.push("bumps");

    function callsFromClosureThatEscapes(n) {
        let state = n;
        function advance() { try { return ++state; } catch { } }
        return () => advance() + advance();
    }
    {
        const a = unknown.keep(callsFromClosureThatEscapes(0));
        const b = unknown.keep(callsFromClosureThatEscapes(100));
        collect();
        check(a(), 3, "called from a closure that escapes");
        check(b(), 203, "called from another closure that escapes");
        check(a(), 7, "its environment is kept alive by that closure");
    }
    withoutObject.push("advance");

    function perActivation(depth, seen) {
        let mine = depth;
        function reads() { try { return mine; } catch { } }
        if (depth)
            perActivation(depth - 1, seen);
        mine *= 10;
        seen.push(reads());
        return seen;
    }
    check(perActivation(3, []).join(), "0,10,20,30", "each activation has its own");
    withoutObject.push("reads");

    function inBlockAndLoop(n) {
        let sum = 0;
        for (let i = 0; i < n; i++) {
            let twice = i;
            function doubled() { try { return twice + i; } catch { } }
            twice = i * 1;
            sum += doubled();
        }
        return sum;
    }
    check(inBlockAndLoop(5), 20, "declared in the body of a loop");
    withoutObject.push("doubled");

    function throwsFromNested(n) {
        let message = "m" + n;
        function fails() { try { throw new Error(message); } finally { message = ""; } }
        try {
            fails();
        } catch (error) {
            return error.message + ":" + error.stack.split("\n").slice(0, 2).map(line => line.split("@")[0]).join(">");
        }
    }
    check(throwsFromNested(1), "m1:fails>throwsFromNested", "the stack of an error names a function without an object");
    withoutObject.push("fails");

    function capturesStackTrace(n) {
        let holder = { n };
        function captures() { try { Error.captureStackTrace(holder); return holder.stack.split("\n")[0].split("@")[0]; } catch (error) { return String(error); } }
        return typeof Error.captureStackTrace === "function" ? captures() : "captures";
    }
    check(capturesStackTrace(1), "captures", "Error.captureStackTrace in a function without an object");
    withoutObject.push("captures");

    function appliesTwice(f, x) { try { return f(f(x)); } catch { } }
    function passesToClosedFunction(n) {
        let step = 0;
        function stepsUp(x) { try { step += increment; return x + step; } catch { } }
        let increment = n;
        return appliesTwice(stepsUp, 1);
    }
    check(passesToClosedFunction(10), 31, "passed to a function that is known and only calls it");
    withoutObject.push("stepsUp");

    function makesCounter(start) {
        let value = start;
        function nextValue() { try { return value++; } catch { } }
        try { return nextValue; } catch { }
    }
    function usesCounter(start) {
        const counter = makesCounter(start);
        collect();
        return counter() * 100 + counter() * 10 + counter();
    }
    check(usesCounter(1), 123, "returned to a caller that is known and only calls it");
    withoutObject.push("nextValue");

    function overwritesStack(depth) { try { return depth ? overwritesStack(depth - 1) + 1 : 0; } catch { } }
    function keepsLatest(rounds) {
        let latest;
        let sum = 0;
        function makesLatest(n) {
            function tellsBoth() { try { return early.value + late.value; } catch { } }
            let early = { value: n };
            latest = tellsBoth;
            let late = { value: n + 1 };
            try { return 0; } catch { }
        }
        for (let i = 0; i < rounds; i++) {
            makesLatest(i);
            overwritesStack(30);
            sum += latest();
        }
        return sum;
    }
    check(keepsLatest(20000), 20000 * 20000, "a fresh environment that an old one points to is stored to afterwards");
    withoutObject.push("tellsBoth");

    function tailCalls(n) {
        let calls = 0;
        function even(k) { calls++; if (!k) return "even" + calls; return odd(k - 1); }
        function odd(k) { calls++; if (!k) return "odd" + calls; return even(k - 1); }
        return even(n);
    }
    check(tailCalls(7), "odd8", "tail calls between functions without objects");
    check(tailCalls(100000), "even100001", "tail calls between functions without objects do not grow the stack");
    withoutObject.push("even", "odd");

    function* yieldsBetweenCalls(n) {
        let seen = n;
        function more() { try { return ++seen; } catch { } }
        yield more();
        collect();
        yield more();
        return more();
    }
    {
        const generator = yieldsBetweenCalls(1);
        check([generator.next().value, generator.next().value, generator.next().value].join(), "2,3,4", "kept across yield");
    }

    async function awaitsBetweenCalls(n) {
        let seen = n;
        function another() { try { return ++seen; } catch { } }
        const before = another();
        await null;
        collect();
        return before * 10 + another();
    }
    {
        let result;
        awaitsBetweenCalls(1).then(value => { result = value; });
        drain();
        check(result, 23, "kept across await");
    }

    function callsInHandlers(n) {
        let log = "";
        function note(what) { try { log += what; return log.length; } catch { } }
        try {
            note("a");
            if (n)
                unknown.call(() => { throw new Error("x"); });
            note("b");
        } catch {
            note("c");
        } finally {
            note("d");
        }
        return log;
    }
    check(callsInHandlers(0), "abd", "called in try and finally");
    check(callsInHandlers(1), "acd", "called in catch and finally");
    withoutObject.push("note");

    function callsInLoop(values) {
        let sum = 0;
        function adds(value) { try { sum += value; return sum; } catch { } }
        for (let i = 0; i < values.length; i++)
            adds(values[i]);
        return sum;
    }
    check(callsInLoop([1, 2, 3, 4]), 10, "called in a loop");
    check(callsInLoop([1, 2.5, "x", 4]), "3.5x4", "called in the generic copy of a loop");
    check(callsInLoop({ length: 2, 0: 5, 1: 6 }), 11, "called in a loop over something else");
    withoutObject.push("adds");

    function hasDefaultParameters(a, b = () => a + 1, c = b() + 1) {
        let d = c;
        function sums() { try { return a + c + d; } catch { } }
        d++;
        return sums();
    }
    check(hasDefaultParameters(1), 1 + 3 + 4, "in a function with a scope for its parameters");

    function takesManyArguments(n) {
        let base = n;
        function nine(a, b, c, d, e, f, g, h, i) { try { return base + a + b + c + d + e + f + g + h + i; } catch { } }
        function gathers(first, ...rest) { try { return base + first + rest.length; } catch { } }
        return nine(1, 2, 3, 4, 5, 6, 7, 8, 9) * 100 + gathers(1, 2, 3) + nine(1);
    }
    check(String(takesManyArguments(1)), "NaN", "missing arguments are undefined");
    function takesManyArgumentsAgain(n) {
        let base = n;
        function nineAgain(a, b, c, d, e, f, g, h, i) { try { return base + a + b + c + d + e + f + g + h + i; } catch { } }
        function gathersAgain(first, ...rest) { try { return base + first + rest.length; } catch { } }
        return nineAgain(1, 2, 3, 4, 5, 6, 7, 8, 9) * 100 + gathersAgain(1, 2, 3);
    }
    check(takesManyArgumentsAgain(1), 4604, "arguments passed as a list");
    withoutObject.push("nineAgain", "gathersAgain");

    function isNotThereYet(early) {
        let x = 1;
        if (early)
            return later();
        const later = () => { try { return x++; } catch { } };
        return later() + later();
    }
    check(isNotThereYet(false), 3, "a constant that holds a function");
    check(thrownBy(() => isNotThereYet(true)), "ReferenceError", "called before its initialization");

    function mayBeMissing(has) {
        let x = 1;
        let maybe;
        if (has)
            maybe = () => { try { return x++; } catch { } };
        return maybe() + maybe();
    }
    check(mayBeMissing(true), 3, "a variable that may hold a function");
    check(thrownBy(() => mayBeMissing(false)), "TypeError", "undefined is not a function");

    function isTakenAway(when) {
        let x = 1;
        function gone() { try { return x++; } catch { } }
        function takes() { try { gone = undefined; } catch { } }
        const before = gone();
        if (when)
            takes();
        return before + gone();
    }
    check(isTakenAway(false), 3, "a declaration that may be assigned to");
    check(thrownBy(() => isTakenAway(true)), "TypeError", "a declaration that was assigned undefined");

    function isReplacedByAnotherActivation(previous, n) {
        let mine = n;
        function tells() { try { return mine; } catch { } }
        function swaps() { try { if (previous) tells = previous; } catch { } }
        if (!previous)
            return isReplacedByAnotherActivation(tells, n + 1);
        swaps();
        return tells() * 10 + mine;
    }
    check(isReplacedByAnotherActivation(undefined, 1), 12, "the same declaration of another activation");

    function isTestedForTruth(has) {
        let x = 5;
        let handler = null;
        if (has)
            handler = () => { try { return x++; } catch { } };
        return handler ? handler() : handler == null ? -1 : -2;
    }
    check(isTestedForTruth(true), 5, "tested for truth, there");
    check(isTestedForTruth(false), -1, "tested for truth, missing");

    function returnsIt(n) { let x = n; function returned() { try { return x++; } catch { } } return returned; }
    check(unknown.keep(returnsIt(1))(), 1, "returned to unknown code");
    function storesIt(n) { let x = n; function stored() { try { return x++; } catch { } } return { stored }; }
    check(storesIt(2).stored(), 2, "stored in a property");
    function passesIt(n) { let x = n; function passed() { try { return x++; } catch { } } return unknown.call(passed); }
    check(passesIt(3), 3, "passed to unknown code");
    function asksWhatItIs(n) { let x = n; function asked() { try { return x++; } catch { } } return typeof asked + asked(); }
    check(asksWhatItIs(4), "function4", "typeof");
    function readsItsName(n) { let x = n; function named() { try { return x++; } catch { } } return named.name + named(); }
    check(readsItsName(5), "named5", "name");
    function readsItsLength(n) { let x = n; function long(a, b) { try { return x++; } catch { } } return long.length * 10 + long(); }
    check(readsItsLength(6), 26, "length");
    function callsWithCall(n) { let x = n; function calledWithCall() { try { return x++; } catch { } } return calledWithCall.call(null); }
    check(callsWithCall(7), 7, "Function.prototype.call");
    function constructsIt(n) { let x = n; function Constructed() { this.x = x++; } return new Constructed().x + x; }
    check(constructsIt(8), 17, "constructed");
    function comparesIt(n) { let x = n; function compared() { try { return x++; } catch { } } const same = compared; return (same === compared) + compared(); }
    check(comparesIt(9), 10, "compared");
    function mapsWithIt(n) { let x = n; function mapped(v) { try { return v + x++; } catch { } } return [1, 2].map(mapped).join(); }
    check(mapsWithIt(10), "11,13", "passed to a built-in");
    function readsItself(n) { let x = n; const f = function itself() { try { return typeof itself + x++; } catch { } }; return f(); }
    check(readsItself(11), "function11", "a function expression that uses its own name");
    function usesArguments(n) { let x = n; function variadic() { try { return arguments.length + x++; } catch { } } return variadic(1, 2); }
    check(usesArguments(12), 14, "arguments");
    function writesProperty(n) { let x = n; function tagged() { try { return x++; } catch { } } tagged.tag = 1; return tagged.tag + tagged(); }
    check(writesProperty(13), 14, "a property of its own");
    function testsInstance(n) { let x = n; function Kind() { try { return x++; } catch { } } return ({ } instanceof Kind) + Kind(); }
    check(testsInstance(14), 14, "right of instanceof");
    function bindsIt(n) { let x = n; function bound() { try { return x++; } catch { } } return bound.bind(null)(); }
    check(bindsIt(15), 15, "bound");
    function spreadsInto(n) { let x = n; function spreadInto(a, b) { try { return a + b + x++; } catch { } } return spreadInto(...[1, 2]); }
    check(spreadsInto(16), 19, "called with spread arguments");
    function convertsIt(n) { let x = n; function converted() { try { return x++; } catch { } } return ("" + converted).slice(0, 8) + converted(); }
    check(convertsIt(17), "function17", "converted to a string");
    function isAsyncInside(n) { let x = n; async function asynchronous() { return x++; } asynchronous(); return x; }
    check(isAsyncInside(19), 20, "an async function");
    function isGeneratorInside(n) { let x = n; function* generates() { yield x++; } return generates().next().value + x; }
    check(isGeneratorInside(20), 41, "a generator");
    withObject.push("returned", "stored", "passed", "asked", "named", "long", "calledWithCall", "Constructed", "compared", "mapped", "itself", "variadic", "tagged", "Kind", "bound", "spreadInto", "converted", "asynchronous", "generates");

    function holdsWhatItCaptures(n) {
        const held = () => { try { return n + 1; } catch { } };
        return held() + held();
    }
    check(holdsWhatItCaptures(1), 4, "a function that holds what it captures");
    withObject.push("held");
})();

(function () {
    "use strict";
    function evaluates(n) { let x = n; function visibleToEval() { try { return x++; } catch { } } return eval("typeof visibleToEval") + visibleToEval(); }
    check(evaluates(18), "function18", "visible to a direct eval");
    function enclosesEval(n) {
        let x = n;
        function besideEval() { try { return x++; } catch { } }
        function asks(what) { return eval(what); }
        return asks("typeof besideEval") + besideEval();
    }
    check(enclosesEval(19), "function19", "visible to a direct eval in a sibling");
    withObject.push("visibleToEval", "besideEval");
})();

(function () {
    function declaresInBlock(n) {
        let x = n;
        {
            function copied() { try { return x++; } catch { } }
            copied();
        }
        return codeIsSloppy ? typeof copied + copied() : "function" + x;
    }
    check(declaresInBlock(1), "function2", "the copy that sloppy code makes of a function declared in a block");
    function isSloppy(n) {
        let x = n;
        function sloppy() { try { return x++; } catch { } }
        return sloppy() + sloppy();
    }
    check(isSloppy(1), 3, "in sloppy code");
    if (codeIsSloppy)
        withObject.push("copied", "sloppy");
})();

if (codeIsSloppy) {
    (new Function("check", "withObject", `
        const scope = { shadowed: 1 };
        with (scope) {
            (function () {
                "use strict";
                function insideWith(n) {
                    let x = n;
                    function underWith() { try { return x++ + shadowed; } catch { } }
                    return underWith() + underWith();
                }
                check(insideWith(1), 5, "strict code inside with");
            })();
        }
    `))(check, withObject);
}

if (typeof aotRemarks === "function" && aotRemarks("callsDeclarations")) {
    const remarksOf = name => aotRemarks(name) || [];
    for (const name of withObject) {
        if (remarksOf(name).includes("function-has-no-object"))
            throw new Error(name + " needs a function object and has none");
    }
    for (const name of withoutObject) {
        if (!remarksOf(name).includes("function-has-no-object"))
            throw new Error(name + " has a function object that nothing needs: " + remarksOf(name).filter(remark => remark.startsWith("function-")).join(" "));
    }
    const makes = (maker, name) => remarksOf(maker).includes("no-function-object:" + name);
    for (const [maker, name] of [["callsDeclarations", "addsOne"], ["callsSiblings", "second"], ["recurses", "down"], ["middle", "innermost"], ["callsArrow", "bumps"], ["callsFromClosureThatEscapes", "advance"], ["inBlockAndLoop", "doubled"], ["makesCounter", "nextValue"], ["makesLatest", "tellsBoth"], ["tailCalls", "odd"]]) {
        if (!makes(maker, name))
            throw new Error(maker + " makes a function object for " + name);
    }
    for (const [maker, name] of [["returnsIt", "returned"], ["asksWhatItIs", "asked"], ["readsItsName", "named"], ["comparesIt", "compared"], ["holdsWhatItCaptures", "held"]]) {
        if (makes(maker, name))
            throw new Error(maker + " makes no function object for " + name);
    }
    if (remarksOf("callsDeclarations").some(remark => remark.startsWith("allocation-") && remark.includes(":closure")))
        throw new Error("callsDeclarations is said to allocate closures: " + remarksOf("callsDeclarations").join(" "));
    if (!remarksOf("returnsIt").some(remark => remark.startsWith("allocation-escapes:closure")))
        throw new Error("returnsIt is not said to allocate a closure that escapes");
    if (!remarksOf("holdsWhatItCaptures").includes("captures-by-value:1"))
        throw new Error("captures by value no longer come first");
}
