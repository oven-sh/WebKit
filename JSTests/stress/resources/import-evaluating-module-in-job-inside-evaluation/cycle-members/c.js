import "./a.js";
log.push("c1");
// c.js is a member of the cycle of a.js, and not its root.
track("c", import("./c.js"));
track("a", import("./a.js"));
drainMicrotasks();
log.push(`c2(c=${states.c},a=${states.a})`);
