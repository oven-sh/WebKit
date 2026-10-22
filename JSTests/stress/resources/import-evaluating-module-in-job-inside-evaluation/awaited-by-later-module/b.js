log.push("b1");
globalThis.importOfA = import("./a.js");
track("a", importOfA);
drainMicrotasks();
log.push(`b2(a=${states.a})`);
