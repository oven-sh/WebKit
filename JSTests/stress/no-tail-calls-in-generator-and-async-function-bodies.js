// Per https://tc39.es/ecma262/#sec-isintailposition (steps 4-7), a call is
// never in tail position inside the body of a generator, async function,
// async generator, or async arrow function: the body's frame has to survive
// yield/await suspension, so `return <call-expression>` must not become a
// proper tail call there.
//
// A stray tail call out of those bodies is directly observable: the body's
// frame is replaced by the callee's and disappears from Error().stack. For
// each body kind, the stack depth a callee sees in `return <call>` position
// must match the depth an otherwise identical body produces at a call that
// is not in tail position. An ordinary strict function body IS a tail
// position and must still get the proper tail call (one frame fewer);
// tail-call-recognize.js covers the rest of that side.
"use strict";

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": got " + actual + ", expected " + expected);
}

function captureStack() {
    return new Error().stack;
}
noInline(captureStack);

function depth(stack) {
    return stack.split("\n").length;
}
function hasFrame(stack, name) {
    return stack.split("\n").some(line => line.startsWith(name + "@"));
}

// Control: an ordinary strict function body is a tail position.
function ordinaryTail()    { return captureStack(); }
function ordinaryNonTail() { const s = captureStack(); return s; }

function* generatorTail()    { return captureStack(); }
function* generatorNonTail() { const s = captureStack(); return s; }

// `await 0` first so the synchronous wrapper frame (which shares the
// function's name) has already returned; only the body frame is in question.
async function asyncFunctionTail()    { await 0; return captureStack(); }
async function asyncFunctionNonTail() { await 0; const s = captureStack(); return s; }

async function* asyncGeneratorTail()    { return captureStack(); }
async function* asyncGeneratorNonTail() { const s = captureStack(); return s; }

const asyncArrowTail    = async () => { await 0; return captureStack(); };
const asyncArrowNonTail = async () => { await 0; const s = captureStack(); return s; };

const stacks = {};
stacks.ordinaryTail = ordinaryTail();
stacks.ordinaryNonTail = ordinaryNonTail();
stacks.generatorTail = generatorTail().next().value;
stacks.generatorNonTail = generatorNonTail().next().value;
// Async generators start lazily and run the body synchronously inside next().
asyncGeneratorTail().next().then(result => { stacks.asyncGeneratorTail = result.value; });
asyncGeneratorNonTail().next().then(result => { stacks.asyncGeneratorNonTail = result.value; });
asyncFunctionTail().then(s => { stacks.asyncFunctionTail = s; });
asyncFunctionNonTail().then(s => { stacks.asyncFunctionNonTail = s; });
asyncArrowTail().then(s => { stacks.asyncArrowTail = s; });
asyncArrowNonTail().then(s => { stacks.asyncArrowNonTail = s; });
drainMicrotasks();

// The four excluded body kinds keep their frame.
for (const kind of ["generator", "asyncFunction", "asyncGenerator", "asyncArrow"]) {
    const tail = stacks[kind + "Tail"];
    const nonTail = stacks[kind + "NonTail"];
    shouldBe(typeof tail, "string", kind + " tail stack captured");
    shouldBe(typeof nonTail, "string", kind + " nonTail stack captured");
    shouldBe(depth(tail), depth(nonTail), kind + " stack depth");
}
// The async arrow body frame is anonymous, so it is only covered by the
// depth comparison above. The named bodies must appear by name.
shouldBe(hasFrame(stacks.generatorTail, "generatorTail"), true, "generator body frame present");
shouldBe(hasFrame(stacks.asyncFunctionTail, "asyncFunctionTail"), true, "async function body frame present");
shouldBe(hasFrame(stacks.asyncGeneratorTail, "asyncGeneratorTail"), true, "async generator body frame present");

// Proper tail calls still fire for an ordinary strict function body.
shouldBe(depth(stacks.ordinaryTail), depth(stacks.ordinaryNonTail) - 1, "ordinary strict function stack depth");
shouldBe(hasFrame(stacks.ordinaryTail, "ordinaryTail"), false, "ordinary strict function frame elided");
