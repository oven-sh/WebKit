//@ requireOptions("--useJSThreads=1")
// Flag-on, shift/splice on an ArrayStorage array compacts into a fresh AS
// butterfly (shiftCountWithArrayStorageConcurrent). Every (startIndex, count)
// the generic caller can hand it must produce an exact result, including
// ranges that end at the last element and empty ranges, and the array must
// stay usable for further shifts and splices afterwards.

function makeArrayStorage(n)
{
    const a = [];
    a[100000] = 0;
    delete a[100000];
    a.length = 0;
    for (let i = 0; i < n; ++i)
        a[i] = i;
    return a;
}

function expectArray(actual, expected, what)
{
    if (actual.length !== expected.length)
        throw new Error(what + ": length " + actual.length + ", expected " + expected.length);
    for (let i = 0; i < expected.length; ++i) {
        if (actual[i] !== expected[i])
            throw new Error(what + ": [" + i + "] = " + actual[i] + ", expected " + expected[i]);
        if (!(i in actual))
            throw new Error(what + ": hole at " + i);
    }
    if (Object.keys(actual).length !== expected.length)
        throw new Error(what + ": key count " + Object.keys(actual).length + ", expected " + expected.length);
}

for (let n = 0; n <= 40; n += 4) {
    for (let start = 0; start <= n; ++start) {
        for (let count = 0; start + count <= n; ++count) {
            const a = makeArrayStorage(n);
            const removed = a.splice(start, count);
            const expected = [];
            const expectedRemoved = [];
            for (let i = 0; i < n; ++i) {
                if (i >= start && i < start + count)
                    expectedRemoved.push(i);
                else
                    expected.push(i);
            }
            const what = "splice(" + start + ", " + count + ") on " + n;
            expectArray(removed, expectedRemoved, what + " removed");
            expectArray(a, expected, what);

            // The compacted storage must still shift and splice exactly.
            const first = a.shift();
            if (expected.length) {
                if (first !== expected[0])
                    throw new Error(what + " then shift: got " + first + ", expected " + expected[0]);
                expectArray(a, expected.slice(1), what + " then shift");
            } else if (first !== undefined)
                throw new Error(what + " then shift on empty: got " + first);
            a.splice(0, a.length);
            expectArray(a, [], what + " then splice all");
        }
    }
}

// Repeated shifts drain the array down to empty without leaving stale values.
{
    const a = makeArrayStorage(64);
    for (let i = 0; i < 64; ++i) {
        const v = a.shift();
        if (v !== i)
            throw new Error("shift #" + i + " returned " + v);
        if (a.length !== 63 - i)
            throw new Error("length after shift #" + i + " is " + a.length);
    }
    if (a.shift() !== undefined || a.length !== 0)
        throw new Error("shift on drained array");
}
