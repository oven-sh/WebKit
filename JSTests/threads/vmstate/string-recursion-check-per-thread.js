//@ requireOptions("--useJSThreads=1")
// The Array.prototype.join/toString recursion check tracks one thread's call
// stack. A thread parked inside a stringification (its element's toString
// blocks on a contended Lock, dropping the GIL) must not make the object it
// is stringifying look like a cycle to another thread that stringifies the
// same object meanwhile: that thread gets the real string, not "".
load("../resources/assert.js", "caller relative");

const lock = new Lock();
const cond = new Condition();
let state = 0;
let parkedOnce = false;

const elem = {
    toString() {
        if (!parkedOnce) {
            parkedOnce = true;
            lock.hold(() => {
                state = 1;
                cond.notifyAll();
                while (state !== 2)
                    cond.wait(lock);
            });
        }
        return "elem";
    }
};
const shared = [elem];

const thread = new Thread(() => shared.join());

// Wait until the spawned thread is parked inside shared.join().
lock.hold(() => {
    while (state !== 1)
        cond.wait(lock);
});

shouldBe(shared.join(), "elem");
shouldBe(String(shared), "elem");
shouldBe([shared, elem].join("|"), "elem|elem");

// A genuine cycle on this thread is still detected while the other thread
// is parked.
const cyclic = [1];
cyclic.push(cyclic);
shouldBe(cyclic.join(), "1,");

lock.hold(() => {
    state = 2;
    cond.notifyAll();
});

shouldBe(thread.join(), "elem");
