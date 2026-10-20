log.push("b1");
// The job that rejects t.js runs here. root.js waits for t.js and is on the stack.
drainMicrotasks();
track("inner", import("./root.js"));
drainMicrotasks();
log.push(`b2(inner=${states.inner})`);
throw new Error("from b");
