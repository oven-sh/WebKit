//@ requireOptions("--useJSThreads=1")
// JSON.parse keeps the property keys it has seen in a small table on the VM,
// so a document with the same keys again is cheaper to parse. Several
// threads parse at once. Each thread uses its own keys, chosen so that they
// land in the same slots of that table as the other threads' keys, so every
// parse replaces what another thread stored. A parse must still produce
// exactly the keys and values of its own text.
load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 300;
const KEYS = 64;

// Keys of one length with the same first and last character share a slot;
// the middle differs per thread and per key.
function keyFor(t, i) {
    const middle = String.fromCharCode(97 + t) + i.toString(36).padStart(3, "0");
    return "k" + middle + "z";
}

const texts = [];
const expectedKeys = [];
for (let t = 0; t < THREADS; ++t) {
    const object = {};
    const keys = [];
    for (let i = 0; i < KEYS; ++i) {
        const key = keyFor(t, i);
        keys.push(key);
        object[key] = key + ":" + t;
    }
    // An array of such objects, so that one parse fills the table many times.
    const list = [];
    for (let j = 0; j < 8; ++j)
        list.push(object);
    texts.push(JSON.stringify(list));
    expectedKeys.push(keys);
}

function work(t) {
    const text = texts[t];
    const keys = expectedKeys[t];
    let parsed = 0;
    for (let round = 0; round < ROUNDS; ++round) {
        const list = JSON.parse(text);
        for (const object of list) {
            const got = Object.keys(object);
            if (got.length !== KEYS)
                throw new Error("thread " + t + ": " + got.length + " keys");
            for (let i = 0; i < KEYS; ++i) {
                if (got[i] !== keys[i])
                    throw new Error("thread " + t + ": key " + i + " is " + got[i] + ", expected " + keys[i]);
                if (object[keys[i]] !== keys[i] + ":" + t)
                    throw new Error("thread " + t + ": value of " + keys[i] + " is " + object[keys[i]]);
            }
        }
        ++parsed;
    }
    return parsed;
}

const threads = [];
for (let t = 1; t < THREADS; ++t)
    threads.push(new Thread(work.bind(null, t)));
shouldBe(work(0), ROUNDS, "parses on the main thread");
for (const thread of threads)
    shouldBe(thread.join(), ROUNDS, "parses on a thread");
