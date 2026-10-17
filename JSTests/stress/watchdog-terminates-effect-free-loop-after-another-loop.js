//@ runDefault("--watchdog=1000", "--watchdog-exception-ok", "--usePollingTraps=false", "--useConcurrentJIT=false")
// With signal-based VM traps, the watchdog (like any VM::notifyNeedTermination() client) can only interrupt
// DFG/FTL code by installing a breakpoint at one of the code block's InvalidationPoints. The infinite loop
// below follows a warm-up loop in the same code block; global CSE used to fold its InvalidationPoint into the
// warm-up loop's (nothing that fires watchpoints separates them), leaving a bare backward jump that no
// termination request could break into, and this test never finished.

function now() { return Date.now(); }
noInline(now);

let start = now();
while (now() - start < 500) { }
for (;;) { }
