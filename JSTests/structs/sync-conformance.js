// Deadlock detection, the null prototype of mutexes and conditions, argument validation and the lengths of the
// functions of Atomics.Mutex and Atomics.Condition.
function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }
function throws(fn, type, m) {
    try { fn(); } catch (e) { assert(e instanceof type, m + " threw " + e); return; }
    throw new Error("did not throw: " + m);
}

const m = new Atomics.Mutex();
const c = new Atomics.Condition();

// A mutex that the agent holds cannot be locked again: lock would wait for itself.
{
    const token = Atomics.Mutex.lock(m);
    throws(() => Atomics.Mutex.lock(m), TypeError, "lock of a mutex that this agent holds");
    throws(() => Atomics.Mutex.lockIfAvailable(m, 0), TypeError, "lockIfAvailable with 0");
    throws(() => Atomics.Mutex.lockIfAvailable(m, 10), TypeError, "lockIfAvailable with a timeout");
    throws(() => Atomics.Mutex.lockIfAvailable(m, Infinity), TypeError, "lockIfAvailable with Infinity");
    assert(token.locked, "the failed attempts did not release it");
    token.unlock();
    const again = Atomics.Mutex.lockIfAvailable(m, 0);
    assert(again !== null, "free after the unlock");
    again.unlock();
    // Waiting on a condition unlocks the mutex and takes it again, still as the holder.
    const held = Atomics.Mutex.lock(m);
    assert(Atomics.Condition.waitFor(c, held, 5) === false, "a wait that times out");
    throws(() => Atomics.Mutex.lock(m), TypeError, "the mutex is held again after the wait");
    held.unlock();
}

// Mutexes and conditions are shared structs with no fields: a null prototype, and constructors without a prototype.
assert(Object.getPrototypeOf(m) === null && Object.getPrototypeOf(c) === null, "null prototypes");
assert(!Object.prototype.hasOwnProperty.call(Atomics.Mutex, "prototype") && !Object.prototype.hasOwnProperty.call(Atomics.Condition, "prototype"), "no prototype property");
assert(Atomics.Mutex.prototype === undefined && Atomics.Condition.prototype === undefined, "reading it gives undefined");
throws(() => m instanceof Atomics.Mutex, TypeError, "instanceof needs a prototype");
assert(Reflect.ownKeys(m).length === 0 && !Object.isExtensible(m), "no keys, sealed");

// lockIfAvailable: the mutex, then the token, then the timeout, which must be a Number.
throws(() => Atomics.Mutex.lockIfAvailable(m), TypeError, "no timeout");
throws(() => Atomics.Mutex.lockIfAvailable(m, undefined), TypeError, "undefined timeout");
throws(() => Atomics.Mutex.lockIfAvailable(m, "0"), TypeError, "a string timeout");
throws(() => Atomics.Mutex.lockIfAvailable(m, null), TypeError, "a null timeout");
{
    let converted = false;
    throws(() => Atomics.Mutex.lockIfAvailable(m, { valueOf() { converted = true; return 0; } }), TypeError, "an object timeout");
    assert(!converted, "the timeout is not converted");
    throws(() => Atomics.Mutex.lockIfAvailable({}, 0), TypeError, "not a mutex");
    throws(() => Atomics.Mutex.lockIfAvailable(m, 0, {}), TypeError, "not a token");
    for (const timeout of [0, -0, -1, -Infinity, NaN, Infinity, 1e9]) {
        const token = Atomics.Mutex.lockIfAvailable(m, timeout);
        assert(token !== null, "an uncontended lock with timeout " + timeout);
        token.unlock();
    }
}

// Condition.waitFor: the condition, the token, a timeout that is a Number, a callable predicate.
{
    const held = Atomics.Mutex.lock(m);
    throws(() => Atomics.Condition.waitFor(c, held), TypeError, "no timeout");
    throws(() => Atomics.Condition.waitFor(c, held, "1"), TypeError, "a string timeout");
    throws(() => Atomics.Condition.waitFor(c, held, undefined, () => true), TypeError, "an undefined timeout");
    throws(() => Atomics.Condition.waitFor(c, held, 1, 1), TypeError, "a predicate that is not callable");
    // A timeout that has run out returns false without releasing the mutex.
    assert(Atomics.Condition.waitFor(c, held, 0) === false && held.locked, "timeout 0 without a predicate");
    assert(Atomics.Condition.waitFor(c, held, -5, () => false) === false && held.locked, "a negative timeout with a predicate");
    assert(Atomics.Condition.waitFor(c, held, 0, () => true) === true, "the predicate comes first");
    throws(() => Atomics.Mutex.lock(m), TypeError, "still held by this agent");
    held.unlock();
}

// notify: the count is an integral Number or Infinity.
assert(Atomics.Condition.notify(c) === 0 && Atomics.Condition.notify(c, Infinity) === 0 && Atomics.Condition.notify(c, 0) === 0 && Atomics.Condition.notify(c, 3) === 0 && Atomics.Condition.notify(c, -1) === 0, "valid counts");
for (const count of [1.5, NaN, "1", null, -Infinity, {}, true])
    throws(() => Atomics.Condition.notify(c, count), TypeError, "the count " + String(count));

// Lengths.
assert(Atomics.Mutex.lock.length === 1 && Atomics.Mutex.lockIfAvailable.length === 2, "Mutex function lengths");
assert(Atomics.Condition.wait.length === 2 && Atomics.Condition.waitFor.length === 3 && Atomics.Condition.notify.length === 1, "Condition function lengths");
assert(Atomics.Mutex.length === 0 && Atomics.Condition.length === 0 && Atomics.Mutex.UnlockToken.length === 0, "constructor lengths");

// The prototype of the unlock token has the properties of the proposal and no others.
{
    const proto = Atomics.Mutex.UnlockToken.prototype;
    const keys = Reflect.ownKeys(proto);
    assert(keys.length === 3 && keys.includes("locked") && keys.includes("unlock") && keys.includes(Symbol.dispose), "the properties of the token prototype: " + String(keys.map(String)));
    assert(!Object.prototype.hasOwnProperty.call(proto, "constructor") && !Object.prototype.hasOwnProperty.call(proto, Symbol.toStringTag), "no constructor and no tag");
    assert(Object.getPrototypeOf(proto) === Object.prototype, "the prototype of the prototype");
    const d = Object.getOwnPropertyDescriptor(Atomics.Mutex.UnlockToken, "prototype");
    assert(!d.writable && !d.enumerable && !d.configurable, "the prototype property");
}

// The dispose method of the token is named after its key.
assert(Atomics.Mutex.UnlockToken.prototype[Symbol.dispose].name === "[Symbol.dispose]", "name of the dispose method: " + Atomics.Mutex.UnlockToken.prototype[Symbol.dispose].name);

// A mutex and a condition are shared structs with no fields.
for (const target of [m, c]) {
    throws(() => Atomics.load(target, "x"), RangeError, "load of a field that is not there");
    throws(() => Atomics.load(target, Symbol()), RangeError, "load of a symbol field");
    throws(() => Atomics.store(target, "x", 1), RangeError, "store");
    throws(() => Atomics.exchange(target, "x", 1), RangeError, "exchange");
    throws(() => Atomics.compareExchange(target, "x", 1, 2), RangeError, "compareExchange");
    throws(() => Atomics.load(target, 0), TypeError, "a Number is not a property key");
    throws(() => Atomics.store(target, "x", {}), TypeError, "an unshareable value comes first");
}
