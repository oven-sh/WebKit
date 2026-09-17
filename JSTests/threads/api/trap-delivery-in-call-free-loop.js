//@ requireOptions("--useJSThreads=1", "--watchdog=400", "--watchdog-exception-ok")
// SPEC-jit I21, history §53 (tenth round): companion of
// gil-on-trap-delivery-without-polls.js for the main thread, in every
// configuration. A call-free loop in optimized code must be reachable by a
// trap: through its polls GIL off (and wherever polling traps are selected),
// through the signal sender GIL on, where polls are no longer compiled. The
// watchdog terminates it; an unreachable loop spins until the runner's timeout.
const cell = { stop: 0 };
function spin() {
    let x = 0;
    while (!cell.stop)
        x = (x + 1) | 0;
    return x;
}
noInline(spin);
spin();
throw new Error("the loop ended by itself");
