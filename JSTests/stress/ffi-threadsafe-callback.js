//@ requireOptions("--useDollarVM=1")
// Threadsafe callbacks. The C caller invokes the callback from a FOREIGN OS thread; the engine
// must NOT run JS there. It copies the raw argument slots into a record and hands it to the
// registered dispatch function (here $vm's queue); the JS thread later drains the queue, and only
// THEN are the raw slots converted to JS values (so i64/u64 BigInt boxing happens on the JS
// thread -- the shape of oven-sh/bun#35406). Assertions:
//   1) the callback does NOT run inline during the FFI call (queued, count 0 before drain);
//   2) drain delivers it exactly once with exact values: i32, i64->BigInt, u64->BigInt, f64;
//   3) values that don't fit an int32 (u64 = 2^64-1) round-trip exactly as BigInt;
//   4) a callback close()d before the drain is a silent no-op.
if (!$vm.useJIT()) quit();

const callFromThread = $vm.ffiFunction({ args: ["ptr", "i32", "i64", "u64", "f64"], returns: "void" },
    $vm.ffiFixture("ffi_call_cb_from_thread"), "call_cb_from_thread");

let seen = [];
const cb = $vm.ffiCallback({ args: ["i32", "i64", "u64", "f64"], returns: "void" },
    (a, b, c, d) => seen.push([a, b, c, d]), { threadsafe: true });

if (cb.threadsafe !== true) throw new Error("expected .threadsafe === true, got " + cb.threadsafe);

for (let i = 0; i < 200; ++i) {
    seen = [];
    // (1) invoked from a foreign thread: must be queued, NOT run inline.
    callFromThread(cb.ptr, -7 - i, -9007199254740993n, 18446744073709551615n, 2.5);
    if (seen.length !== 0) throw new Error("threadsafe callback ran INLINE (should be queued): " + JSON.stringify(seen));

    // (2) the JS thread drains: exactly one invocation delivered, exact values.
    const delivered = $vm.drainThreadsafeCallbacks();
    if (delivered !== 1) throw new Error("expected 1 delivered, got " + delivered);
    if (seen.length !== 1) throw new Error("expected 1 seen after drain, got " + seen.length);
    const [a, b, c, d] = seen[0];
    if (a !== -7 - i) throw new Error("i32 wrong: " + a);
    if (b !== -9007199254740993n) throw new Error("i64 wrong: " + String(b) + " (typeof " + typeof b + ")");
    // (3) 2^64-1 must be an exact BigInt, boxed on the JS thread.
    if (c !== 18446744073709551615n) throw new Error("u64 wrong: " + String(c) + " (typeof " + typeof c + ")");
    if (d !== 2.5) throw new Error("f64 wrong: " + d);
}

// (4) close() before drain -- WITH a full GC in between and no JS reference to the callback.
// The queued record holds a raw pointer to the cell: the engine must keep the cell rooted until
// the record drains (finding: unrooting on close() while records are queued was a use-after-free).
seen = [];
{
    let victim = $vm.ffiCallback({ args: ["i32", "i64", "u64", "f64"], returns: "void" },
        (a) => seen.push(a), { threadsafe: true });
    callFromThread(victim.ptr, 111, 2n, 3n, 4.5); // queued, not run
    callFromThread(victim.ptr, 222, 2n, 3n, 4.5); // a second record for the same cell
    victim.close();                                 // close while records are queued
    victim = null;                                  // drop the only JS reference
}
for (let i = 0; i < 5; ++i) { fullGC(); edenGC(); } // cell must survive: it is rooted until drain
const late = $vm.drainThreadsafeCallbacks();       // must NOT crash / touch a swept cell
if (late !== 2) throw new Error("expected 2 records drained even when closed, got " + late);
if (seen.length !== 0) throw new Error("closed threadsafe callback still ran: " + JSON.stringify(seen));
// A closed callback that is fully drained is now collectible; more GC must not resurrect issues.
for (let i = 0; i < 3; ++i) { fullGC(); }
if ($vm.drainThreadsafeCallbacks() !== 0) throw new Error("queue not empty");

// (5) the other test callback still works after all that.
callFromThread(cb.ptr, 5, 6n, 7n, 8.5);
if ($vm.drainThreadsafeCallbacks() !== 1) throw new Error("post-race delivery failed");
if (seen.length !== 1 || seen[0][0] !== 5) throw new Error("wrong post-race value: " + JSON.stringify(seen));
cb.close();

print("ffi threadsafe callback: all checks passed");
