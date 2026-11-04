export function forEach(callback /*, thisArg */) {
    "use strict";

    if (!$isMap(this))
        $throwTypeError("Map operation called on non-Map object");

    if (!$isCallable(callback))
        $throwTypeError("Map.prototype.forEach callback must be a function");

    const thisArg = $argument(1);
    let table = $collectionTable(this);
    let entry = 0;
    for (;;) {
        const step = $mapNext(this, table, entry);
        const hasEntry = $resultAt(step, $mapNextResultHasEntry);
        entry = $resultAt(step, $mapNextResultEntry);
        const key = $resultAt(step, $mapNextResultKey);
        const value = $resultAt(step, $mapNextResultValue);
        if (!hasEntry)
            return;
        table = $collectionTable(this);
        callback.$call(thisArg, value, key, this);
        entry++;
    }
}
