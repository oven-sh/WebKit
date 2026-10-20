// e.js is on the stack of the inner walk.
track("e", import("./e.js"));
drainMicrotasks();
log.push(`e(e=${states.e})`);
