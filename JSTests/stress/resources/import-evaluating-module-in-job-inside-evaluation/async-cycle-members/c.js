import "./a.js";
log.push("c1");
// Two imports of modules of one cycle, whose root (a.js) is asynchronous.
track("c", import("./c.js"));
track("a", import("./a.js"));
drainMicrotasks();
log.push(`c2(c=${states.c},a=${states.a})`);
