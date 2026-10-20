//@ runDefault("--watchdog=100", "--watchdog-exception-ok")

// See import-evaluating-module-in-job-inside-evaluation.js. Here the watchdog ends the evaluation while a.js is on
// its stack and an import of a.js waits for it. Nothing runs after that: the test is that the shell exits cleanly.

import("./resources/import-evaluating-module-in-job-inside-evaluation/terminated/root.js");
drainMicrotasks();
