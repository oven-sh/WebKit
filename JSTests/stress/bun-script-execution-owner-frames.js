//@ skip unless $buildType == "release" or $buildType == "relassert"
//@ runDefault
//@ runNoJIT
//@ runNoLLInt
//@ runNoFTL
//@ runDFGEager
//@ runDFGEagerNoCJITValidate
//@ runEagerJettisonNoCJIT
//@ runFTLEager
//@ runFTLEagerNoCJITValidate
//@ runFTLNoCJITValidate
//@ runFTLNoCJITNoInlineValidate
//@ runMiniMode

// A function of script made under a script execution owner runs with that owner as the current one. When another is
// current as it is called, op_enter makes the function's the current one for as long as the function's frame is there
// (CommonSlowPaths::enterScriptExecutionOwner()): the frame's return PC is replaced with one that puts the previous
// owner back on the way (llint_script_execution_owner_return), and a frame that is unwound instead is the unwinder's to
// put it back for. This is about that frame: what it returns, how it is left, what is left when it is, in every tier.
// $vm.enteredScriptExecutionOwnerCount() is how many frames on the stack have something to put back.

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
const entered = () => $vm.enteredScriptExecutionOwnerCount();
const collect = () => { fullGC(); };
// Nothing is current and nothing is left to put back: what every part of this ends with.
function settled(where) {
    same([current(), entered()], ["none", 0], "after " + where);
}

const path = "./resources/bun-script-execution-owner/frames.js";
const n = testLoopCount;

async function test() {
    const bindings = { current, entered, collect };
    loaders.A = $vm.createModuleLoader(bindings, undefined, true);
    loaders.B = $vm.createModuleLoader(bindings, loaders.A, true);
    const a = await $vm.moduleLoaderImport(loaders.A, path);
    const b = await $vm.moduleLoaderImport(loaders.B, path);
    const c = await $vm.moduleLoaderImport($vm.createModuleLoader(bindings), path);
    settled("importing");

    // What the function returns is what the caller gets, whatever kind of value it is.
    const object = { tag: "object" };
    const values = [0, -0, 1, -1, 0x7fffffff, -0x80000000, 2 ** 31, 2 ** 53, 0.1, -1.5, NaN, Infinity, -Infinity, true, false, null, undefined, "", "string", Symbol.iterator, 10n, 2n ** 80n, object, [1, 2], current];
    for (let i = 0; i < n; ++i) {
        const x = values[i % values.length];
        for (const ns of [a, b]) {
            assert(Object.is(ns.value(x), x), "value(" + String(x) + ")");
            assert(Object.is(ns.valueAfterCall(x), x), "valueAfterCall(" + String(x) + ")");
        }
        assert(Object.is(c.value(x), x), "no owner: value(" + String(x) + ")");
    }
    settled("returning every kind of value");

    // One frame to put back per function that found another owner current; none for a call within the owner.
    for (let i = 0; i < n; ++i) {
        same(a.state(), ["A", 1], "called from outside");
        same(c.state(), ["none", 0], "a function with no owner");
        same(a.calls(a.state), ["A", 1], "A's called by A's");
        same(a.calls(b.state), ["B", 2], "B's called by A's");
        same(a.calls(c.calls, b.state), ["B", 2], "B's called by a function with no owner that A's called");
        same(a.calls(c.state), ["A", 1], "a function with no owner called by A's");
        same(a.calls(b.calls, a.calls, b.state), ["B", 4], "A, B, A, B");
        same(a.calls(() => [current(), entered()]), ["A", 1], "a function of the test's called by A's");
        same(a.callsFixed((x, y) => [current(), entered(), x, y], 1, 2), ["A", 1, 1, 2], "a call that does not spread");
    }
    settled("nesting");

    // Fewer and more arguments than parameters.
    for (let i = 0; i < n; ++i) {
        same(a.fewer(), ["A", 1, null, null, null, null, null, null, 0], "no arguments for six parameters");
        same(a.fewer(1, 2), ["A", 1, 1, 2, null, null, null, null, 2], "two arguments for six parameters");
        same(a.more(1, 2, 3, 4, 5, 6, 7, 8, 9), ["A", 1, 9, 9], "nine arguments for no parameters");
        same(b.calls(a.fewer, 1), ["A", 2, 1, null, null, null, null, null, 1], "too few, from another owner's function");
        same(a.fewer.call(null, 1, 2, 3), ["A", 1, 1, 2, 3, null, null, null, 3], "too few, through call()");
        same(a.more.apply(null, [1, 2, 3]), ["A", 1, 3, 3], "too many, through apply()");
        same(Reflect.apply(a.fewer, null, []), ["A", 1, null, null, null, null, null, null, 0], "too few, through Reflect.apply()");
    }
    settled("calls whose arguments do not match the parameters");

    // A tail call gives the frame away with where it returns to, and with what the frame puts back when it goes: a
    // frame has one thing to put back, however many owners' functions run in it.
    for (let i = 0; i < n; ++i) {
        same(a.tail(c.state), ["A", 1], "a tail call of a function with no owner runs as the caller's");
        same(a.tail(a.state), ["A", 1], "a tail call within the owner");
        same(a.tail(b.state), ["B", 1], "a tail call of another owner's function runs as that owner, in the same frame");
        same(a.tailFixed(b.calls, a.state), ["A", 2], "and what that calls enters in its own");
        same(a.tailFewer(b.fewer), ["B", 1, 1, null, null, null, null, null, 1], "a tail call with fewer arguments than parameters");
        same(a.tailMore(b.more), ["B", 1, 12, 12], "a tail call with more arguments than the caller was given");
        same(a.tail(b.tail, a.tail, b.state), ["B", 1], "a chain of tail calls across owners");
        settled("tail calls");
    }
    // However long the chain: it is one frame.
    for (const hops of [1, 2, 7, 1000, 200000]) {
        const seen = [];
        same(a.hop(hops, a, b, seen), [hops % 2 ? "B" : "A", 1], hops + " hops");
        same([seen.length, seen[0], seen[1], seen[seen.length - 1]], [hops + 1, "A", "B", hops % 2 ? "B" : "A"], "who each hop ran as");
        settled(hops + " hops of tail calls between two owners");
    }

    // Exceptions: a frame that is unwound puts back what it would have on returning; one that catches keeps it.
    const thrown = new Error("thrown");
    for (let i = 0; i < n; ++i) {
        for (const what of [thrown, 1, undefined, "string"]) {
            let caught = "nothing";
            try { a.throws(what); } catch (error) { caught = error; }
            assert(caught === what, "thrown through one frame");
            settled("an exception through one frame");
            try { a.calls(b.calls, a.throws, what); } catch (error) { caught = [error]; }
            assert(caught[0] === what, "thrown through three frames");
            settled("an exception through three frames of two owners");
            try { a.tail(b.tailThrows, what); } catch (error) { caught = [[error]]; }
            assert(caught[0][0] === what, "thrown through tail calls");
            settled("an exception through a frame two owners entered in");
        }
        same(a.catchesOwn(() => b.throws(thrown)).slice(0, 2), ["A", 1], "caught by the function that entered: its frame stays");
        same(a.catchesOwn(() => b.calls(a.calls, b.throws, thrown)).slice(0, 2), ["A", 1], "caught three frames up");
        same(a.calls(b.catchesOwn, () => a.throws(thrown)).slice(0, 2), ["B", 2], "caught in the middle");
        settled("exceptions that are caught inside");
        const log = [];
        try { a.finallyRuns(() => b.finallyRuns(() => a.throws(thrown), log), log); } catch { }
        same(log, [["B", 2], ["A", 1]], "finally blocks on the way out");
        same(a.finallyRuns(() => b.state(), log), ["B", 2], "finally on a normal return");
        settled("finally");
        let message;
        try { a.throwsAfterCalling(b.state, thrown); } catch (error) { message = error.message; }
        same(message, "thrown", "thrown after a call that returned");
        settled("an exception after what was called has returned");
    }
    {
        const deepest = { depth: 0 };
        let error;
        try { a.overflow(a, b, deepest); } catch (e) { error = e; }
        assert(error instanceof RangeError, "the stack overflows: " + error);
        assert(deepest.depth > 100, "every frame entered: " + deepest.depth);
        settled("a stack overflow " + deepest.depth + " frames of two owners deep");
        same(a.calls(b.state), ["B", 2], "and entering works as before");
    }

    // A collection with functions that entered on the stack.
    for (let i = 0; i < 5; ++i) {
        same(a.collects(), 1, "collecting inside one frame");
        same(a.collects(() => b.collects(() => a.collects())), 3, "collecting inside three");
        settled("collecting");
    }

    // Called by native code: the caller's frame is not script's.
    for (let i = 0; i < n; ++i) {
        a.comparator.seen.length = 0;
        same([3, 1, 2].sort(a.comparator), [1, 2, 3], "a comparator");
        assert(a.comparator.seen.length && a.comparator.seen.every(s => s === "A1"), "the comparator ran as its owner: " + a.comparator.seen);
        same(JSON.parse(JSON.stringify({ x: a.json, y: b.json })), { x: ["A", 1], y: ["B", 1] }, "toJSON");
        same("xx".replace(/x/g, a.replacer), "A1A1", "a replacer");
        same(Array.from(a.iterable), [["A", 1], ["A", 1]], "an iterator");
        same(Object.entries(a.accessors), [["prop", ["A", 1]]], "a getter, read by native code");
        same([a.trapped.anything, "anything" in b.trapped], [["A", 1], true], "a proxy's traps");
        same(b.calls(() => JSON.parse(JSON.stringify(a.json))), ["A", 2], "native code called by another owner's function");
        settled("calls native code makes");
    }

    // Constructors.
    for (let i = 0; i < n; ++i) {
        const made = new a.Made(1);
        same([made.state, made.x, made.newTarget], [["A", 1], 1, "Made"], "new");
        const derived = new b.MadeDerived(2);
        same([derived.state, derived.derivedState, derived.x, derived.newTarget, derived instanceof b.Made], [["B", 1], ["B", 1], 2, "MadeDerived", true], "new of a derived class");
        const reflected = Reflect.construct(a.Made, [3], b.MadeDerived);
        same([reflected.state, reflected.newTarget, reflected instanceof b.MadeDerived], [["A", 1], "MadeDerived", true], "Reflect.construct() with another owner's new.target");
        const replaced = new a.returnsObjectFromConstructor(4);
        same([replaced.replaced, replaced.state, "ignored" in replaced], [4, ["A", 1], false], "a constructor that returns an object");
        settled("constructors");
    }

    // Generators and async functions.
    for (let i = 0; i < n; ++i) {
        const generator = a.generator();
        same([generator.next().value, b.calls(() => generator.next().value), generator.next()], [["A", 1], ["A", 2], { value: ["A", 1], done: true }], "every resumption of a generator");
        settled("a generator");
    }
    for (let i = 0; i < 50; ++i) {
        same(await a.asyncFunction(() => settledInside.push([current(), entered()])), [["A", 1], ["A", 0]], "an async function, before and after it awaits");
        settled("an async function");
    }
    same(settledInside[0], ["A", 0], "what an async function calls after it awaits");

    // Tiers changing under a frame that entered.
    same(a.hotLoop(200000), ["A", 1, 100000], "a loop that is entered by OSR");
    settled("OSR entry");
    for (let i = 0; i < n; ++i)
        same(a.reads({ v: 1 }), ["A", 1, 50], "reads");
    same(a.reads({ v: 1.5 }), ["A", 1, 75], "another kind of number");
    same(a.reads({ w: 0, v: "s" })[2].length, 51, "another kind of value, and of object");
    same(a.reads({ get v() { return current() === "A" ? 2 : -1; } }), ["A", 1, 100], "a getter of the test's");
    settled("OSR exit");

    // A loop of the test's calling an owner's function: the call is not inlined once it has been found to enter.
    {
        let sum = 0, wrong = 0;
        for (let i = 0; i < 20 * n; ++i) {
            sum += a.value(i & 3);
            if (entered() || current() !== "none")
                ++wrong;
        }
        same([wrong, sum > 0], [0, true], "a hot loop of calls from outside");
        same(a.callsInside(20 * n), ["A", 1, 20 * n], "a hot loop of calls within the owner");
        settled("hot loops");
    }

    // Called by WebAssembly, whose frames are between the test's and the function's.
    if (typeof WebAssembly !== "undefined") {
        // (module (import "m" "f" (func (result i32))) (func (export "call") (result i32) call 0))
        const bytes = new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, 1, 5, 1, 0x60, 0, 1, 0x7f, 2, 7, 1, 1, 0x6d, 1, 0x66, 0, 0, 3, 2, 1, 0, 7, 8, 1, 4, 0x63, 0x61, 0x6c, 0x6c, 0, 1, 0x0a, 6, 1, 4, 0, 0x10, 0, 0x0b]);
        const module = new WebAssembly.Module(bytes);
        const calls = new WebAssembly.Instance(module, { m: { f: a.fromWasm } }).exports.call;
        const throws = new WebAssembly.Instance(module, { m: { f: b.throwsFromWasm } }).exports.call;
        for (let i = 0; i < 20 * n; ++i) {
            if (calls() !== 1)
                throw new Error("called by WebAssembly: " + calls());
        }
        settled("calls from WebAssembly");
        for (let i = 0; i < n; ++i) {
            let message;
            try { throws(); } catch (error) { message = error.message; }
            same(message, "thrown as B1", "thrown through WebAssembly");
            same(a.calls(() => { try { throws(); } catch (error) { return [error.message, current(), entered()]; } }), ["thrown as B2", "A", 1], "and caught by another owner's caller");
            settled("an exception through WebAssembly");
        }
    }
}
const settledInside = [];

let failure = "did not finish";
test().then(() => { failure = null; }, error => { failure = error; });
drainMicrotasks();
if (failure)
    throw failure;
