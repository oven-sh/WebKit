//@ requireOptions("--useJSThreads=1", "--verifyConcurrentButterfly=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10")
//@ threadsRequireGILOff
// The JIT's put_by_val_direct slow path reads the target's vector and public
// lengths to update the array profile. Array.from stores that way into
// whatever its constructor returns. Another thread pushes to and shrinks that
// array at the same time. Its first push past the vector makes the word
// segmented, and the profile read must not decode that word as flat storage.
load("../harness.js", "caller relative");

const MAX_LENGTH = 40;
const shared = { target: null, round: 0, stop: 0 };

const writer = new Thread(() => {
    let seen = 0;
    let step = 0;
    while (Atomics.load(shared, "stop") === 0) {
        const round = Atomics.load(shared, "round");
        if (round === seen)
            continue;
        seen = round;
        const target = shared.target;
        for (let i = 0; i < 32; ++i) {
            ++step;
            if (target.length < MAX_LENGTH - 4)
                target.push(100 + step % 100, 100 + (step + 1) % 100);
            if (step % 7 === 0)
                target.length = step % 3;
        }
    }
});

let target = null;
function Ctor() { return target; }
const source = [7, 8, 9];

const deadline = Date.now() + 700;
let calls = 0;
for (let round = 1; Date.now() < deadline; ++round) {
    // Not a constant literal: a copy-on-write array would not segment.
    target = [round % 100, 1];
    shared.target = target;
    Atomics.store(shared, "round", round);
    for (let i = 0; i < 20; ++i) {
        const result = Array.from.call(Ctor, source);
        ++calls;
        if (result !== target)
            throw new Error("Array.from did not return the constructed array");
        if (target.length > MAX_LENGTH)
            throw new Error("length out of range: " + target.length);
        for (let j = 0; j < target.length; ++j) {
            const value = target[j];
            if (value !== undefined && !(Number.isInteger(value) && value >= 0 && value < 200))
                throw new Error("target[" + j + "] is a value nobody wrote: " + String(value));
        }
    }
}
Atomics.store(shared, "stop", 1);
writer.join();
shouldBeTrue(calls > 0, "reader made progress");
