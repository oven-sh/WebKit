import "./a.js";
log.push("c1");
track("c", import("./c.js"));
drainMicrotasks();
log.push(`c2(c=${states.c})`);
await 0;
log.push("c3");
