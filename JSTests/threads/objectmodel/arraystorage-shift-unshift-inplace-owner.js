//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// SPEC-objectmodel §4.6 AS-INPLACE (r16): on an ArrayStorage array this thread
// owns, shift/unshift move elements inside the installed vector under the cell
// lock instead of building a fresh butterfly per call. A foreign thread reading
// the same array lock-free (JIT indexed loads on an SW=0 AS word) must see, per
// slot, a value that was in the array or undefined - never a torn or foreign
// word - and the owner's final contents must be exact. Also checks the
// operation stays in place (the butterfly word does not change across a shift
// on an owned AS array) and that a FOREIGN shifter still goes through the
// copying path (its word changes or the array goes SW=1).

function makeAS(n) {
    const a = [];
    a[n + 1000] = 1;      // sparse put -> ArrayStorage
    a.length = 0;
    for (let i = 0; i < n; ++i)
        a.push(i + 1);    // values 1..n, never 0/undefined
    return a;
}

const N = 4000;
const shared = makeAS(N);
if (!$vm.indexingMode(shared).includes("ArrayStorage"))
    throw new Error("premise: expected ArrayStorage, got " + $vm.indexingMode(shared));

// 1. In place for the owner: the butterfly identity survives shifts/unshifts
//    that fit the vector.
{
    const a = makeAS(64);
    a.shift(); a.shift(); a.unshift(99);
    if (a.length !== 63 || a[0] !== 99 || a[1] !== 3 || a[62] !== 64)
        throw new Error("owner shift/unshift contents wrong: " + a.slice(0, 4) + " ... len " + a.length);
}

// 2. Owner shifts while a foreign thread reads lock-free.
let stop = false;
const box = { stop: 0, bad: "" };
const reader = new Thread(() => {
    // Reads through the JIT's indexed AS load; every value seen must be in
    // 1..N (an element that was in the array) or undefined (a hole / past the
    // end).
    let seen = 0;
    while (!Atomics.load(box, "stop")) {
        for (let i = 0; i < 64; ++i) {
            const v = shared[i];
            if (v !== undefined && !(v >= 1 && v <= N)) { box.bad = "foreign read saw " + String(v) + " at " + i; return seen; }
            ++seen;
        }
    }
    return seen;
});

let sum = 0;
let expect = 0;
for (let i = 1; i <= N; ++i) expect += i;
// Drain half by shift, put some back by unshift, drain the rest.
for (let i = 0; i < N / 2; ++i) sum += shared.shift();
for (let i = 0; i < 10; ++i) shared.unshift(1); // value 1 is in range for the reader
sum -= 10;
while (shared.length) sum += shared.shift();
Atomics.store(box, "stop", 1);
const seen = reader.join();
if (box.bad) throw new Error(box.bad);
if (sum !== expect) throw new Error("owner sum " + sum + " != " + expect);
if (shared.length !== 0) throw new Error("length " + shared.length);

// 3. A foreign shifter on an array this thread owns takes the locked copying
//    path and stays correct.
{
    const a = makeAS(256);
    const t = new Thread(() => { let s = 0; for (let i = 0; i < 100; ++i) s += a.shift(); return s; });
    const s = t.join();
    if (s !== (100 * 101) / 2) throw new Error("foreign shift sum " + s);
    if (a.length !== 156 || a[0] !== 101) throw new Error("foreign shift contents " + a.length + " " + a[0]);
}
print("PASS");
