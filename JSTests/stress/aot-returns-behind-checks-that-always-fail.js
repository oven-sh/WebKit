//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1", "--validateAOTInferredTypes=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function has(name, remark) {
    let remarks = aotRemarks(name);
    return remarks && remarks.length ? remarks.some(other => other === remark || other.startsWith(remark + ":")) : undefined;
}
function says(name, remark) { check(has(name, remark) !== false, true, name + " has " + remark); }
function doesNotSay(name, remark) { check(has(name, remark) !== true, true, name + " has no " + remark); }

(function () {
function stub() { return null; }
function behindFailingCheck(e, n) { $$t(n, 0x380); return n.list.some(s => s === e); }
function passesNull(e) { let n = stub(); try { return behindFailingCheck(e, n); } catch (x) { return x.constructor.name; } }
function behindPassingCheck(e, n) { $$t(n, 0x380); return n.list.some(s => s === e); }
function passesObject(e) { try { return behindPassingCheck(e, { list: ["a", "b"] }); } catch (x) { return x.constructor.name; } }
check(passesNull("a"), "TypeError", "a check that always fails");
check(passesObject("a"), true, "a check that passes, found");
check(passesObject("c"), false, "a check that passes, not found");
says("behindFailingCheck", "inlined-builtin");
says("behindPassingCheck", "inlined-builtin");
says("passesNull", "direct-call:behindFailingCheck");
says("passesNull", "call-never-returns");
doesNotSay("passesObject", "call-never-returns");
})();
