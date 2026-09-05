//@ requireOptions("--useJSThreads=1")
// String.raw turns number elements of the `raw` array into strings through a
// number-to-string table on the VM. Several threads call it at once, each with
// its own numbers, so every call replaces entries that another thread is
// reading. The result must still be the text of the caller's own numbers.
load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 400;
const COUNT = 96;

const raws = [];
const expected = [];
for (let t = 0; t < THREADS; ++t) {
    const ints = [];
    const doubles = [];
    for (let i = 0; i < COUNT; ++i) {
        // Above the small-integer table, and spread so that threads collide.
        ints.push(5000 + i * 1024 + t);
        doubles.push(0.5 + i * 1024 + t);
    }
    raws.push([{ raw: ints }, { raw: doubles }]);
    expected.push([ints.join(""), doubles.join("")]);
}

function work(t) {
    const [intsRaw, doublesRaw] = raws[t];
    const [intsText, doublesText] = expected[t];
    let done = 0;
    for (let round = 0; round < ROUNDS; ++round) {
        const a = String.raw(intsRaw);
        if (a !== intsText)
            throw new Error("thread " + t + ": integers came out as " + a.slice(0, 40) + "...");
        const b = String.raw(doublesRaw);
        if (b !== doublesText)
            throw new Error("thread " + t + ": doubles came out as " + b.slice(0, 40) + "...");
        ++done;
    }
    return done;
}

const threads = [];
for (let t = 1; t < THREADS; ++t)
    threads.push(new Thread(work.bind(null, t)));
shouldBe(work(0), ROUNDS, "rounds on the main thread");
for (const thread of threads)
    shouldBe(thread.join(), ROUNDS, "rounds on a thread");
