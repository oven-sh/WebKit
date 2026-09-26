//@ requireOptions("--useDollarVM=1", "--countJSThreadsCounters=1")
// The counters of the JS threads work count only in a process that has JS threads. Their test begins with the test of
// the threads mode, so that a function compiled once per mode carries no test of them in the copy a process without
// threads runs (JSThreadsCounters::enabled()). Without the flag every counter stays at zero, whatever the options say;
// with it they count as before. Before the change the throws below counted 100 without the flag.
function thrower(i) { throw new Error("e" + i); }
noInline(thrower);
let caught = 0;
for (let i = 0; i < 100; ++i) {
    try {
        thrower(i);
    } catch (e) {
        ++caught;
    }
}
if (caught !== 100)
    throw new Error("caught " + caught);
const counted = $vm.jsThreadsCounter("exceptionThrown");
const hasThreads = typeof Thread === "function";
if (hasThreads ? counted < 100 : counted !== 0)
    throw new Error("exceptionThrown counted " + counted + (hasThreads ? " with" : " without") + " JS threads");
