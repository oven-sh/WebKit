log.push("b1");
// The deferred namespace of a module that is on the stack: nothing waits for an evaluation.
track("a", import.defer("./a.js"));
drainMicrotasks();
log.push(`b2(a=${states.a})`);
