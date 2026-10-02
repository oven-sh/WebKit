//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

(function () {
    let scope = { stored: 20 };
    let put;
    with (scope) {
        put = function (text, value) {
            eval(text);
            (function () { stored = value; })();
        };
    }
    for (let i = 0; i < 5; ++i) {
        put("", i);
        check(scope.stored, i, "a variable of a function that calls eval, read from a nested function");
    }
})();

(function () {
    function outer(text, value) {
        eval(text);
        return (function () { return value; })();
    }
    for (let i = 0; i < 5; ++i)
        check(outer("", i), i, "read through a scope that eval may have added to");
    check(outer("var value = 'shadowed'", 1), "shadowed", "eval declares a variable of the same name");
    for (let i = 0; i < 5; ++i)
        check(outer("", i), i, "and does not");
})();

(function () {
    function outer(object, a, b) {
        with (object) {
            return (function () { return a + ":" + b; })();
        }
    }
    for (let i = 0; i < 5; ++i)
        check(outer({ }, i, i + 1), i + ":" + (i + 1), "two variables, read through an object that has neither");
    check(outer({ a: "x" }, 1, 2), "x:2", "the object has one");
    for (let i = 0; i < 5; ++i)
        check(outer({ }, i, i + 1), i + ":" + (i + 1), "and has neither again");
})();

(function () {
    function outer(text) {
        eval(text);
        let uninitialized = () => later;
        try {
            uninitialized();
        } catch (error) {
            check(error instanceof ReferenceError, true, "a variable that is not initialized yet");
            let later = 1;
            return later;
        }
        let later = 2;
        return uninitialized();
    }
    for (let i = 0; i < 5; ++i)
        check(outer(""), 1, "the temporal dead zone");
})();
