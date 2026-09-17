//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--jitPolicyScale=0")
//@ threadsRequireGILOff
// SPEC-jit §5.6 "Deferred claims in flight" (history §55). The transitions that
// wait for other threads' claimed-but-unfired watchpoint sets must not wait
// inside a stop: haveABadTime converts every array of its realm to
// SlowPutArrayStorage inside its own stop, and a thread that claimed a set and
// then asked for a stop of its own (creating ArrayStorage on an object of a
// watched structure does exactly that) is parked by that very stop, with its
// claim, until the world resumes. The first build with the rule hung there until
// the stop watchdog fired (30 s, SIGABRT): three stress files under the mirror
// harness, `racy-slow-put-cloned-arguments-when-having-a-bad-time.js` in 4 of 40
// runs. Here several threads each give fresh realms a bad time, over and over,
// through a store that also creates ArrayStorage on the realm's Object.prototype.
// The test passes by finishing; every realm's result is checked as well.
load("../harness.js", "caller relative");

const THREADS = 3;
// A Debug build creates a realm some fifty times slower; the deadlock needed a handful of rounds.
const ROUNDS = ($vm.assertEnabled && $vm.assertEnabled()) ? 12 : 160;

const body = `
    function test() {
        "use strict";
        return arguments;
    }
    noInline(test);
    var arrays = [];
    for (var i = 0; i < 10; i++) {
        test(i, i + 1);
        arrays.push([i, i + 1, i + 2]);
    }
    Object.defineProperty(Object.prototype, 0, { value: "from the prototype", configurable: true });
    var args = test();
    if (args[0] !== "from the prototype")
        throw new Error("arguments[0] reads " + String(args[0]));
    for (var a of arrays) {
        if (a.length !== 3 || a[0] + 1 !== a[1])
            throw new Error("an array changed: " + a);
    }
`;

function worker(id) {
    for (let r = 0; r < ROUNDS; ++r)
        runString(body);
    return id;
}

const threads = [];
for (let id = 1; id < THREADS; ++id)
    threads.push(new Thread(worker, id));
worker(0);
for (const thread of threads)
    thread.join();
