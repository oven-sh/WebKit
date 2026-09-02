//@ requireOptions("--useJSThreads=1")
// A thread parked inside a stringification (its element's toString blocks on a
// contended Lock, dropping the GIL) must not change what another thread gets
// when it stringifies the same object meanwhile: that thread gets the real
// string. Array.prototype.join has no cycle detection, so a cycle recurses
// until the stack is exhausted and throws a RangeError.
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

// A genuine cycle on this thread overflows the stack while the other thread
// is parked.
const cyclic = [1];
cyclic.push(cyclic);
shouldThrow(RangeError, () => cyclic.join());

lock.hold(() => {
    state = 2;
    cond.notifyAll();
});

shouldBe(thread.join(), "elem");
