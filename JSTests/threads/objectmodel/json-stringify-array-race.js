//@ requireOptions("--useJSThreads=1", "--verifyConcurrentButterfly=1")
//@ threadsRequireGILOff
// JSON.stringify has fast paths for Int32 and Contiguous arrays that read the
// elements straight from the butterfly. Another thread changes the array while
// it is stringified: it pushes, pops, shrinks, and turns the array from Int32
// into Double or Contiguous. A foreign push past the vector also makes the word
// segmented. The fast paths must read one flat butterfly, or give up.
load("../harness.js", "caller relative");

const MAX_LENGTH = 40;
const shared = { array: null, round: 0, stop: 0 };

// Every value the writer stores. A hole or a value past a racing shrink
// becomes null.
function isWriterValue(value) {
    if (value === null || value === "m")
        return true;
    if (typeof value === "string")
        return /^s\d+$/.test(value);
    if (typeof value !== "number")
        return false;
    return (Number.isInteger(value) && value >= 0) || value % 1 === 0.5;
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
        const strings = typeof array[0] === "string";
        for (let i = 0; i < 48; ++i) {
            ++step;
            if (array.length < MAX_LENGTH - 4)
                array.push(strings ? "s" + step : step, strings ? "s" + (step + 1) : step + 1);
            if (step % 5 === 0)
                array.pop();
            if (step % 9 === 0)
                array.length = step % 4;
            if (step % 13 === 0)
                array.length = Math.min(array.length + 2, MAX_LENGTH);
            if (step % 23 === 0 && array.length)
                array[0] = step + 0.5;
            if (step % 41 === 0 && array.length)
                array[array.length - 1] = "m";
        }
    }
});

// A constant literal is copy-on-write, and the writer's first push gives it a
// flat butterfly that the writer owns, so it stays flat. The other literals
// segment on the writer's first push past the vector.
function makeArray(round) {
    switch (round & 3) {
    case 0:
        return ["s0", "s1", "s2"];
    case 1:
        return ["s" + round, "s1", "s2"];
    case 2:
        return [0, 1, 2];
    default:
        return [round, 1, 2];
    }
}

const deadline = Date.now() + 700;
let calls = 0;
for (let round = 1; Date.now() < deadline; ++round) {
    const array = makeArray(round);
    shared.array = array;
    Atomics.store(shared, "round", round);
    for (let i = 0; i < 20; ++i) {
        const text = (i & 1) ? JSON.stringify(array, null, 1) : JSON.stringify({ array }).slice(9, -1);
        ++calls;
        const parsed = JSON.parse(text);
        if (!Array.isArray(parsed) || parsed.length > MAX_LENGTH)
            throw new Error("JSON.stringify gave a bad array: " + text.slice(0, 80));
        for (let j = 0; j < parsed.length; ++j) {
            if (!isWriterValue(parsed[j]))
                throw new Error("JSON.stringify produced a value nobody wrote: [" + j + "] = " + JSON.stringify(parsed[j]));
        }
    }
}
Atomics.store(shared, "stop", 1);
writer.join();
shouldBeTrue(calls > 0, "reader made progress");
