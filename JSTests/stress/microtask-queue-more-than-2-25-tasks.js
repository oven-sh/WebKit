//@ skip if $memoryLimited
//@ slow!
//@ runDefault

// The microtask queue is a WTF::Deque. The 2^25th pending task makes it grow to 2^26 tasks, which is more
// than the 2^31 - 1 bytes a Vector buffer can have, and that aborted the process. This needs about 4.5 GB.

function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`bad value: expected ${expected} but got ${actual}`);
}

const count = 2 ** 25 + 1;
const settled = Promise.resolve();
let ran = 0;
const job = () => { ++ran; };
for (let i = 0; i < count; ++i)
    settled.then(job);
drainMicrotasks();
shouldBe(ran, count);

// The queue still works after it grew that far.
let order = "";
for (let i = 0; i < 5; ++i)
    settled.then(() => { order += i; });
drainMicrotasks();
shouldBe(order, "01234");
