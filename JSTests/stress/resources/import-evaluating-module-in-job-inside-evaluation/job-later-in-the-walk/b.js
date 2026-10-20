log.push("b1");
track("a", import("./a.js"));
drainMicrotasks();
log.push(`b2(a=${states.a})`);
