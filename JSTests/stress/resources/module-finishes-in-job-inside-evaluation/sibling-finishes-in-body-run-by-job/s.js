import "./t.js";
log.push("s1");
// AsyncModuleExecutionFulfilled of t runs this body. The job that ends the await of x runs here.
drainMicrotasks();
log.push("s2");
