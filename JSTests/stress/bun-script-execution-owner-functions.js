//@ skip unless $buildType == "release" or $buildType == "relassert"
// What defaultRun runs, without the bytecode cache: a module of a loader that has bindings (as these do, to say what is
// current) is not written to the disk cache, so --forceDiskCache has nothing to find for it.
//@ runDefault
//@ runNoJIT
//@ runNoLLInt
//@ runNoFTL
//@ runNoCJITValidatePhases
//@ runDFGEager
//@ runDFGEagerNoCJITValidate
//@ runEagerJettisonNoCJIT
//@ runFTLEager
//@ runFTLEagerNoCJITValidate
//@ runFTLNoCJITSmallPool
//@ runFTLNoCJITValidate
//@ runFTLNoCJITNoPutStackValidate
//@ runFTLNoCJITNoInlineValidate
//@ runMiniMode
//@ runLockdown

// A script execution owner is a module loader's scope that is a JSScriptExecutionOwnerEnvironment. A
// function of script made under it runs with it current, whoever calls it (op_enter, CodeBlock::scriptExecutionOwnerDepth()): called
// from outside, it calls @callInScriptExecutionOwner and returns what that does, which makes the call again with the
// owner current and puts back what was. Here: every way of reaching such a function from outside; two owners that share code, so that a
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
    for (let i = 0; i < n; ++i) {
        // Entered from outside, then a tail call: the function is not in the stack, as when it has no owner.
        let owned, notOwned;
        try { a.tailThrower(); } catch (error) { owned = error.stack; }
        try { c.tailThrower(); } catch (error) { notOwned = error.stack; }
        same([owned.split("tailThrower").length, owned.split("thrower").length], [notOwned.split("tailThrower").length, notOwned.split("thrower").length], "a tail call by a function entered from outside: " + owned);
        // B's function calls A's, which calls B's again: each of the three calls is in the stack, once.
        const nested = b.relay((last) => a.relay((last) => b.relay(null, last), last), b.stackOf);
        assert(nested.split("relay").length === 4 && nested.split("stackOf").length === 2, "calls nested across two owners: " + nested);
    }

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

    // A and B share code, so A's tail() tail-calling B's is a tail call of "the same function" to the compiler, which
    // the FTL turns into a jump past op_enter: B's must still run as B. (Both directions, and turn by turn.)
    for (let round = 0; round < 8; ++round) {
        same([a.countTailsNotRunningAs("B", a.tail, b.tail, n * 4), b.countTailsNotRunningAs("A", b.tail, a.tail, n * 4)], [0, 0], "a tail call of the other owner's function with the same code");
        same([a.countTailTurnsNotRunningAs("B", a.tailTurns, b.tailTurns, n * 4), a.countTailsNotRunningAs("A", a.tail, a.tail, n * 4)], [0, 0], "tail calls taking turns between two owners, and within one");
    }
    assert(current() === "none", "what was current is back after the tail calls");

    // A function the host calls too stays a plain inlined call where its own owner calls it: the loop around it is
    // compiled a few times, not over and over (each function that enters an owner goes through the one builtin, whose
    // call has then seen them all).
    for (let i = 0; i < n; ++i)
        assert(a.incrementLoop(100) === 100, "incrementLoop before anything outside calls increment()");
    for (let i = 0; i < n; ++i)
        assert(a.increment(i) === i + 1 && a.label() === "label", "increment() and label() from outside");
    for (let i = 0; i < n; ++i)
        assert(a.incrementLoop(100) === 100, "incrementLoop after");
    if (typeof numberOfDFGCompiles === "function" && $vm.useDFGJIT())
        assert(numberOfDFGCompiles(a.incrementLoop) <= 4, "incrementLoop was compiled " + numberOfDFGCompiles(a.incrementLoop) + " times");

    // A's loop around calls of B's functions, compiled once the calls have found B's owner not to be the current one: what
    // they return is what the loop computes with (an int32, a double, an object), and A is current in between.
    for (let round = 0; round < 6; ++round) {
        same([a.accumulate(b.increment, n * 2, 0), a.accumulate(b.half, n * 2, 0), a.accumulate(b.wrap, n * 2, null).value, a.accumulate(a.increment, n * 2, 0)], [n * 2, n, n * 2, n * 2], "a loop in A around calls of B's functions");
        same(a.callsBack(() => a.accumulate(b.increment, 10, 0)), ["A", 10, "A"], "and A is current around it");
    }

    // Varargs calls inside an owner's code (one owner's: the callee's owner is a constant; then with B running the same
    // code, where it is not), and one that crosses into the other owner.
    for (let round = 0; round < 4; ++round) {
        for (const ns of [a, b, c]) {
            const results = {};
            for (const shape of ["spread", "applyArguments", "applyArray", "newSpread", "reflectApply", "forwardArguments", "superSpread", "superSpreadArguments", "proxyApplyTrap", "proxyConstructTrap", "callSpreadArguments", "applyRest"])
                results[shape] = ns.varargsShapes[shape](n);
            const counting = n * (n - 1) / 2 + n;
            same(results, { spread: counting, applyArguments: counting, applyArray: 3 * n, newSpread: 3 * n, reflectApply: counting, forwardArguments: counting, superSpread: counting, superSpreadArguments: counting, proxyApplyTrap: counting, proxyConstructTrap: counting, callSpreadArguments: counting, applyRest: counting }, "varargs calls inside an owner's code");
        }
        same([a.varargsShapes.crossingSpread(n, b.addOf), b.varargsShapes.crossingSpread(n, a.addOf), a.callsBack(() => a.varargsShapes.crossingSpread(10, b.addOf))], [3 * n, 3 * n, ["A", 30, "A"]], "a varargs call of the other owner's function");
    }
    assert(current() === "none", "what was current is back after the varargs calls");

    // A promise is settled as the owner whose script made it, whoever calls its resolving functions: resolved with a
    // thenable, the job that calls the thenable's then() (a function with no owner, which runs as what is current) was
    // queued by the settling. (An embedder's rejection tracker is told of a rejection by the settling too.)
    for (let i = 0; i < 50; ++i) {
        const seen = [];
        const thenable = (tag) => ({ then(onFulfilled) { seen.push(tag + " as " + current()); onFulfilled(tag); } });
        let resolveOfA, resolveOfB, resolveOfNone;
        const made = [a.makePromise((resolve) => { resolveOfA = resolve; }), b.makePromise((resolve) => { resolveOfB = resolve; }), new Promise((resolve) => { resolveOfNone = resolve; })];
        resolveOfA(thenable("A's, resolved from outside"));
        a.callsBack(() => resolveOfB(thenable("B's, resolved by A")));
        b.callsBack(() => resolveOfNone(thenable("no owner's, resolved by B")));
        assert(current() === "none", "what was current is back after resolving");
        await Promise.all(made);
        same(seen.sort(), ["A's, resolved from outside as A", "B's, resolved by A as B", "no owner's, resolved by B as none"], "a promise is settled as the owner that made it");
    }

    // Promise.withResolvers() makes the same kind of resolving functions in C++.
    for (let i = 0; i < 50; ++i) {
        const seen = [];
        const thenable = (tag) => ({ then(onFulfilled) { seen.push(tag + " as " + current()); onFulfilled(tag); } });
        const ofA = a.makeWithResolvers(), ofNone = Promise.withResolvers();
        b.callsBack(() => ofA.resolve(thenable("A's")));
        a.callsBack(() => ofNone.resolve(thenable("no owner's")));
        await Promise.all([ofA.promise, ofNone.promise]);
        same(seen.sort(), ["A's as A", "no owner's as none"], "a Promise.withResolvers() promise is settled as the owner that made it");
    }

    // The combinators and settlements passed on, by an owner's script and by none's, give what they always gave.
    const combined = JSON.stringify([[1, 1, 2], ["fulfilled", "rejected"], 1, 1, "all rejected", "AggregateError", "race rejected", 1, 1, "passed on"]);
    for (let i = 0; i < 300; ++i) {
        const rejected = Promise.reject(new Error("rejected")); rejected.catch(() => { });
        const rejectedForB = Promise.reject(new Error("rejected")); rejectedForB.catch(() => { });
        const results = await Promise.all([a.combine(Promise.resolve(1), rejected), b.combine(Promise.resolve(1), rejectedForB)]);
        same(JSON.stringify(results[0]), combined, "the combinators, from A's script");
        same(JSON.stringify(results[1]), combined, "the combinators, from B's script");
        assert(current() === "none", "what was current is back after the combinators");
    }

    // Entering an owner is a call like any other: two owners' functions calling each other get within a small factor
    // as deep as one owner's function calling itself (each level is the function, the builtin and the function again).
    {
        const within = a.descend(0, a.descend, a.descend);
        const between = a.descend(0, a.descend, b.descend);
        assert(current() === "none", "what was current is back after running out of stack");
        assert(between * 8 > within, "two owners calling each other got " + between + " deep, one owner " + within);
    }

    // Loaders whose module scopes have the same symbol table share code, which has the owner's depth linked in: not when
    // one's scope is an owner and the other's is not. (In both orders of linking.)
    {
        const plainAfter = await $vm.moduleLoaderImport($vm.createModuleLoader({ current }, loaders.A), path);
        assert(plainAfter.plain() === "none" && current() === "none", "a loader with A's symbol table that is no owner, called from outside: " + plainAfter.plain());
        same(a.callsBack(plainAfter.plain), ["A", "A", "A"], "its function called by A's runs as A");
        same(plainAfter.callsBack(a.plain), ["none", "A", "none"], "A's function called by its function");

        const plainFirstPath = "./resources/bun-script-execution-owner/plain-first.js";
        const plainFirstLoader = $vm.createModuleLoader({ current });
        const plainFirst = await $vm.moduleLoaderImport(plainFirstLoader, plainFirstPath);
        assert(plainFirst.plain() === "none", "linked first by a loader that is no owner");
        loaders.C = $vm.createModuleLoader({ current }, plainFirstLoader, true);
        const ownedSecond = await $vm.moduleLoaderImport(loaders.C, plainFirstPath);
        for (let i = 0; i < n; ++i) {
            assert(ownedSecond.plain() === "C" && plainFirst.plain() === "none", "then by an owner with the same symbol table: " + ownedSecond.plain() + ", " + plainFirst.plain());
            same(ownedSecond.callsBack(plainFirst.plain), ["C", "C", "C"], "the first loader's function called by the owner's");
        }
    }
    assert(current() === "none", "what was current is back at the end");
}

test().then(() => { }, (error) => {
    print("FAIL", error, error && error.stack);
    $vm.abort();
});
