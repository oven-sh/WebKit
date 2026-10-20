// The job that ends the async body of c.js runs here. c.js stays on the stack: its cycle is not closed.
drainMicrotasks();
track("c", import("./c.js"));
drainMicrotasks();
log.push(`d(c=${states.c})`);
