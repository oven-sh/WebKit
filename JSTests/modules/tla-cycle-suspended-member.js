import { shouldBe } from "./resources/assert.js";

// In each graph a.js imports a module that imports a.js back, and then throws.js. That first module runs before a.js
// (it is a dependency) and is suspended at a top-level await while it is still on the stack of Evaluate(): its cycle
// is not complete. Then throws.js throws, and Evaluate() step 9 gives every module on the stack that error.
//
// The suspended body is not affected. ExecuteModule started it with AsyncBlockStart, so it is resumed like the body of
// an async function and runs to its end. AsyncModuleExecutionFulfilled and AsyncModuleExecutionRejected then find the
// module evaluated, with an error, and return.

globalThis.log = [];

function logged() {
    return log.join(", ");
}

// Loads a graph and runs every job: how the import() settled, and what was logged by then.
function load(graph) {
    log.length = 0;
    let result = "pending";
    let loggedWhenSettled;
    const settle = (value) => {
        result = value;
        loggedWhenSettled = logged();
    };
    import(`./tla-cycle-suspended-member/${graph}-a.js`).then(() => settle("fulfilled"), (error) => settle(`rejected: ${error.message}`));
    drainMicrotasks();
    return { result, loggedWhenSettled };
}

// The rest of the body runs, and so does a finally block around the await. The job that resumes the body is the one
// its await queued, which was before throws.js ran: the body ends before the import() is rejected.
{
    const { result, loggedWhenSettled } = load("finally");
    shouldBe(result, "rejected: finally-throws.js");
    shouldBe(loggedWhenSettled, "suspended: start, throws, suspended: after the await, suspended: finally, suspended: end");
    shouldBe(logged(), loggedWhenSettled);
}

// Every later await resumes too.
shouldBe(load("awaits").result, "rejected: awaits-throws.js");
shouldBe(logged(), "suspended: start, throws, suspended: 1, suspended: 2, suspended: 3");

// A rejected promise resumes the body with a throw.
shouldBe(load("rejected-await").result, "rejected: rejected-await-throws.js");
shouldBe(logged(), "suspended: start, throws, suspended: caught rejected, suspended: end");

// A for-await loop resumes the module from the queue of the async generator.
shouldBe(load("for-await").result, "rejected: for-await-throws.js");
shouldBe(logged(), "suspended: start, throws, suspended: 1, suspended: 2, suspended: finally");

// The suspended body keeps its code through a collection, and what it declares after the await is initialized.
{
    let openGate;
    globalThis.gate = new Promise((resolve) => { openGate = resolve; });
    shouldBe(load("gate").result, "rejected: gate-throws.js");
    shouldBe(logged(), "suspended: start, throws");
    fullGC();
    openGate("open");
    drainMicrotasks();
    shouldBe(logged(), "suspended: start, throws, suspended: open, suspended: declared 42, suspended: closure 42");
}

// An error the body throws after the await goes nowhere: the module already has the error of throws.js, and importing
// it again gives that one.
shouldBe(load("late-throw").result, "rejected: late-throw-throws.js");
shouldBe(logged(), "suspended: start, throws, suspended: after the await");
{
    let result = "pending";
    import("./tla-cycle-suspended-member/late-throw-suspended.js").then(() => { result = "fulfilled"; }, (error) => { result = `rejected: ${error.message}`; });
    drainMicrotasks();
    shouldBe(result, "rejected: late-throw-throws.js");
    shouldBe(logged(), "suspended: start, throws, suspended: after the await");
}

// Only a body that has started continues. Here waits.js depends on suspended.js, so it has not started when throws.js
// throws, and it never does.
shouldBe(load("not-started").result, "rejected: not-started-throws.js");
shouldBe(logged(), "suspended: start, throws, suspended: end");
