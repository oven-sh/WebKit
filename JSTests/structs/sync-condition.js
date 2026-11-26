function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
function deadlocks(fn) { try { fn(); } catch (e) { return e instanceof TypeError; } return false; }
const m = new Atomics.Mutex();
const c = new Atomics.Condition();
assert(Atomics.Condition.notify(c) === 0, "no waiters");

const held = Atomics.Mutex.lock(m);
assert(Atomics.Condition.waitFor(c, held, 10) === false, "waitFor without a predicate times out");
assert(Atomics.Condition.waitFor(c, held, 10, () => false) === false, "waitFor times out");
assert(Atomics.Condition.waitFor(c, held, 10, () => true) === true, "waitFor sees a true predicate");
// The mutex is held again after a timed-out wait.
assert(deadlocks(() => Atomics.Mutex.lockIfAvailable(m, 0)), "mutex is re-acquired");
assert(held.locked, "still locked after the waits");
assert(held.unlock() === true, "unlock reports it released the mutex");

let threw = false;
try { Atomics.Condition.wait({}, held); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "wait needs a condition");
threw = false;
try { Atomics.Condition.waitFor(c, held, 1, 5); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "predicate must be callable");

// The wait takes the token, not the mutex, and the token has to be locked.
threw = false;
try { Atomics.Condition.waitFor(c, m, 1); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "a mutex is not an unlock token");
threw = false;
try { Atomics.Condition.wait(c, new Atomics.Mutex.UnlockToken()); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "an empty token cannot wait");
threw = false;
try { Atomics.Condition.wait(c, held); } catch (e) { threw = e instanceof TypeError; }
assert(threw, "an unlocked token cannot wait");

// A predicate that unlocks the token (and drops the mutex) is an error, not a wait on a released mutex.
{
    const tok = Atomics.Mutex.lock(new Atomics.Mutex());
    threw = false;
    try { Atomics.Condition.waitFor(c, tok, 50, () => { tok.unlock(); gc(); return false; }); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "waitFor rejects a token unlocked by its predicate");
    threw = false;
    const tok2 = Atomics.Mutex.lock(new Atomics.Mutex());
    let converted = false;
    try { Atomics.Condition.waitFor(c, tok2, { valueOf() { converted = true; tok2.unlock(); return 1; } }); } catch (e) { threw = e instanceof TypeError; }
    assert(threw && !converted, "waitFor does not convert a timeout that is not a Number");
}
