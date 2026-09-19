// The target of a shorthand property in an object assignment pattern is a variable that the code uses. The parser
// said so only for a target with a default value, say `v` in `({ v = 1 } = {})`. For `({ w = 1, v } = { v: "new" })`
// it left `v` out, because the parse of the same text as an object literal marks the targets it reads, and that
// parse stops at `w = 1`. A closure around such an assignment did not capture `v`: it wrote to a global variable in
// sloppy code, and threw a ReferenceError in strict code.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${String(expected)} but got ${String(actual)}`);
}

function sloppy() {
    let v = "init", w;
    (() => { ({ w = 1, v } = { v: "new" }); })();
    return v;
}
shouldBe(sloppy(), "new", "shorthand property after a default value");
shouldBe("v" in globalThis, false, "shorthand property after a default value");

function strict() {
    "use strict";
    let v = "init", w;
    (() => { ({ w = 1, v } = { v: "new" }); })();
    return v;
}
shouldBe(strict(), "new", "shorthand property after a default value in strict code");

function nested() {
    let v = "init", w;
    (() => { [{ w = 1, v }] = [{ v: "new" }]; })();
    return v;
}
shouldBe(nested(), "new", "shorthand property after a default value in a nested pattern");

function key() {
    let v = "init", w;
    (() => { ({ k: { w = 1, v } } = { k: { v: "new" } }); })();
    return v;
}
shouldBe(key(), "new", "shorthand property after a default value under a key");

function many() {
    let u = "init", v = "init", w;
    (() => { ({ w = 1, u, v } = { u: "new u", v: "new v" }); })();
    return u + "/" + v;
}
shouldBe(many(), "new u/new v", "two shorthand properties after a default value");

function eachOther() {
    let v = "init", w = "init";
    (() => { ({ w = 1, v } = { v: "new v", w: "new w" }); })();
    return v + "/" + w;
}
shouldBe(eachOther(), "new v/new w", "a target with a default value and one without");

// `arguments` in a shorthand property is the same case, and the function that holds it needs its arguments object.
function argumentsShorthand() {
    let w;
    ({ w = 1, arguments } = { arguments: "new" });
    return arguments;
}
shouldBe(argumentsShorthand("old"), "new", "arguments in a shorthand property");
shouldBe("arguments" in globalThis, false, "arguments in a shorthand property");

async function argumentsShorthandInAsyncFunction() {
    let w;
    ({ w = 1, arguments } = { arguments: "new" });
    return arguments;
}
let result;
argumentsShorthandInAsyncFunction("old").then(value => { result = value; });
drainMicrotasks();
shouldBe(result, "new", "arguments in a shorthand property of an async function");
shouldBe("arguments" in globalThis, false, "arguments in a shorthand property of an async function");
