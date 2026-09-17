//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// SPEC-ungil §N.6, tenth round. GIL off, typed-array sort() works on a private
// copy (any view can be reached from another thread). main guarantees that an
// already sorted input of any size is returned untouched without allocating
// (JSTests/stress/typedarray-sort-out-of-memory.js sorts a 4 GB zero-filled
// view); GIL off the copy came first, so that sort threw OutOfMemory. A
// read-only scan with relaxed lane loads now precedes the copy for views long
// enough to take main's radix path. This test checks what the scan must not
// break, for every element type with lanes of two bytes or more, on views just
// below and above the scan's thresholds, plain and shared:
//  - sorted input stays as it is; input with one inversion anywhere (first
//    pair, last pair, middle) is sorted; NaN is an inversion even when the
//    numbers around it are in order, and ends up last; -0 sorts before +0;
//  - a sort that races another thread's writes to the same shared view
//    returns, and afterwards every lane holds a value some thread wrote.
load("../harness.js", "caller relative");

const kinds = [
    [Int16Array, 128, false], [Uint16Array, 128, false], [Float16Array, 128, true],
    [Int32Array, 512, false], [Uint32Array, 512, false], [Float32Array, 512, true],
    [Float64Array, 8192, true], [BigInt64Array, 8192, false], [BigUint64Array, 8192, false],
].filter(k => typeof k[0] === "function");

function isSortedTyped(a, isFloat) {
    for (let i = 1; i < a.length; ++i) {
        const p = a[i - 1], c = a[i];
        if (isFloat) {
            if (p !== p) { if (c === c) return "NaN before a number at " + i; continue; }
            if (c !== c) continue;
            if (p === 0 && c === 0 && Object.is(p, 0) && Object.is(c, -0)) return "+0 before -0 at " + i;
        }
        if (p > c) return "inversion at " + i;
    }
    return null;
}

for (const [Type, threshold, isFloat] of kinds) {
    const big = Type === BigInt64Array || Type === BigUint64Array;
    const v = x => big ? BigInt(x) : x;
    for (const length of [threshold - 1, threshold, threshold * 2 + 3]) {
        for (const shared of [false, true]) {
            const make = () => shared ? new Type(new SharedArrayBuffer(length * Type.BYTES_PER_ELEMENT)) : new Type(length);
            const fillAscending = a => { for (let i = 0; i < length; ++i) a[i] = v(i >> 2); return a; }; // runs of equal values
            const name = Type.name + "[" + length + (shared ? ", shared" : "") + "]";

            let a = fillAscending(make());
            const before = Array.from(a);
            a.sort();
            for (let i = 0; i < length; ++i) {
                if (a[i] !== before[i])
                    throw new Error(name + ": sorted input changed at " + i);
            }
            for (const at of [0, length - 2, length >> 1]) {
                a = fillAscending(make());
                const t = a[at]; a[at] = v(Number(a[at + 1]) + 1); a[at + 1] = t; // one inversion (values stay in range)
                a.sort();
                const bad = isSortedTyped(a, isFloat);
                if (bad)
                    throw new Error(name + ": one inversion at " + at + " left " + bad);
            }
            if (isFloat) {
                a = fillAscending(make());
                a[length >> 1] = NaN;
                a.sort();
                if (a[length - 1] === a[length - 1])
                    throw new Error(name + ": NaN did not end up last");
                const bad = isSortedTyped(a, true);
                if (bad)
                    throw new Error(name + ": with a NaN: " + bad);
                a = make();
                a[0] = 0; a[1] = -0; // the rest is +0
                a.sort();
                if (!Object.is(a[0], -0))
                    throw new Error(name + ": -0 did not sort before +0");
            }
        }
    }
}

// A sort racing writers: terminates, and every lane afterwards holds a written value.
{
    const N = 4096;
    const view = new Int32Array(new SharedArrayBuffer(N * 4));
    for (let i = 0; i < N; ++i)
        view[i] = i;
    const flag = { stop: 0 };
    const writer = new Thread(() => {
        let n = 0;
        while (!Atomics.load(flag, "stop")) {
            view[(n * 7919) % N] = (n & 1) ? N + (n % N) : (n % N);
            ++n;
            if (!(n & 1023))
                sleepMs(0);
        }
        return n;
    });
    for (let round = 0; round < 300; ++round)
        view.sort();
    Atomics.store(flag, "stop", 1);
    writer.join();
    for (let i = 0; i < N; ++i) {
        const x = view[i];
        if (!(x >= 0 && x < 2 * N))
            throw new Error("lane " + i + " holds " + x + ", which no thread wrote");
    }
}
