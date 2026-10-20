log.push("b1");
track("x", import("./x.js"));
drainMicrotasks();
log.push(`b2(x=${states.x})`);
