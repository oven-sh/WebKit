log.push("c1");
import("./n.js").then(() => log.push("n fulfilled"), error => log.push(`n rejected (${error.message})`));
// The jobs that load and evaluate n.js run here.
drainMicrotasks();
log.push("c2");
