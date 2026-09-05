//@ requireOptions("--useJSThreads=1")
// An Error keeps the stack trace it captured until `stack` (or `line`,
// `column`, `sourceURL`) is first read; that read turns the trace into the
// properties and frees it. With the GIL off, several threads can do that first
// read on one shared error at the same time, and one thread used to free the
// trace while another was still walking it (a crash in Release and Debug, on
// every run of this test). One thread does the work now and the others wait
// for it and use its result.
load("../harness.js", "caller relative");

const THREADS = 4;
const ERRORS = 1500;

function deep(n) {
    if (!n)
        return new Error("e" + n);
    return deep(n - 1);
}

const errors = [];
for (let i = 0; i < ERRORS; ++i)
    errors.push(deep(20));

const gate = { go: 0 };
const threads = spawnN(THREADS, (t) => {
    while (Atomics.load(gate, "go") === 0) { }
    const stacks = [];
    // Each thread starts at a different error, so the threads meet often.
    for (let n = 0; n < ERRORS; ++n) {
        const error = errors[(n + t * 7) % ERRORS];
        const stack = (n & 1) ? error.stack : String(Object.getOwnPropertyDescriptor(error, "stack") && error.stack);
        if (typeof stack !== "string" || !stack.includes("deep"))
            throw new Error("thread " + t + ": bad stack for error " + n + ": " + String(stack).slice(0, 80));
        stacks.push(error.stack);
    }
    return stacks;
});
Atomics.store(gate, "go", 1);
const results = joinAll(threads);

// Every thread must have seen the one materialized string of each error.
for (let i = 0; i < ERRORS; ++i) {
    const expected = errors[i].stack;
    shouldBeTrue(typeof expected === "string" && expected.length > 0, "stack of error " + i);
    shouldBe(typeof errors[i].line, "number", "line of error " + i);
}
for (let t = 0; t < THREADS; ++t) {
    for (let n = 0; n < ERRORS; ++n)
        shouldBe(results[t][n], errors[(n + t * 7) % ERRORS].stack, "stack thread " + t + " read for its error " + n);
}
