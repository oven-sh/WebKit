//@ requireOptions("--useJSThreads=1")
// A thread parked in a contended lock.hold behind a long-lived holder must not
// delay unrelated thread completions. The completion sequence yields until
// every open "release -> resumed" park window closes (bounded by a 500ms
// progress deadline, park-no-microtask-drain.js); that window must open at
// the holder's release, not at park entry, or every completion while the
// contender merely waits pays the full deadline.
//
// Before the fix each trivial join below took 500ms; the two together now
// take a few milliseconds.
load("../harness.js", "caller relative");

const lock = new Lock();
const box = { holderIn: 0, parkerIn: 0 };

const holder = new Thread(() => lock.hold(() => {
    box.holderIn = 1;
    sleepMs(1000); // hold long enough for both completions below to happen while the parker waits
    return "held";
}));
waitUntil(() => box.holderIn === 1);

const parker = new Thread(() => {
    box.parkerIn = 1;
    return lock.hold(() => "parked-then-held");
});
waitUntil(() => box.parkerIn === 1);
sleepMs(50); // let the parker reach its contended park

const start = Date.now();
shouldBe(new Thread(() => 1).join(), 1);
shouldBe(new Thread(() => 2).join(), 2);
const elapsed = Date.now() - start;
if (elapsed >= 500)
    throw new Error("unrelated thread completions stalled " + elapsed + "ms behind a parked contended hold");

shouldBe(holder.join(), "held");
shouldBe(parker.join(), "parked-then-held");
