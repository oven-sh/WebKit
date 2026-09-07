// Mirror harness: runs one test file's top-level code concurrently on two JS
// threads that share the shell's global object. Wrong output and uncaught
// exceptions are expected and ignored; the harness exists to surface crashes,
// assertion failures, sanitizer reports and hangs.
//
//   jsc --useJSThreads=1 [engine options] mirror.js -- <mode> <file> [threads]
//
// mode "eval": each thread evaluates the source as sloppy indirect eval code,
//   so `var` and function declarations land on the shared global object and the
//   threads race on the objects they name.
// mode "func": the source is compiled once as a function body and that one
//   function is called on every thread, so the threads share every CodeBlock,
//   inline cache, closure structure and profile, but not their locals.
// The run directory should be the test's own directory so that relative
// load() paths resolve as they do under run-jsc-stress-tests.

const mode = arguments[0];
const path = arguments[1];
const nthreads = Number(arguments[2] || 2);
if (!mode || !path)
    throw new Error("usage: mirror.js -- <eval|func> <file> [threads]");

const source = readFile(path);

// Tests signal their own assertion failures through $vm.crash()/$vm.abort(),
// which the driver could not tell from an engine abort. Under the mirror the
// threads corrupt each other's results by construction, so turn those into
// ordinary exceptions; engine assertions do not go through here.
const refusedCrash = function () { throw new Error("mirror: the test asked to crash"); };
if (typeof $vm !== "undefined") {
    // $vm's own properties are frozen; hand the test a forwarding stand-in.
    const realVM = $vm;
    globalThis.$vm = new Proxy({}, {
        get(target, key) { return (key === "abort" || key === "crash") ? refusedCrash : realVM[key]; },
        has(target, key) { return key in realVM; },
    });
}
if (typeof crash === "function")
    globalThis.crash = refusedCrash;

let body;
if (mode === "func")
    body = new Function(source + "\n");
else if (mode === "eval")
    body = function () { (0, eval)(source); };
else
    throw new Error("unknown mode " + mode);

const gate = { arrived: 0 };
function arriveAndWait() {
    Atomics.add(gate, "arrived", 1);
    while (Atomics.load(gate, "arrived") < nthreads) { }
}

function runOne() {
    arriveAndWait();
    body();
}

const threads = [];
for (let i = 1; i < nthreads; ++i)
    threads.push(new Thread(runOne));

let failure = null;
try {
    runOne();
} catch (e) {
    failure = e;
}
for (const t of threads) {
    try {
        t.join();
    } catch (e) {
        failure = failure || e;
    }
}
// Exit the way the shell does for an uncaught exception, so that the driver can
// tell "threw" (exit 3) from "ran to completion" (exit 0); both are non-findings.
if (failure)
    throw failure;
