// A conservative root that points at the start of a JSCellButterfly cell must not also mark the JSCellButterfly cell
// that ends there. The storage of a Set is such a cell. The storage of a dead Set stayed alive, with its keys, while
// the Set whose storage was allocated right after it was being iterated.

const kept = [];
const droppedKeys = [];
function allocate(keep) {
    const key = {};
    // A Set allocates its storage on the first add. Consecutive allocations are neighbors in a MarkedBlock.
    const set = new Set([key]);
    if (keep)
        kept.push(set);
    else
        droppedKeys.push(new WeakRef(key));
}
noInline(allocate);
for (let i = 0; i < 200; ++i)
    allocate(i % 2 === 1);

// A WeakRef keeps its target alive until the task that created it ends.
setTimeout(() => {
    // forEach keeps the storage of the Set it iterates in a stack slot. At the deepest call the stack references the
    // storage of every kept Set. Nothing references the storage of a dropped Set.
    let stillLive = -1;
    (function iterateAll(i) {
        if (i === kept.length) {
            gc();
            stillLive = droppedKeys.filter((weakRef) => weakRef.deref() !== undefined).length;
            return;
        }
        kept[i].forEach(() => iterateAll(i + 1));
    })(0);

    // A few keys can survive through an unrelated stack word.
    if (stillLive >= droppedKeys.length / 5)
        throw new Error(`${stillLive} of ${droppedKeys.length} dropped keys are still alive`);
}, 0);
