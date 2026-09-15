function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`bad value: expected ${expected} but got ${actual}`);
}

// The microtask queue keeps its tasks in fixed-size segments: 409 tasks each with Bun's QueuedTask, 511
// without. These depths stay inside one segment, end exactly at a segment boundary, or cross some.
const depths = [0, 1, 2, 3, 100, 408, 409, 410, 510, 511, 512, 817, 818, 819, 1021, 1022, 1023, 1227, 1228, 5000];

const settled = Promise.resolve();

// Tasks run in the order in which they were queued, and the queue is usable again after it drains.
for (const depth of depths) {
    const order = [];
    for (let i = 0; i < depth; ++i)
        settled.then(() => { order.push(i); });
    drainMicrotasks();
    shouldBe(order.length, depth);
    for (let i = 0; i < depth; ++i)
        shouldBe(order[i], i);
}

// Every job queues its successor, so the queue keeps `depth` tasks while it moves forward through the
// segments and frees them behind it. Only the queued task refers to each payload: a task that the
// collector does not visit comes back with a dead payload.
function walk(depth, jobsPerChain, collect)
{
    let ran = 0;
    function makePayload(index, remaining)
    {
        return { index, remaining, text: `job ${index}`, filler: new Array(8).fill(index) };
    }
    function step(payload)
    {
        shouldBe(payload.index, ran);
        shouldBe(payload.text, `job ${ran}`);
        shouldBe(payload.filler.length, 8);
        shouldBe(payload.filler[7], ran);
        ++ran;
        if (collect)
            collect(ran);
        if (payload.remaining)
            Promise.resolve(makePayload(payload.index + depth, payload.remaining - 1)).then(step);
    }
    for (let chain = 0; chain < depth; ++chain)
        Promise.resolve(makePayload(chain, jobsPerChain - 1)).then(step);
    drainMicrotasks();
    shouldBe(ran, depth * jobsPerChain);
}

for (const depth of [1, 2, 7, 408, 409, 410, 511, 512, 1300])
    walk(depth, Math.ceil(4000 / depth), null);

// Collections in the middle of a drain: full and eden, at segment boundaries and between them.
walk(1, 400, (ran) => { if (!(ran % 50)) fullGC(); });
walk(5, 300, (ran) => { if (!(ran % 101)) edenGC(); });
walk(409, 8, (ran) => { if (!(ran % 409)) fullGC(); else if (!(ran % 200)) edenGC(); });
walk(511, 8, (ran) => { if (!(ran % 511)) fullGC(); else if (!(ran % 250)) edenGC(); });
walk(1000, 5, (ran) => { if (!(ran % 777)) gc(); });

// A deep queue that is collected while it is full, while it drains, and while jobs add tasks to it.
{
    const depth = 20000;
    let ran = 0;
    let late = 0;
    for (let i = 0; i < depth; ++i) {
        Promise.resolve({ index: i, filler: new Array(4).fill(i) }).then((payload) => {
            shouldBe(payload.index, ran);
            shouldBe(payload.filler[3], ran);
            ++ran;
            if (!(ran % 4999))
                fullGC();
            if (!(ran % 1000)) {
                Promise.resolve({ queuedAt: ran, filler: new Array(4).fill(ran) }).then((payload) => {
                    shouldBe(ran, depth);
                    shouldBe(payload.filler[0], payload.queuedAt);
                    ++late;
                });
            }
        });
    }
    fullGC();
    edenGC();
    drainMicrotasks();
    shouldBe(ran, depth);
    shouldBe(late, depth / 1000);
}
