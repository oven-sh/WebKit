import defer * as d from "./d.js";
log.push("b1");
// A second Evaluate(), of d.js, runs a complete walk here, inside the walk that has a.js and b.js on its stack.
d.value;
drainMicrotasks();
log.push(`b2(a=${states.a},e=${states.e})`);
