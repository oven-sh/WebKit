//@ requireOptions("--useJSThreads=1")
//@ threadsRequireGILOff
// GIL-off, the top call frame lives in the current thread's lite, not in the
// VM member. Runtime C++ that reads the VM member walks a stale stack: error
// messages lose their "(evaluating '...')" suffix, f.caller is wrong and
// f.arguments is null. One thread is enough to show it.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + String(expected) + ", got " + String(actual));
}

function messageOf(fn) {
    try {
        fn();
    } catch (e) {
        return e.message;
    }
    return "(no throw)";
}

function readsNull(o) { return o.x; }
noInline(readsNull);

function callee() { return callee.caller; }
function caller() { return callee(); }
noInline(callee);
noInline(caller);

function withArguments(a, b) { return withArguments.arguments; }
noInline(withArguments);

for (let i = 0; i < 10000; ++i) {
    shouldBe(messageOf(() => readsNull(null)), "null is not an object (evaluating 'o.x')", "message at iteration " + i);
    shouldBe(caller(), caller, "caller at iteration " + i);
    const args = withArguments(i, "b");
    shouldBe(args !== null && args[0], i, "arguments at iteration " + i);
}
