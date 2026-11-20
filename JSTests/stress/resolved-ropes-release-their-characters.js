//@ skip if $asan
//@ runDefault

function resolve(rope) {
    return rope.charCodeAt(rope.length - 1);
}
noInline(resolve);

const half = "a".repeat(128 * 1024);
resolve(half);
gc();
const before = MemoryFootprint().current;

for (let round = 0; round < 16; ++round) {
    // Some are resolved in the cycle that made them, some in a block that has been swept since.
    const ropes = [];
    for (let i = 0; i < 100; ++i)
        ropes.push(half + half + (i % 10));
    for (let i = 0; i < 100; ++i) {
        if (i === 50)
            (round & 1 ? edenGC : fullGC)();
        if (resolve(ropes[i]) !== 48 + i % 10)
            throw new Error("bad contents");
    }
}

gc();
const megabytes = (MemoryFootprint().current - before) / (1024 * 1024);
if (megabytes > 200)
    throw new Error(`${megabytes} of 400 MB are still there`);
