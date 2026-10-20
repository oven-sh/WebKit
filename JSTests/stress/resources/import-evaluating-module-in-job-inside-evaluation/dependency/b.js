log.push("b1");
track("a", import("./a.js"));
track("a2", import("./a.js"));
// The jobs that continue the two imports run here, while a.js is on the stack of the walk.
drainMicrotasks();
log.push(`b2(a=${states.a})`);
