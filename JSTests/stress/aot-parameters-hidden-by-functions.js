//@ runDefault("--compileMainScriptAheadOfTime=1")
(function () {
    function check(actual, expected, what) {
        if (!Object.is(actual, expected))
            throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
    }
    function isCompiled(f) {
        if (aotRemarks(f.name))
            check(isAOTCompiled(f), true, f.name + " is compiled");
    }

    function hidesParameter(a) { function a() { return "declared"; } arguments; return () => a; }
    function keepsOthers(a, b) { function a() { } arguments[1] = "changed"; return () => b; }
    function hidesLast(a, b) { function b() { } return (() => arguments.length + ":" + a)(); }
    function hidesBoth(a, b) { function a() { return "a"; } function b() { return "b"; } arguments; return () => a() + b(); }
    function writesHidden(a, value) { function a() { } arguments[0] = value; return (() => arguments[0])(); }
    function handsOutHidden(f) { function f() { return "declared"; } return (() => arguments[0])(); }
    function namesEveryParameter(a, b) { arguments[1] = "changed"; return () => a + b; }

    check(hidesParameter(1)()(), "declared", "the function hides the parameter");
    check(keepsOthers(1, 2)(), "changed", "the other parameter is still mapped");
    check(hidesLast("x", 2, 3), "3:x", "the number of arguments");
    check(hidesBoth(1, 2)(), "ab", "both parameters are hidden");
    check(writesHidden(1, "written"), "written", "a store through arguments");
    check(namesEveryParameter("x", "y")(), "xchanged", "no parameter is hidden");

    let seen = "nothing";
    const passed = value => { seen = value; return typeof value; };
    passed(1);
    const handedOut = handsOutHidden(passed);
    check(typeof handedOut, "function", "what arguments hands out");
    handedOut("a string");
    check(handedOut === passed ? seen : "a string", "a string", "a closure that gets out through arguments may be called with anything");

    const object = { x: 1 };
    check(writesHidden({ x: 2 }, object), object, "an object stored through arguments");

    for (let f of [hidesParameter, keepsOthers, hidesLast, hidesBoth, writesHidden, handsOutHidden, namesEveryParameter])
        isCompiled(f);
})();
