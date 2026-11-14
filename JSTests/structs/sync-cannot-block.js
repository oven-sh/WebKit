// jsc-options: --can-block-is-false
// An agent that cannot suspend (the way Atomics.wait is refused in a browser's main thread): lock, the predicate and the
// waits check AgentCanSuspend before they do anything, and lockIfAvailable only with a timeout other than 0.
function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }
function throws(fn, type, m) {
    try { fn(); } catch (e) { assert(e instanceof type, m + " threw " + e); return; }
    throw new Error("did not throw: " + m);
}

throws(() => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 1), TypeError, "this agent cannot block");

const m = new Atomics.Mutex();
const c = new Atomics.Condition();
throws(() => Atomics.Mutex.lock(m), TypeError, "lock throws even though the mutex is free");
throws(() => Atomics.Mutex.lockIfAvailable(m, 10), TypeError, "lockIfAvailable with a timeout");
throws(() => Atomics.Mutex.lockIfAvailable(m, Infinity), TypeError, "lockIfAvailable with Infinity");
const token = Atomics.Mutex.lockIfAvailable(m, 0);
assert(token !== null && token.locked, "lockIfAvailable with 0 does not need to block");
throws(() => Atomics.Mutex.lockIfAvailable(m, 0), TypeError, "and finds the deadlock");
let called = false;
throws(() => Atomics.Condition.waitFor(c, token, 10, () => { called = true; return true; }), TypeError, "waitFor throws before the predicate");
assert(!called, "the predicate was not called");
throws(() => Atomics.Condition.waitFor(c, token, 0), TypeError, "waitFor with timeout 0");
throws(() => Atomics.Condition.wait(c, token), TypeError, "wait");
assert(token.locked, "the token still holds the mutex");
token.unlock();
