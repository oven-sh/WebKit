log.push("b1");
track("a", import("./a.js"));
drainMicrotasks();
// The job that continues this import runs after the evaluation has failed.
track("a2", import("./a.js"));
log.push(`b2(a=${states.a},a2=${states.a2})`);
