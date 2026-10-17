log.push("c1");
import("./n.js").then(() => log.push("n fulfilled"), error => log.push(`n rejected (${error.message})`));
// The job that ends the await of t.js runs here, and then the jobs that load and evaluate n.js.
drainMicrotasks();
log.push("c2");
