log.push("c1");
// The job that ends the await of a module that the walk reached before runs here.
drainMicrotasks();
log.push("c2");
