import defer * as d from "./d.js";
log.push("b1");
// a.js is on the stack of the outer walk.
track("a", import("./a.js"));
drainMicrotasks();
try {
    // A second Evaluate(), of d.js, runs a walk here, and that walk fails.
    d.value;
} catch (error) {
    log.push(`caught(${error.message})`);
}
drainMicrotasks();
log.push(`b2(a=${states.a},e=${states.e})`);
