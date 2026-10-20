import "./c.js";
// c.js has finished, and the cycle has not: a.js is its root.
drainMicrotasks();
log.push(`a(c=${states.c})`);
