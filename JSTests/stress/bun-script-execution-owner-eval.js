//@ skip unless $buildType == "release" or $buildType == "relassert"
// As bun-script-execution-owner-functions.js: a module of a loader that has bindings is not written to the disk cache.
//@ requireOptions("--useShadowRealm=1")
//@ runDefault
//@ runNoJIT
//@ runNoLLInt
//@ runDFGEager
//@ runFTLEager

// JSScriptExecutionOwnerEnvironment::setEvalEnabled(false): while that owner is the current one, script is not made
// from a string (JSGlobalObject::currentScriptExecutionOwnerAllowsEval()). The owner that is current is the one whose
// script is calling, so this is about the script of the owner and what it calls; the same global object's other
// script, of another owner or of none, is not affected.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}
function same(actual, expected, message) {
    assert(JSON.stringify(actual) === JSON.stringify(expected), message + ": " + JSON.stringify(actual) + " instead of " + JSON.stringify(expected));
}

const path = "./resources/bun-script-execution-owner/evals.js";
const refused = "EvalError: Code generation from strings disallowed for this context";
const n = testLoopCount;

function run(routes) {
    const result = {};
    for (const name in routes)
        result[name] = routes[name]();
    return result;
}

async function test() {
    const strict = $vm.createModuleLoader({}, undefined, true);
    const lax = $vm.createModuleLoader({}, strict, true);
    const notOwned = $vm.createModuleLoader({});
    $vm.setScriptExecutionOwnerEvalEnabled(strict, false);
    const s = await $vm.moduleLoaderImport(strict, path);
    const l = await $vm.moduleLoaderImport(lax, path);
    const c = await $vm.moduleLoaderImport(notOwned, path);

    const allowed = run(c.routes);
    same(allowed["new Function"], 2, "script with no owner makes script from a string");
    same(allowed["ShadowRealm.prototype.evaluate"], 2, "script with no owner evaluates in a ShadowRealm");
    const whenRefused = {};
    for (const name in allowed)
        whenRefused[name] = refused;
    whenRefused["eval of a number"] = 2;
    whenRefused["a function expression"] = 2;

    for (let i = 0; i < 20; ++i) {
        same(run(s.routes), whenRefused, "the owner that refuses");
        // The two owners share code: what one may do is not a property of the code.
        same(run(l.routes), allowed, "another owner, running the same code");
        same(run(c.routes), allowed, "script with no owner");
    }

    // Script that belongs to no owner runs as its caller: refused when the refusing owner's script calls it, not otherwise.
    const make = () => new Function("return 1 + 1")();
    same(make(), 2, "called with no owner current");
    let caught;
    try { s.callsBack(make); } catch (error) { caught = error; }
    assert(caught instanceof EvalError, "called by the owner that refuses");
    same(l.callsBack(make), 2, "called by another owner");
    same(make(), 2, "and the caller is back to what it was");

    // The owner follows its script across an await.
    same(await s.afterAwait(), refused, "after an await, in the owner that refuses");
    same(await l.afterAwait(), 2, "after an await, in another owner");

    // Every tier: the check is made by eval itself, not by the code that calls it.
    same(s.hot(n), n, "every direct eval in a hot loop is refused");
    same(l.hot(n), 0, "and none is in the other owner's copy of the loop");

    // The setting can be changed back.
    $vm.setScriptExecutionOwnerEvalEnabled(strict, true);
    same(run(s.routes), allowed, "enabled again");
}

test().then(() => { }, (error) => {
    print("FAIL", error, error && error.stack);
    $vm.abort();
});
