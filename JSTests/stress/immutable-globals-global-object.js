//@ requireOptions("--immutableGlobals=globalThis")

// The entry globalThis of --immutableGlobals is the global object itself. (This script cannot declare anything at the top level.)

(function () {
    "use strict";

    function shouldBe(actual, expected, message) {
        if (actual !== expected)
            throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
    }

    function shouldThrow(func, errorType) {
        let error;
        try {
            func();
        } catch (e) {
            error = e;
        }
        if (!(error instanceof errorType))
            throw new Error("expected " + errorType.name + " but got " + String(error));
    }

    const isImmutable = $vm.hasImmutableProperties;

    shouldBe(isImmutable(globalThis), true);
    shouldThrow(() => { globalThis.Array = 1; }, TypeError);
    shouldThrow(() => { globalThis.Date = 1; }, TypeError);
    shouldThrow(() => { globalThis.print = 1; }, TypeError);
    shouldThrow(() => { globalThis.fresh = 1; }, TypeError);
    shouldThrow(() => { delete globalThis.Math; }, TypeError);
    shouldThrow(() => { undeclared = 1; }, ReferenceError);

    // The functions it holds, the engine's and the embedder's, materialized by now or not. A constructor brings its prototype.
    for (let object of [Array, Array.prototype, Array.prototype.map, Date, Date.prototype, Date.prototype.getTime, parseInt, isNaN, print, debug])
        shouldBe(isImmutable(object), true);
    // Not the objects it holds that are not functions.
    for (let object of [Math, Math.max, JSON, Intl, performance, $])
        shouldBe(isImmutable(object), false);

    for (let source of ["var a;", "function a() { }"])
        shouldThrow(() => loadString(source), TypeError);
    for (let source of ["let a;", "const a = 1;", "class a { }", "let Array;", "var b; let a;", "function b() { } let a;"])
        shouldThrow(() => loadString(source), SyntaxError);
    shouldBe(typeof a, "undefined");
    shouldBe(typeof b, "undefined");
    shouldBe(loadString("{ let a = 1; a + eval('let b = 2; b') + new Function('let c = 3; return c;')(); }"), 6);
})();
