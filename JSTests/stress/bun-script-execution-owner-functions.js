//@ skip unless $buildType == "release" or $buildType == "relassert"
//@ defaultRun
//@ runNoFTL
//@ runNoLLInt

// A script execution owner is a module loader's scope that says it is one (SymbolTable::isScriptExecutionOwner). A
// function of script made under it runs with it current, whoever calls it (op_enter, CodeBlock::scriptExecutionOwnerDepth()): called
// from outside, it tail-calls @callInScriptExecutionOwner, which makes the call again with the owner current and puts
// back what was. Here: every way of reaching such a function from outside; two owners that share code, so that a
// function's owner is not a constant; functions with no owner; and every tier, before and after a function has first
// been entered from outside.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}
function same(actual, expected, message) {
    assert(JSON.stringify(actual) === JSON.stringify(expected), message + ": " + JSON.stringify(actual) + " instead of " + JSON.stringify(expected));
}

const loaders = {};
function current() {
    for (const name in loaders) {
        if ($vm.isCurrentScriptExecutionOwner(loaders[name]))
            return name;
    }
    return "none";
}
noInline(current);

const path = "./resources/bun-script-execution-owner/owned.js";
const n = testLoopCount;

async function test() {
    loaders.A = $vm.createModuleLoader({ current }, undefined, true);
    loaders.B = $vm.createModuleLoader({ current }, loaders.A, true);
    const notOwned = $vm.createModuleLoader({ current });
    const a = await $vm.moduleLoaderImport(loaders.A, path);
    const b = await $vm.moduleLoaderImport(loaders.B, path);
    const c = await $vm.moduleLoaderImport(notOwned, path);
    assert(a.plain !== b.plain && a.plain !== c.plain, "each loader has its own functions");

    const shapes = (ns) => ({
        "function": ns.plain(),
        "arrow": ns.arrow(),
        "this and arguments": ns.withArguments.call({ tag: "t" }, 1, 2, 3),
        "rest": ns.withRest(1, 2, 3, 4),
        "default parameter": ns.withDefault(),
        "constructor, field and new.target": (() => { const thing = new ns.Thing(7); return [thing.madeAs, thing.field, thing.x, thing.newTargetName, thing instanceof ns.Thing]; })(),
        "derived constructor": (() => { const thing = new ns.Derived(8); return [thing.madeAs, thing.derivedAs, thing.x, thing.newTargetName]; })(),
        "Reflect.construct with another new.target": (() => { function Other() { } Other.prototype = ns.Thing.prototype; return Reflect.construct(ns.Thing, [9], Other).newTargetName; })(),
        "method": new ns.Thing(0).method(),
        "getter": new ns.Thing(0).prop,
        "setter": (() => { const thing = new ns.Thing(0); thing.prop = 1; return thing.setAs; })(),
        "static method": ns.Thing.make(),
        "private method": new ns.Thing(0).viaPrivate(),
        "generator, every next()": (() => { const it = ns.gen(); return [it.next().value, it.next().value, it.next().value]; })(),
        "for-of": [...ns.iterable],
        "toString, Symbol.toPrimitive, toJSON": [String({ toString: ns.coercible.toString }), `${ns.coercible}`, JSON.stringify(ns.coercible)],
        "Proxy trap": ns.trapped.anything,
        "closure": ns.makeClosure()(),
        "bound function": ns.bound(),
        "Reflect.apply": Reflect.apply(ns.plain, undefined, []),
        "array callback": [1, 2].map(ns.plain),
        "new Function inside": ns.made(),
    });
    const expected = (name) => ({
        "function": name,
        "arrow": name,
        "this and arguments": [name, 1, 2, 3, "t"],
        "rest": [name, 4, 4],
        "default parameter": [name, name],
        "constructor, field and new.target": [name, name, 7, "Thing", true],
        "derived constructor": [name, name, 8, "Derived"],
        "Reflect.construct with another new.target": "Other",
        "method": name,
        "getter": name,
        "setter": name,
        "static method": name,
        "private method": name,
        "generator, every next()": [name, name, name],
        "for-of": [name, name],
        "toString, Symbol.toPrimitive, toJSON": [name, name, JSON.stringify(name)],
        "Proxy trap": name,
        "closure": name,
        "bound function": name,
        "Reflect.apply": name,
        "array callback": [name, name],
        "new Function inside": true,
    });

    for (let i = 0; i < n; ++i) {
        same(shapes(a), expected("A"), "A's functions called from outside any owner");
        same(shapes(b), expected("B"), "B's functions called from outside any owner");
        same(shapes(c), expected("none"), "functions with no owner run as their caller");
        assert(current() === "none", "what was current is back");
    }

    // From inside another owner: a function runs as its own, and the caller's is back afterwards.
    for (let i = 0; i < n; ++i) {
        same(a.callsBack(b.plain), ["A", "B", "A"], "B's function called by A's");
        same(a.callsBack(c.plain), ["A", "A", "A"], "a function with no owner called by A's runs as A");
        same(a.callsBack(current), ["A", "A", "A"], "the test's function called by A's runs as A");
    }

    // Asynchronous functions continue as their owner, and the caller continues as itself.
    same(await a.asyncFunction(), ["A", "A"], "async function");
    same(await b.asyncFunction(), ["B", "B"], "async function of the second owner");
    { const it = a.asyncGen(); same([(await it.next()).value, (await it.next()).value], ["A", "A"], "async generator, every next()"); }
    same(await a.thenable, "A", "thenable");
    assert(current() === "none", "what was current is back after awaiting");

    // An exception leaves the owner, is the same exception, and shows the function once.
    for (let i = 0; i < n; ++i) {
        let caught;
        try { a.thrower(); } catch (error) { caught = error; }
        assert(caught.message === "thrown as A" && current() === "none", "thrown through the entry: " + caught.message + ", then " + current());
        assert(caught.stack.split("thrower").length === 2, "the entered function is in the stack once: " + caught.stack);
    }
    assert(a.stackOf().split("stackOf").length === 2, "the entered function is in its own stack once: " + a.stackOf());

    // Entered from outside while it runs hot loops, in every tier: once per call, as its owner. (Made again by an
    // optimizing tier that inlines the entry, the call must not also go on into the function's own code.)
    for (const [name, ns] of [["A", a], ["B", b]]) {
        const before = ns.hotRuns();
        for (let i = 0; i < n; ++i) {
            const result = ns.hot(200);
            assert(result === name + ">" + name + ":200", name + ".hot() ran as " + result);
        }
        assert(ns.hotRuns() === before + n, name + ".hot() ran " + (ns.hotRuns() - before) + " times for " + n + " calls");
    }
    assert(c.hot(10) === "none>none:10", "hot() with no owner");

    // A function only ever called from inside its owner, then entered from outside once its callers are optimized.
    for (let i = 0; i < n; ++i)
        assert(a.callsInner(100) === 100, "callsInner");
    assert(current() === "none", "what was current is back at the end");
}

test().then(() => { }, (error) => {
    print("FAIL", error, error && error.stack);
    $vm.abort();
});
