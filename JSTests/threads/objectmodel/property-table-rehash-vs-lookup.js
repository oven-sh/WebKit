//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// Lookups on several threads probe a property table that another thread
// rebuilds. A lookup reaches the new index vector through the vector word (an
// address dependency), and the writer publishes the word after it allocates
// and zero-fills the vector. Under TSAN the word is an acquire load and a
// release store, so TSAN sees that order. TSAN reported the pairs when it was
// not. Without TSAN, this checks that the lookups read the stored value.

const LIVE = 8;
const CHURN = 200000;
const READERS = 8;

// Deleting a property makes the object a dictionary, and its structure keeps
// the table. The table stays small, and each delete leaves a deleted entry,
// so the writer rebuilds the table many thousands of times.
const object = { seed: 1, gone: 1 };
delete object.gone;
const keys = [];
for (let i = 0; i < 256; ++i)
    keys.push("k" + i);
for (let i = 0; i < LIVE; ++i)
    object[keys[i]] = i;

const state = { stop: false, started: 0 };
const readers = [];
for (let r = 0; r < READERS; ++r) {
    readers.push(new Thread(() => {
        let count = 0;
        Atomics.add(state, "started", 1);
        while (!state.stop) {
            if (object.seed !== 1)
                throw new Error("seed is " + object.seed);
            ++count;
        }
        return count;
    }));
}
while (Atomics.load(state, "started") !== READERS) { }

// A Debug build is much slower, so the churn also stops at a deadline.
const deadline = Date.now() + 10000;
let last = LIVE;
for (; last < LIVE + CHURN; ++last) {
    object[keys[last & 255]] = last;
    delete object[keys[(last - LIVE) & 255]];
    if (!(last & 1023) && Date.now() > deadline) {
        ++last;
        break;
    }
}
state.stop = true;
for (const reader of readers) {
    if (!(reader.join() > 0))
        throw new Error("a reader did not run");
}
for (let i = last - LIVE; i < last; ++i) {
    if (object[keys[i & 255]] !== i)
        throw new Error(keys[i & 255] + " is " + object[keys[i & 255]]);
}
