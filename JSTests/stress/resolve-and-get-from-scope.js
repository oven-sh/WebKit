// resolve_scope + get_from_scope fused into resolve_and_get_from_scope: the cases that differ in how they resolve.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}`);
}
function shouldThrow(func, errorType, message) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error(`expected ${errorType.name}, got ${String(error)}`);
    if (message !== undefined && error.message !== message)
        throw new Error(`bad message: ${error.message}`);
}

// GlobalVar, GlobalLexicalVar, GlobalProperty.
var globalVar = 1;
let globalLet = 2;
globalThis.globalProp = 3;
function readGlobals() { return globalVar + globalLet + globalProp; }
for (let i = 0; i < 1e4; ++i)
    shouldBe(readGlobals(), 6);
globalVar = 10;
globalLet = 20;
globalThis.globalProp = 30;
shouldBe(readGlobals(), 60);

// A global that does not exist yet: UnresolvedProperty, then GlobalProperty once defined.
function readLate() { return lateGlobal; }
shouldThrow(readLate, ReferenceError);
globalThis.lateGlobal = 7;
for (let i = 0; i < 1e4; ++i)
    shouldBe(readLate(), 7);
delete globalThis.lateGlobal;
shouldThrow(readLate, ReferenceError);

// typeof on an undeclared name must not throw.
function typeofUndeclared() { return typeof neverDeclaredAnywhere; }
for (let i = 0; i < 1e4; ++i)
    shouldBe(typeofUndeclared(), "undefined");

// A global lexical in its TDZ.
function readTDZ() { return tdzLet; }
shouldThrow(readTDZ, ReferenceError);
let tdzLet = 5;
shouldBe(readTDZ(), 5);

// ClosureVar through several scope levels.
function outer() {
    let a = 1;
    return function middle() {
        let b = 2;
        return function inner() {
            return a + b;
        };
    }();
}
{
    const inner = outer();
    for (let i = 0; i < 1e4; ++i)
        shouldBe(inner(), 3);
}

// A bare call resolved through a scope passes undefined as `this`: sloppy callee sees globalThis, strict sees undefined.
function sloppyThis() { return this; }
function strictThis() { "use strict"; return this; }
function callThem() { return [sloppyThis(), strictThis()]; }
for (let i = 0; i < 1e4; ++i) {
    const [sloppy, strict] = callThem();
    shouldBe(sloppy, globalThis);
    shouldBe(strict, undefined);
}
{
    let closureSloppy = function () { return this; };
    let closureStrict = function () { "use strict"; return this; };
    function callClosures() { return [closureSloppy(), closureStrict()]; }
    for (let i = 0; i < 1e4; ++i) {
        const [sloppy, strict] = callClosures();
        shouldBe(sloppy, globalThis);
        shouldBe(strict, undefined);
    }
}

// Tagged templates resolved through a scope likewise.
function tag(strings) { return [this, strings[0]]; }
function callTag() { return tag`x`; }
for (let i = 0; i < 1e4; ++i) {
    const [thisValue, str] = callTag();
    shouldBe(thisValue, globalThis);
    shouldBe(str, "x");
}

// Inside `with`, resolution is dynamic and stays unfused: the with object wins and is `this` for calls.
function withRead(obj) {
    with (obj)
        return [globalVar, f()];
}
{
    const obj = { globalVar: 99, f() { return this; } };
    for (let i = 0; i < 1e4; ++i) {
        const [value, thisValue] = withRead(obj);
        shouldBe(value, 99);
        shouldBe(thisValue, obj);
    }
    const [value, thisValue] = withRead({ f() { return this; } });
    shouldBe(value, 10);
}

// Sloppy direct eval injecting a var flips the *WithVarInjectionChecks types.
function injected() {
    eval("var injectedVar = 1");
    function read() { return injectedVar; }
    for (let i = 0; i < 1e4; ++i)
        shouldBe(read(), 1);
    eval("var injectedVar = 2");
    shouldBe(read(), 2);
}
injected();

// A read of a global whose lexical binding epoch changes after caching.
function readShadowed() { return shadowedLater; }
globalThis.shadowedLater = "prop";
for (let i = 0; i < 1e4; ++i)
    shouldBe(readShadowed(), "prop");

// A function nested inside `with` resolves through the with object at runtime even though its own scope chain is static.
{
    const obj = { h() { return this; }, nestedX: 1 };
    function makeNested() { with (obj) { return function nested() { return [h(), nestedX, typeof nestedX]; }; } }
    const nested = makeNested();
    for (let i = 0; i < 1e4; ++i) {
        const [thisValue, value, type] = nested();
        shouldBe(thisValue, obj);
        shouldBe(value, 1);
        shouldBe(type, "number");
    }
}
