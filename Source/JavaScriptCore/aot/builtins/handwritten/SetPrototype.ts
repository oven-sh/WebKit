export function forEach(callback /*, thisArg */) {
    "use strict";

    if (!$isSet(this))
        $throwTypeError("Set operation called on non-Set object");

    if (!$isCallable(callback))
        $throwTypeError("Set.prototype.forEach callback must be a function");

    const thisArg = $argument(1);
    let table = $collectionTable(this);
    let entry = 0;
    for (;;) {
        const step = $setNext(this, table, entry);
        const hasEntry = $resultAt(step, $setNextResultHasEntry);
        entry = $resultAt(step, $setNextResultEntry);
        const key = $resultAt(step, $setNextResultKey);
        if (!hasEntry)
            return;
        table = $collectionTable(this);
        callback.$call(thisArg, key, key, this);
        entry++;
    }
}
