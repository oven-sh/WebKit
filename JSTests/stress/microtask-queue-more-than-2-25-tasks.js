//@ skip if $memoryLimited
//@ slow!
//@ runDefault

// The microtask queue was a WTF::Deque, and the 2^25th pending task made it grow to a capacity that is
// not valid for a Vector, which aborts the process. This needs about 3 GB.

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

// The queue works again after it gave all of that back.
let order = "";
for (let i = 0; i < 5; ++i)
    settled.then(() => { order += i; });
drainMicrotasks();
shouldBe(order, "01234");
