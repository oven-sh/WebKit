//@ runDefault("--slowPathAllocsBetweenGCs=1")
//@ runDefault("--slowPathAllocsBetweenGCs=1", "--sweepSynchronously=true", "--useZombieMode=true")

// streamingJSONParse() (Bun.JSONL) reads a key that JSONCache knows and takes the transition of that key from the
// cache as a StructureID. When the value needs out-of-line storage, the allocation of that storage can collect.
// Nothing but JSONCache refers to the new Structure then: the objects that had it are dead, and a transition table is
// weak. So JSONCache has to mark its transitions while the streaming parse runs, as it does for JSON.parse().
//
// --slowPathAllocsBetweenGCs=1 collects at the allocation of the out-of-line storage.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + expected + " but got " + actual);
}

// Six properties are stored inline, so the seventh one allocates.
const keyCount = 8;
const rounds = 20;

function keyOf(round, index) {
    return "r" + round + "k" + index;
}

// Every round has keys of its own, and with them Structures that no other round keeps alive.
function makeText(round) {
    let text = "{";
    for (let i = 0; i < keyCount; ++i)
        text += '"' + keyOf(round, i) + '":' + i + ",";
    // The empty object is the last allocation of a parse. It leaves cells on the free list of its size class, so the
    // object of the next parse is allocated without a collection, and the first collection of the streaming parse
    // is the one at the out-of-line storage.
    return text + '"z":{}}';
}

const keep = [null, null, null];

// The first parse makes the Structures, the second one gives JSONCache their names, and the third one reads them.
// The three objects keep the Structures alive until then. After that, nothing does.
function warmUp(text) {
    keep[0] = JSON.parse(text);
    keep[1] = JSON.parse(text);
    keep[2] = JSON.parse(text);
    keep[0] = null;
    keep[1] = null;
    keep[2] = null;
}
noInline(warmUp);

function check(round, object) {
    for (let i = 0; i < keyCount; ++i)
        shouldBe(object[keyOf(round, i)], i);
    const keys = Object.keys(object);
    shouldBe(keys.length, keyCount + 1);
    for (let i = 0; i < keyCount; ++i)
        shouldBe(keys[i], keyOf(round, i));
    shouldBe(keys[keyCount], "z");
    shouldBe(Object.keys(object.z).length, 0);
}
noInline(check);

// JSONCache compares a name with 32 characters of the source, so the input goes on behind the object.
const texts = [];
const lines = [];
for (let round = 0; round < rounds; ++round) {
    texts.push(makeText(round));
    lines.push(texts[round] + "\n" + " ".repeat(64));
}
// Flatten the ropes here: nothing may be allocated between the last JSON.parse() and the streaming parse.
for (let round = 0; round < rounds; ++round) {
    texts[round].charCodeAt(0);
    lines[round].charCodeAt(0);
}

for (let round = 0; round < rounds; ++round) {
    warmUp(texts[round]);
    const result = streamingJSONParse(lines[round]);
    shouldBe(result.status, "complete");
    shouldBe(result.values.length, 1);
    check(round, result.values[0]);
}
