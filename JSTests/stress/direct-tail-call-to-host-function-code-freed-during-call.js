//@ runDefault("--zeroExecutableMemoryOnFree=1", "--useConcurrentJIT=0")
//@ runDefault("--zeroExecutableMemoryOnFree=1", "--useConcurrentJIT=0", "--useFTLJIT=0")

// A direct tail call to a host function must not run the call thunk from the
// caller's own JIT code. The tail call destroys the caller's frame, so nothing
// on the stack refers to the caller's CodeBlock while the host function runs.
// A jettison plus a collection inside the host function then frees the code
// that the host call returns into.
//
// The host function here is encodeURIComponent. It calls toString on its
// argument, and that hook:
//   1. adds a property to the object whose structure the caller's code watches,
//      which jettisons the caller's optimized code, and
//   2. collects, which frees the jettisoned code.
// --zeroExecutableMemoryOnFree fills the freed code with zeroes, so a return
// into it crashes every time instead of once in a while.

"use strict";

const o = { f: encodeURIComponent };

let armed = false;
let hookCalls = 0;
const arg = {
    toString() {
        hookCalls++;
        if (!armed)
            return "x";
        o.g = 1;
        gc();
        return "x";
    }
};

function hot(v) { return o.f(v); }

for (let i = 0; i < 50000; i++)
    hot(arg);

armed = true;
for (let i = 0; i < 3; i++) {
    const result = hot(arg);
    if (result !== "x")
        throw new Error(`expected "x", got ${result}`);
}

if (hookCalls !== 50003)
    throw new Error(`expected 50003 hook calls, got ${hookCalls}`);
