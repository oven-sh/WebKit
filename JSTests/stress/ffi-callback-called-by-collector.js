//@ requireOptions("--useDollarVM=1", "--useExecutableAllocationFuzz=false")
// A callback that is NOT threadsafe, with the collector as its C caller. The callback is the deallocator of
// external ArrayBuffers, so the end phase of the collection that frees them calls it, on a thread that is doing
// GC work, where JS cannot run (the parser's atom table is null there; Interpreter::executeCallImpl asserts
// !vm.isCollectorBusyOnCurrentThread()). The call goes through the dispatch a threadsafe callback uses. Assertions:
//   1) outside a collection the callback runs inline, as before;
//   2) nothing runs during the collection;
//   3) a drain on the JS thread delivers every call exactly once, with the C arguments;
//   4) a callback close()d between the collection and the drain still delivers what the collection queued;
//   5) a call from a thread that does not hold the VM's lock is queued too. That is the thread of another VM when
//      a transferred ArrayBuffer is freed there. It used to wait for the lock, which the JS thread does not give up.
if (!$vm.useJIT()) quit();

const signature = { args: ["ptr", "ptr"], returns: "void" };

function makeBuffers(deallocator, count) {
    for (let context = 1; context <= count; ++context)
        $vm.ffiExternalArrayBuffer(64, deallocator, context);
}
noInline(makeBuffers);

const seen = new Map(); // context -> bytes
function record(bytes, context) {
    if (seen.has(context)) throw new Error("deallocator called twice for context " + context);
    if (typeof bytes !== "number" || !bytes) throw new Error("bad bytes pointer: " + bytes);
    seen.set(context, bytes);
}

const cb = $vm.ffiCallback(signature, record);
if (cb.threadsafe !== false) throw new Error("expected .threadsafe === false, got " + cb.threadsafe);

// (1) Called from C on the JS thread with no collection running: inline.
const callDirectly = $vm.ffiFunction(signature, cb.ptr, "call_deallocator_directly");
callDirectly(4096, 1000000);
if (seen.get(1000000) !== 4096) throw new Error("the callback did not run inline outside a collection");
if ($vm.drainThreadsafeCallbacks() !== 0) throw new Error("an inline call was queued");
seen.clear();

const count = 100;
makeBuffers(cb.ptr, count);
let delivered = 0;
// A buffer the conservative scan still sees takes another round.
for (let round = 0; round < 20 && delivered < count; ++round) {
    fullGC();
    // (2) the end phase freed buffers, and their deallocator did not run there.
    if (seen.size !== delivered) throw new Error("the callback ran during the collection");
    delivered += $vm.drainThreadsafeCallbacks();
    // (3) each queued call ran once.
    if (seen.size !== delivered) throw new Error("drained " + delivered + " calls but the callback ran " + seen.size + " times");
}
if (delivered !== count) throw new Error("expected " + count + " deallocator calls, got " + delivered);
for (let context = 1; context <= count; ++context) {
    if (!seen.has(context)) throw new Error("no deallocator call for context " + context);
}
cb.close();

// (4) close() while calls are queued. `victim` stays referenced to the end of the test: a buffer the conservative
// scan kept past close() still has the thunk as its deallocator (a closed callback drops the call).
seen.clear();
const victim = $vm.ffiCallback(signature, record);
makeBuffers(victim.ptr, count);
fullGC();
victim.close();
for (let i = 0; i < 3; ++i) { fullGC(); edenGC(); }
if (seen.size) throw new Error("the callback ran during the collection");
const late = $vm.drainThreadsafeCallbacks();
if (!late || late > count) throw new Error("expected the queued calls of a closed callback to drain, got " + late);
if (seen.size !== late) throw new Error("drained " + late + " calls of a closed callback but it ran " + seen.size + " times");
if ($vm.drainThreadsafeCallbacks() !== 0) throw new Error("queue not empty");

// (5) The fixture calls the callback on a new thread and waits for that thread, while this thread holds the lock.
const callFromThread = $vm.ffiFunction({ args: ["ptr", "i32", "i64", "u64", "f64"], returns: "void" },
    $vm.ffiFixture("ffi_call_cb_from_thread"), "call_cb_from_thread");
let fromThread = [];
const otherThread = $vm.ffiCallback({ args: ["i32", "i64", "u64", "f64"], returns: "void" },
    (a, b, c, d) => fromThread.push([a, b, c, d]));
callFromThread(otherThread.ptr, -7, -9007199254740993n, 18446744073709551615n, 2.5);
if (fromThread.length) throw new Error("the callback ran on the other thread");
if ($vm.drainThreadsafeCallbacks() !== 1) throw new Error("the call from the other thread was not queued");
const [a, b, c, d] = fromThread[0];
if (fromThread.length !== 1 || a !== -7 || b !== -9007199254740993n || c !== 18446744073709551615n || d !== 2.5)
    throw new Error("wrong arguments from the other thread: " + fromThread.map(String));
otherThread.close();

print("ffi callback called by the collector: all checks passed");
