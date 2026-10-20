// The job that rejects c.js runs here. The evaluation then fails with the error of this body.
drainMicrotasks();
log.push(`d(c=${states.c})`);
throw new Error("from d");
