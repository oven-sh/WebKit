// A mutex or condition kept in a shared struct field or a SharedArray element is one object per global object:
// the object that was stored is the object that every way of reading it gives back.
function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }
function deadlocks(fn) { try { fn(); } catch (e) { return e instanceof TypeError; } return false; }

shared struct Holder { mutex; condition; other; }

const m = new Atomics.Mutex();
const c = new Atomics.Condition();
const h = new Holder();
h.mutex = m;
h.condition = c;

assert(h.mutex === m, "the field is the stored mutex");
assert(h.condition === c, "the field is the stored condition");
assert(Atomics.load(h, "mutex") === m, "Atomics.load gives the stored mutex");
assert(Atomics.load(h, "mutex") === h.mutex, "Atomics.load(s, 'm') === s.m");
assert(Atomics.load(h, "condition") === c && Atomics.load(h, "condition") === h.condition, "the same for a condition");
assert(h.mutex !== h.condition, "a mutex is not a condition");

// exchange and compareExchange return the objects that were stored.
const m2 = new Atomics.Mutex();
assert(Atomics.exchange(h, "mutex", m2) === m, "exchange returns the previous mutex object");
assert(h.mutex === m2 && h.mutex !== m, "the new mutex object is stored");
assert(Atomics.compareExchange(h, "mutex", m, m) === m2, "a mismatch gives the current mutex object");
assert(Atomics.compareExchange(h, "mutex", m2, m) === m2, "a match gives the previous mutex object");
assert(h.mutex === m, "the replacement is stored");
assert(Atomics.store(h, "other", m2) === m2, "store returns its argument");
assert(h.other === m2, "a second field holds the second mutex");

// Two structs and an array that hold one mutex hand out one object.
const h2 = new Holder();
h2.mutex = m;
const a = new SharedArray(3);
a[0] = m;
a[1] = c;
a[2] = h;
assert(h2.mutex === h.mutex && a[0] === h.mutex, "one object for one mutex");
assert(a[0] === m && Atomics.load(a, "0") === m && Atomics.load(a, "0") === a[0], "SharedArray element: Atomics.load(a, i) === a[i] === m");
assert(a[1] === c && Atomics.load(a, "1") === c, "SharedArray element holding a condition");
assert(a[2] === h && Atomics.load(a, "2") === h, "a shared struct element keeps identity too");
assert(Atomics.exchange(a, "0", m2) === m && a[0] === m2, "exchange on an element");
assert(Atomics.compareExchange(a, "0", m2, m) === m2 && a[0] === m, "compareExchange on an element");

// The same state wrapped again in this global object is the same object, whichever way it arrives.
assert(atomicsSyncFromState(atomicsSyncState(m), 0) === m, "wrapping the state of a mutex again gives the mutex");
assert(atomicsSyncFromState(atomicsSyncState(c), 1) === c, "wrapping the state of a condition again gives the condition");
assert(sharedStructFromHandle(sharedStructHandle(h)) === h, "a block that comes back by handle is the same struct");
assert(sharedStructFromHandle(sharedArrayHandle(a)) === a, "and the same array");

// The objects that are read are the real thing: locking one excludes the others.
{
    using token = Atomics.Mutex.lock(h.mutex);
    assert(deadlocks(() => Atomics.Mutex.lockIfAvailable(m, 0)), "locked through the field");
    assert(deadlocks(() => Atomics.Mutex.lockIfAvailable(a[0], 0)), "and through the element");
}
assert(Atomics.Mutex.lockIfAvailable(m, 0) !== null, "unlocked");

// Identity survives collections while the object is referenced.
gc();
assert(h.mutex === m && a[0] === m && h.condition === c, "identity after gc");

// A mutex that nobody references any more can still be read back, as a working mutex over the same state.
let temporary = new Atomics.Mutex();
h.other = temporary;
const state = new Int32Array(atomicsSyncState(temporary));
temporary = undefined;
gc();
const again = h.other;
assert(Object.getPrototypeOf(again) === null && again === h.other, "a read-back mutex is a mutex and keeps its identity");
{
    using token = Atomics.Mutex.lock(again);
    assert(state[0] !== 0, "the read-back mutex locks the state of the one that was stored");
}
assert(state[0] === 0, "released");
