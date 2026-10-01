//@ requireOptions("--compileMainScriptAheadOfTime=1")
// A function that runs out of stack in its prologue has no frame by the time the error is thrown. If native code called it, there is
// then no frame of JavaScript code since the VM was entered, and unwinding has to stop there: the native code gets to see the
// exception, and gets its registers back. It must not go on to the handlers of the frames before that.

let depth = 0;
let nativeSawIt = 0;

// Each level enters the VM again: sort() is native and calls the comparator.
function viaSort() {
    depth++;
    let threw = false;
    try {
        [2, 1].sort(comparator);
    } catch (e) {
        if (!(e instanceof RangeError))
            throw e;
        threw = true;
        nativeSawIt++;
    }
    // Only reached if control came back through sort(), to this frame's own handler.
    if (threw)
        throw new RangeError("passed on");
    return depth;
}
function comparator(a, b) { viaSort(); return a - b; }

// The same through a getter called by JSON.stringify(), with locals that have to survive in callee-saved registers.
function viaStringify(a, b, c, d) {
    const before = a + b * 2 + c * 3 + d * 4;
    let result;
    try {
        result = JSON.stringify({ get x() { return viaStringify(a + 1, b + 1, c + 1, d + 1); } });
    } catch (e) {
        if (!(e instanceof RangeError))
            throw e;
        result = "overflow";
    }
    if (a + b * 2 + c * 3 + d * 4 !== before)
        throw new Error("locals changed at depth " + a);
    return result.length;
}

// With frames of many sizes, so that the stack runs out at every kind of place, the prologue of what native code calls among them.
for (let padding = 0; padding < 48; padding++) {
    depth = 0;
    nativeSawIt = 0;
    const pad = new Function("f", "a".repeat(1) + ", " + Array.from({ length: padding }, (_, i) => "p" + i).join(", ") + (padding ? ", " : "") + "z", "return f();");
    let caught = false;
    try {
        pad(viaSort);
    } catch (e) {
        if (!(e instanceof RangeError))
            throw e;
        caught = true;
    }
    if (!caught)
        throw new Error("did not overflow");
    // Every level that caught it passed it on to the one before: none was skipped.
    if (nativeSawIt < depth - 2)
        throw new Error("padding " + padding + ": " + nativeSawIt + " of " + depth + " levels saw the exception");
    if (typeof pad(() => viaStringify(1, 2, 3, 4)) !== "number")
        throw new Error("bad result");
}
