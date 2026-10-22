import "./e.js";
export const value = 1;
// a.js is on the stack of the outer walk.
track("a", import("./a.js"));
drainMicrotasks();
log.push(`d(a=${states.a},e=${states.e})`);
