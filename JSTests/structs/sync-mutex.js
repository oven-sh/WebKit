function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
function deadlocks(fn) { try { fn(); } catch (e) { return e instanceof TypeError; } return false; }
const m = new Atomics.Mutex();
assert(typeof Atomics.Mutex === "function" && typeof Atomics.Condition === "function", "classes exist");
assert(!Object.keys(Atomics).includes("Mutex"), "not enumerable");

let token = Atomics.Mutex.lock(m);
assert(token instanceof Atomics.Mutex.UnlockToken, "token class");
assert(deadlocks(() => Atomics.Mutex.lockIfAvailable(m, 0)), "a mutex that this agent holds is a deadlock");
token.unlock();
token.unlock(); // a second unlock does nothing
let again = Atomics.Mutex.lockIfAvailable(m, 0);
assert(again !== null, "free after unlock");
again.unlock();

{
    using t = Atomics.Mutex.lock(m);
    assert(deadlocks(() => Atomics.Mutex.lockIfAvailable(m, 0)), "held inside using");
}
let free = Atomics.Mutex.lockIfAvailable(m, 0);
assert(free !== null, "released by using");
free.unlock();

// lockIfAvailable with a timeout that passes while the mutex is held.
let held = Atomics.Mutex.lock(m);
assert(deadlocks(() => Atomics.Mutex.lockIfAvailable(m, 10)), "lockIfAvailable with a timeout is a deadlock too, it does not wait");
assert(held.unlock() === true, "unlock reports a release");
assert(held.unlock() === false, "the second unlock reports nothing");

// Tokens: empty on construction, filled by lock and lockIfAvailable, and reusable.
const reusable = new Atomics.Mutex.UnlockToken();
assert(reusable.locked === false, "a new token is empty");
assert(Atomics.Mutex.lock(m, reusable) === reusable, "lock fills the token it is given");
assert(reusable.locked === true, "now it holds the mutex");
assert(deadlocks(() => Atomics.Mutex.lockIfAvailable(m, 0)), "the mutex is held through the token");
let twice = false;
try { Atomics.Mutex.lock(m, reusable); } catch (e) { twice = e instanceof TypeError; }
assert(twice, "a locked token cannot be passed to lock again");
twice = false;
try { Atomics.Mutex.lockIfAvailable(m, 0, reusable); } catch (e) { twice = e instanceof TypeError; }
assert(twice, "a locked token cannot be passed to lockIfAvailable");
assert(reusable[Symbol.dispose]() === undefined, "dispose returns undefined");
assert(reusable.locked === false, "dispose released it");
assert(Atomics.Mutex.lockIfAvailable(m, 0, reusable) === reusable, "the same token locks again");
reusable.unlock();
for (const notAToken of [{}, null, 1, m]) {
    let rejected = false;
    try { Atomics.Mutex.lock(m, notAToken); } catch (e) { rejected = e instanceof TypeError; }
    assert(rejected, "lock rejects a second argument that is not an unlock token");
}
let ctor = false;
try { Atomics.Mutex.UnlockToken(); } catch (e) { ctor = e instanceof TypeError; }
assert(ctor, "UnlockToken needs new");

for (const bad of [{}, null, 1, new Atomics.Condition()]) {
    let t = false;
    try { Atomics.Mutex.lock(bad); } catch (e) { t = e instanceof TypeError; }
    assert(t, "lock rejects a non-mutex");
}
let t2 = false;
try { Atomics.Mutex(); } catch (e) { t2 = e instanceof TypeError; }
assert(t2, "call without new");
assert(!Object.isExtensible(m), "mutex is sealed");

// Replacing Atomics.Mutex does not change the prototype of the objects the engine makes for shared states.
{
    const saved = Atomics.Mutex;
    const mm = new saved();
    Atomics.Mutex = Object;
    const token = saved.lock(mm);
    assert(token instanceof saved.UnlockToken, "tokens keep their prototype after Atomics.Mutex is replaced");
    token.unlock();
    Atomics.Mutex = saved;
}
