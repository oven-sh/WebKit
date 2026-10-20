log.push("b1");
const importOfA = import("./a.js");
track("a", importOfA);
drainMicrotasks();
log.push(`b2(a=${states.a})`);
// a.js waits for this module.
await importOfA;
log.push("b3");
