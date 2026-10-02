// The global object's properties can be made immutable. That covers its top-level `var` and function declarations too, which
// live in its symbol table and are written by compiled code directly: each of them becomes read-only.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

function shouldThrow(func, errorType, message) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error((message ? message + ": " : "") + "expected " + errorType.name + " but got " + String(error));
}

var topLevelVar = 1;
function topLevelFunction() { return "original"; }
function writeTopLevelVar(value) { topLevelVar = value; return topLevelVar; }
function writeTopLevelVarStrict(value) { "use strict"; topLevelVar = value; }
function writeGlobalProperty(value) { plainProperty = value; }
noInline(writeTopLevelVar);
noInline(writeTopLevelVarStrict);
noInline(writeGlobalProperty);

globalThis.plainProperty = "before";
for (let i = 0; i < testLoopCount * 2; i++) {
    writeTopLevelVar(i);
    writeTopLevelVarStrict(i);
    writeGlobalProperty("warm");
}
let ownKeysBefore = Reflect.ownKeys(globalThis).length;

$vm.makePropertiesImmutable(globalThis);
shouldBe($vm.hasImmutableProperties(globalThis), true);
shouldBe(Object.isExtensible(globalThis), false);

// Properties.
shouldThrow(() => { "use strict"; globalThis.Array = function () { }; }, TypeError);
shouldThrow(() => { "use strict"; globalThis.fresh = 1; }, TypeError);
shouldThrow(() => { "use strict"; delete globalThis.JSON; }, TypeError);
shouldThrow(() => Object.defineProperty(globalThis, "Math", { get() { return 1; } }), TypeError);
shouldThrow(() => Object.setPrototypeOf(globalThis, {}), TypeError);
shouldBe(Reflect.defineProperty(globalThis, "Math", { value: Math }), true);
(function () { implicitGlobal = 1; })();
shouldBe(typeof implicitGlobal, "undefined");
shouldBe(Array.name, "Array");
shouldBe(typeof JSON, "object");

// Hot compiled writers of a property and of a top-level variable.
for (let i = 0; i < testLoopCount * 2; i++) {
    writeGlobalProperty("after");
    writeTopLevelVar(-1);
    try {
        writeTopLevelVarStrict(-1);
    } catch { }
}
shouldBe(plainProperty, "warm");
shouldBe(topLevelVar, testLoopCount * 2 - 1);
shouldThrow(() => writeTopLevelVarStrict(-1), TypeError);
shouldThrow(() => { "use strict"; globalThis.topLevelVar = -1; }, TypeError);
shouldBe(Object.getOwnPropertyDescriptor(globalThis, "topLevelVar").writable, false);
shouldBe(Object.getOwnPropertyDescriptor(globalThis, "Array").writable, true);

// Declarations of a later script.
function declares(source) {
    try {
        loadString(source);
    } catch { }
}
declares("var lateVar = 1;");
shouldBe("lateVar" in globalThis, false);
declares("function lateFunction() { }");
shouldBe("lateFunction" in globalThis, false);
declares("function topLevelFunction() { return 'replaced'; }");
shouldBe(topLevelFunction(), "original");
declares("function Array() { return 'replaced'; }");
shouldBe(Array.isArray([]), true);
declares("var topLevelVar;"); // re-declaring an existing variable is allowed and changes nothing
shouldBe(topLevelVar, testLoopCount * 2 - 1);
shouldBe(Reflect.ownKeys(globalThis).length, ownKeysBefore);

// Everything else keeps working.
shouldBe([1, 2, 3].map(x => x * 2).join(), "2,4,6");
shouldBe(typeof Intl.NumberFormat, "function");
shouldBe(new Function("return 1 + 1")(), 2);

// A top-level var or function reads as non-writable afterwards, which an object that inherits from the global object sees too:
// it cannot assign that name, as with a frozen global object. A property of the global object that is not a variable is unaffected.
{
    let child = Object.create(globalThis);
    shouldThrow(() => { "use strict"; child.topLevelVar = 1; }, TypeError);
    shouldBe(Object.hasOwn(child, "topLevelVar"), false);
    child.Array = 1;
    shouldBe(child.Array, 1);
    shouldBe(globalThis.Array === Array && typeof Array === "function", true);
}
