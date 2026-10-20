// The job that ends the async body of c.js runs here. c.js is on the stack and has an import that waits.
drainMicrotasks();
log.push(`d(c=${states.c})`);
