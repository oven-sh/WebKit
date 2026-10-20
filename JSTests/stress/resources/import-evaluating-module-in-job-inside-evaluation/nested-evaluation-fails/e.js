// e.js is on the stack of the inner walk when that walk fails.
track("e", import("./e.js"));
drainMicrotasks();
log.push(`e(e=${states.e})`);
throw new Error("from e");
