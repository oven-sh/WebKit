//@ requireOptions("--useJSThreads=1", "--destroy-vm")
// SPEC-api 4.6 item 4, history r10.2 (tenth round). join() returns when the
// Thread's completion is published; its native thread then still runs an exit
// tail and only afterwards drops the reference to the VM that its entry closure
// holds. A shell run with --destroy-vm released its own reference right after
// the script, under the API lock, as ~VM's contract asks - but that release was
// then not the last one: the exiting thread made the last one, outside the
// lock, and ~VM fail-stopped there (MC-TDWN S1; a silent SIGABRT in Release).
// The window is the length of the exit tail, so the Thread below leaves its
// exit path a lot to release: SIGABRT in 3 of 40 runs GIL off before. The shell
// now waits (ThreadManager::waitForCompletedThreadsToReleaseVM) for every
// native thread that has published completion to let go of the VM first.
// The test passes by exiting normally; there is nothing to check in JS.
load("../harness.js", "caller relative");

function busy() {
    const keep = [];
    for (let i = 0; i < 60000; ++i) {
        keep.push(new ArrayBuffer(1000));
        keep.push({ a: i, b: [i, i + 1, i + 2], c: "s" + i });
        keep.push(new Array((i & 63) + 1).fill(i));
    }
    return keep.length;
}

// Two that finish early and one that finishes last, joined last: the script
// ends as soon as the last join returns.
const early = [new Thread(() => 1), new Thread(() => 2)];
const last = new Thread(busy);
for (const thread of early)
    thread.join();
if (last.join() !== 180000)
    throw new Error("bad result");
