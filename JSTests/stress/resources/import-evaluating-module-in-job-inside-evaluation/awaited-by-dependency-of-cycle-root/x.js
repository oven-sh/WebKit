log.push("x1");
// c.js is on the stack: a member of the cycle of a.js. a.js waits for this module.
const importOfC = import("./c.js");
track("c", importOfC);
drainMicrotasks();
log.push(`x2(c=${states.c})`);
await importOfC;
log.push("x3");
