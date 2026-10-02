//@ skip if $architecture != "arm64"
//@ requireOptions("--compileMainScriptAheadOfTime=1")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function inFunctionScope() { let f = function () { return 1; }; return function () { return eval("f()"); }; }
shouldBe(inFunctionScope()(), 1);

{ let f = function () { return 2; }; var inBlockScope = function () { return eval("f()"); }; }
shouldBe(inBlockScope(), 2);

class PrivateMethod { #m() { return 3; } call() { return eval("this.#m()"); } }
shouldBe(new PrivateMethod().call(), 3);

class StaticPrivateMethod { static #m() { return 4; } static call() { return eval("StaticPrivateMethod.#m()"); } }
shouldBe(StaticPrivateMethod.call(), 4);

function strict() { "use strict"; const f = () => 5; return () => eval("f()"); }
shouldBe(strict()(), 5);

function twoLevels() { const f = () => 6; return () => () => eval("f()"); }
shouldBe(twoLevels()()(), 6);

function written() { let f = () => 0; const set = () => eval("f = () => 7"); set(); return f(); }
shouldBe(written(), 7);
