//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
//@ threadsEnv("GIGACAGE_ENABLED=0")
// GIL-off, a typed array access on one thread can race a detach on another.
// The access loads the view's length, checks the index, and then loads the
// view's base. If the detach cleared the base in between, the access paired
// the old length with a null base and touched address 0 + index. With the
// Gigacage off, that is a crash. The detach must keep the base until the
// next stop, as it does for the ArrayBuffer.

const READERS = 4;
const ROUNDS = 20000;
const LENGTH = 4096;
const PATTERN = 0x5a;

const mailbox = { stop: false, view: null };

function touch(view) {
    const length = view.length;
    if (!length)
        return 0;
    view[length - 1] = PATTERN;
    const value = view[length - 1];
    if (value !== PATTERN && value !== undefined && value !== 0)
        throw new Error("bad value " + value + " at index " + (length - 1));
    return length;
}

const readers = [];
for (let i = 0; i < READERS; ++i) {
    readers.push(new Thread(() => {
        let sum = 0;
        while (!mailbox.stop) {
            const view = mailbox.view;
            if (view)
                sum += touch(view);
        }
        return sum;
    }));
}

for (let round = 0; round < ROUNDS; ++round) {
    const buffer = new ArrayBuffer(LENGTH);
    const view = new Uint8Array(buffer);
    mailbox.view = view;
    for (let spin = 0; spin < 200; ++spin)
        touch(view);
    transferArrayBuffer(buffer);
    if (view.length !== 0)
        throw new Error("a detached view has length " + view.length);
    if (view.byteOffset !== 0)
        throw new Error("a detached view has byte offset " + view.byteOffset);
    if (view[LENGTH - 1] !== undefined)
        throw new Error("a detached view has an element");
}

mailbox.stop = true;
for (const reader of readers)
    reader.join();
