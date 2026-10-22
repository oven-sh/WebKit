log.push("b1");
track("inner", import("./root.js"));
drainMicrotasks();
log.push(`b2(inner=${states.inner})`);
