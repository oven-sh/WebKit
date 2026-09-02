//@ requireOptions("--useJSThreads=1", "--verifyConcurrentButterfly=1")
//@ threadsRequireGILOff
// Array.prototype.concat with two or more arguments sizes its result from each
// source's length, allocates, and then copies each source. Another thread
// changes a source while this runs: it pushes, pops, shrinks, and turns the
// array from Int32 into Double or Contiguous. A foreign push past the vector
// also makes the word segmented. The copy must never write past the result,
// store raw doubles as values, or read a segmented word as flat storage.
load("../harness.js", "caller relative");

const MAX_LENGTH = 48;
const shared = { array: null, round: 0, stop: 0 };

// Every value the writer stores, plus the hole that a length increase leaves.
function isWriterValue(value) {
    if (value === undefined || value === "m")
        return true;
    if (typeof value !== "number")
        return false;
    return (Number.isInteger(value) && value >= 0 && value < 1000) || value % 1 === 0.5;
}

const writer = new Thread(() => {
    let seen = 0;
    let step = 0;
    while (Atomics.load(shared, "stop") === 0) {
        const round = Atomics.load(shared, "round");
        if (round === seen)
            continue;
        seen = round;
        const array = shared.array;
        for (let i = 0; i < 64; ++i) {
            ++step;
            if (array.length < MAX_LENGTH - 8)
                array.push(step % 1000, (step + 1) % 1000, (step + 2) % 1000);
            if (step % 7 === 0)
                array.pop();
            if (step % 11 === 0)
                array.length = step % 5;
            if (step % 13 === 0)
                array.length = Math.min(array.length + 3, MAX_LENGTH);
            if (step % 29 === 0 && array.length)
                array[0] = 0.5;
            if (step % 53 === 0 && array.length)
                array[array.length - 1] = "m";
        }
    }
});

const other = [900, 901];
const doubles = [0.5, 1.5];
const deadline = Date.now() + 700;
let calls = 0;
for (let round = 1; Date.now() < deadline; ++round) {
    // A constant literal is copy-on-write, and the writer's first push gives it a
    // flat butterfly that the writer owns, so it stays flat. The other literal
    // segments on the writer's first push past the vector.
    const array = (round & 1) ? [1, 2, 3] : [round % 1000, 2, 3];
    shared.array = array;
    Atomics.store(shared, "round", round);
    for (let i = 0; i < 20; ++i) {
        const result = (i & 1) ? array.concat(other, array, 7) : array.concat(doubles, array);
        ++calls;
        const extra = (i & 1) ? other.length + 1 : doubles.length;
        if (result.length < extra || result.length > extra + 2 * MAX_LENGTH)
            throw new Error("concat length out of range: " + result.length);
        for (let j = 0; j < result.length; ++j) {
            if (!isWriterValue(result[j]))
                throw new Error("concat produced a value nobody wrote: result[" + j + "] = " + String(result[j]));
        }
    }
}
Atomics.store(shared, "stop", 1);
writer.join();
shouldBeTrue(calls > 0, "reader made progress");
