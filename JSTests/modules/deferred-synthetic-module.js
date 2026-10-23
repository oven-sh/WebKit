import { shouldBe } from "./resources/assert.js";

// A *.deferred.js file is a module whose record the shell makes with SyntheticSourceProvider::createDeferred():
// nothing knows what it exports until its body has run, and its body runs where the record is evaluated.

async function rejection(promise) {
    try {
        await promise;
    } catch (error) {
        return error;
    }
    throw new Error("not rejected");
}

// After the modules that are imported ahead of it, once.
await import("./deferred-synthetic-module/order/main.js");
shouldBe(globalThis.orderLog.join(), "setup,lib:configured,nested,after,main");

// Every import form binds to what the body exported.
shouldBe((await import("./deferred-synthetic-module/forms/main.js")).result, JSON.stringify({
    a: 1, b: 2,
    lib: ["a", "b"],
    all: ["a", "b", "default"],
    starA: 1, own: 3,
    starAll: ["a", "b", "own"],
    renamed: 1,
    sameDefault: true,
    ns: ["a", "b", "default"],
    sameNamespace: true,
}));

// Code of an importer that runs before the record is evaluated, in an import cycle, reads undefined, and the values afterwards.
{
    const { peek, viaBarrel } = await import("./deferred-synthetic-module/early/main.js");
    shouldBe(globalThis.earlyLog.join(), "first:undefined|undefined|:undefined,lib,main:number|object|default,x:number");
    for (let i = 0; i < 1e4; ++i) {
        shouldBe(peek(), "number|object|default,x");
        shouldBe(viaBarrel(), "number");
    }
}

// A name that the record does not export is the importer's error, once the body has run. Other importers are not affected.
{
    const error = await rejection(import("./deferred-synthetic-module/missing/main.js"));
    shouldBe(error instanceof SyntaxError, true);
    shouldBe(globalThis.missingLog.join(), "setup,lib");
    shouldBe((await import("./deferred-synthetic-module/missing/other.js")).x, 1);
    shouldBe(globalThis.missingLog.join(), "setup,lib");
}

// What the body throws is the error of every import of it, and the body does not run again.
{
    const error = await rejection(import("./deferred-synthetic-module/throws/main.js"));
    shouldBe(String(error), "Error: lib fails");
    shouldBe(await rejection(import("./deferred-synthetic-module/throws/main.js")), error);
    shouldBe(await rejection(import("./deferred-synthetic-module/throws/lib.deferred.js")), error);
    shouldBe(globalThis.throwsLog.join(), "setup,lib");
}

// `export *` from it and from another module: a name that both export is ambiguous, as it is for two ES modules.
{
    shouldBe((await import("./deferred-synthetic-module/ambiguous/fine.js")).result, "a|lib|onlyA,onlyLib");
    const error = await rejection(import("./deferred-synthetic-module/ambiguous/main.js"));
    shouldBe(error instanceof SyntaxError, true);
    shouldBe(globalThis.ambiguousLog.join(), "lib");
}

// A barrel that re-exports it, in a cycle with a module that imports from the barrel.
await import("./deferred-synthetic-module/barrel-cycle/main.js");
shouldBe(globalThis.barrelLog.join(), "a:undefined,legacy,index,main:helped");

// A graph that does not link runs nothing. The record is evaluated by the next graph that imports it.
{
    const error = await rejection(import("./deferred-synthetic-module/link-failure/main.js"));
    shouldBe(error instanceof SyntaxError, true);
    shouldBe(globalThis.linkFailureLog, undefined);
    const ns = await import("./deferred-synthetic-module/link-failure/lib.deferred.js");
    shouldBe(Object.keys(ns).join(), "default,x");
    shouldBe(globalThis.linkFailureLog.join(), "lib");
}

// A module with a top-level await ahead of it has started, and has not finished.
await import("./deferred-synthetic-module/tla/main.js");
shouldBe(globalThis.tlaLog.join(), "tla:1,lib,tla:2,main");

// A name that exists only once the body has run, because of what a module ahead of it did.
shouldBe((await import("./deferred-synthetic-module/runtime-names/main.js")).computed, 1);
