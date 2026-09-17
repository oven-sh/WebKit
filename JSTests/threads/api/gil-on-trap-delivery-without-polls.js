//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--watchdog=400", "--watchdog-exception-ok")
//@ threadsRequireGILOn
// SPEC-jit I21, history §53 (tenth round). GIL on, polling traps are no longer
// forced: optimized code has no polls, and a trap reaches the running thread
// the way it does flag off - the VMTraps signal sender suspends the thread that
// owns the API lock, checks under the suspension that it still owns it, and
// rewrites the invalidation points of the code it is in. Here the owner is a
// spawned thread: it spins in a call-free, allocation-free loop that tiers up
// to the FTL while the main thread waits in join() (which handed the GIL
// over). Nothing in the loop can notice a trap by itself, so only the signal
// path can end it. The shell's watchdog fires after 400 ms and terminates the
// VM's execution; the test then ends either way (the join returns because the
// spinner was terminated, or the main thread's own termination is accepted by
// --watchdog-exception-ok). Before, the forced polls ended the loop; without
// either, the process spins until the runner's timeout.
load("../harness.js", "caller relative");

const cell = { stop: 0, sink: 0 };
function spin() {
    let x = 0;
    while (!cell.stop) // never set: only a trap ends this loop
        x = (x + 1) | 0;
    return x;
}
noInline(spin);

const spinner = new Thread(() => {
    spin();
    return "returned";
});
spinner.join();
