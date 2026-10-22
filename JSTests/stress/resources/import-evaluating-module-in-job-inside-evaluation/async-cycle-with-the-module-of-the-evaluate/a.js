import "./root.js";
log.push("a1");
// a.js is a member of the cycle of root.js, which has the capability of the Evaluate() and is asynchronous.
track("a", import("./a.js"));
drainMicrotasks();
log.push(`a2(a=${states.a})`);
