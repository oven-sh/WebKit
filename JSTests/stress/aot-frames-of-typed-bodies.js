//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1")
// Typed bodies are switched off (AOT::mayHaveGeneralBody admits no function) until every exact call site is checked against the types its callee's typed body was compiled for. The assertions that a function has one are skipped meanwhile. Every value must hold either way.
const typedBodiesAreSwitchedOff = true;
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
const isCompiled = !!(aotRemarks("check") || []).length;
function remarksOf(name) {
    let remarks = aotRemarks(name);
    if (isCompiled && !(remarks && remarks.length))
        throw new Error("no remarks for " + name);
    return isCompiled ? remarks : null;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(name, ...patterns) {
    let remarks = remarksOf(name);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + name + ": " + remarks.join(" "));
    }
}
function doesNotApply(name, ...patterns) {
    let remarks = remarksOf(name);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + name + ": " + remarks.join(" "));
    }
}
function framesOf(f) {
    try {
        f();
    } catch (error) {
        return error.stack.split("\n");
    }
    throw new Error("nothing was thrown");
}
function nameOfError(f) {
    try {
        f();
    } catch (error) {
        return error.constructor.name;
    }
    return "nothing";
}

(function () {
    const holder = { entered: 0, passed: 0 };

    function throwsAboveTwo(n) {
        holder.entered++;
        if (n > 2)
            throw new Error("above two: " + n);
        return n + 1;
    }
    holder.throwsAboveTwo = throwsAboveTwo;
    function callsThrower(n) { return throwsAboveTwo(n); }
    function callsThrowerThroughProperty(n) { return holder.throwsAboveTwo(n); }

    function catchesAboveTwo(n) {
        try {
            if (n > 2)
                throw n;
            return n + 1;
        } catch (thrown) {
            return -thrown;
        }
    }
    holder.catchesAboveTwo = catchesAboveTwo;
    function callsCatcher(n) { return catchesAboveTwo(n); }

    function alsoThrowsAboveTwo(n) {
        if (n > 2)
            throw new Error("above two: " + n);
        return n + 1;
    }
    function letsThrough(n) {
        try {
            return alsoThrowsAboveTwo(n) * 2;
        } finally {
            holder.passed++;
        }
    }
    holder.letsThrough = letsThrough;
    function callsLetsThrough(n) {
        try {
            return letsThrough(n);
        } catch (error) {
            return error.message;
        }
    }

    function generalFirst(n) { return n + 1; }
    holder.generalFirst = generalFirst;
    function callsGeneralFirst(n) { return generalFirst(n); }

    function recurses(n) { return n ? recurses(n - 1) + 1 : 0; }
    holder.recurses = recurses;
    function callsRecursion(n) { return recurses(n); }

    function staysClosed(n) { return n + 1; }
    function callsClosed(n) { return staysClosed(n); }

    function round(when) {
        holder.entered = holder.passed = 0;
        check(callsThrower(1), 2, when + ": the body for known callers returns");
        check(holder.throwsAboveTwo(1), 2, when + ": the general body returns");
        check(holder.throwsAboveTwo("1"), "11", when + ": the general body takes a string");
        check(holder.throwsAboveTwo(1.5), 2.5, when + ": the general body takes a double");
        check(holder.throwsAboveTwo(), NaN, when + ": the general body takes nothing");
        check(holder.throwsAboveTwo.call(null, 2, "more"), 3, when + ": call");
        check(holder.throwsAboveTwo.apply(null, [0]), 1, when + ": apply");
        check(holder.throwsAboveTwo.bind(null, 1)(), 2, when + ": bind");
        check(Reflect.apply(holder.throwsAboveTwo, null, [2]), 3, when + ": Reflect.apply");
        check([0, 1, 2].map(holder.throwsAboveTwo).join(), "1,2,3", when + ": a callback of a built-in");
        check(holder.entered, 12, when + ": every call entered the function");

        let direct = framesOf(() => callsThrower(5));
        let indirect = framesOf(() => callsThrowerThroughProperty(5));
        check(direct[0], indirect[0], when + ": the position of the throw is the same in both bodies");
        check(direct[0].startsWith("throwsAboveTwo@"), true, when + ": the frame has the function's name");
        check(direct[1].startsWith("callsThrower@"), true, when + ": the caller follows");
        check(direct.length, indirect.length, when + ": as many frames");

        check(callsCatcher(1), 2, when + ": nothing thrown");
        check(callsCatcher(5), -5, when + ": caught in the body for known callers");
        check(holder.catchesAboveTwo(7), -7, when + ": caught in the general body");
        check(holder.catchesAboveTwo("9"), -9, when + ": caught in the general body, a string");

        check(callsLetsThrough(1), 4, when + ": returns through two bodies");
        check(callsLetsThrough(5), "above two: 5", when + ": an exception passes through a body for known callers");
        check(nameOfError(() => holder.letsThrough(5)), "Error", when + ": an exception passes through a general body");
        check(holder.passed, 3, when + ": the finally blocks ran");

        check(callsRecursion(100), 100, when + ": recursion");
        check(holder.recurses(100), 100, when + ": recursion that starts in the general body");
        check(callsClosed(1), 2, when + ": a function that stays closed");
    }

    check(holder.generalFirst("a"), "a1", "the general body is entered first");
    check(callsGeneralFirst(1), 2, "then the body for known callers");
    round("first");
    round("second");
    gc();
    round("after a collection");
    check(nameOfError(() => callsRecursion(1e7)), "RangeError", "the stack overflows in the body for known callers");
    check(nameOfError(() => holder.recurses(1e7)), "RangeError", "the stack overflows from the general body");
    round("after overflows");

    const callsTypedBody = "calls-typed-body";
    if (!typedBodiesAreSwitchedOff) {
        applies("callsThrower", callsTypedBody);
        applies("callsCatcher", callsTypedBody);
        applies("callsLetsThrough", callsTypedBody);
        applies("callsGeneralFirst", callsTypedBody);
        applies("callsRecursion", callsTypedBody);
        applies("recurses", callsTypedBody);
    }
    doesNotApply("callsThrowerThroughProperty", callsTypedBody);
    applies("callsClosed", "direct-call");
    doesNotApply("callsClosed", callsTypedBody);
    for (let name of ["callsThrower", "callsCatcher", "callsLetsThrough", "callsGeneralFirst", "callsRecursion", "callsClosed", "staysClosed"])
        applies(name, "function-does-not-escape");
})();
