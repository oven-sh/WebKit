//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// SPEC-jit history §39 (eighth landing round). GIL off the FTL keeps an
// array's butterfly hoisted across loop polls again; what keeps that safe when
// a foreign thread converts the array to segmented storage and grows it under
// the loop (the old flat header's publicLength then keeps growing while its
// own lanes end at its frozen vectorLength, SPEC-objectmodel I9b) is that
// every bound the FTL takes through the hoisted storage is clamped to that
// storage's vectorLength, and pop/shift on a stale storage go to the runtime.
// (1) A reader sums a shared array in FTL-hot loops (in-bounds and
// out-of-bounds-tolerant forms, indexOf, includes, slice) while its owner and
// a third thread push, pop, truncate and store past the end; every element read
// is a value some thread stored (a tagged small integer) or undefined, never garbage,
// and nothing faults. (2) The owner's own loops over an array only it touches
// get the same sums GIL off as GIL on (the bound costs nothing observable).
// cve/mc-jit-stale-base-grow-oob.js stays the regression guard for the
// original out-of-bounds shape.
load("../harness.js", "caller relative");

const TAG = 1 << 20; // elements are small tagged integers k*TAG + j (k = writer, j = an index); anything else is a torn or stray read
function ok(v, i) { return v === undefined || (typeof v === "number" && (v | 0) === v && v >= TAG && v < 8 * TAG); }

const shared = []; for (let i = 0; i < 64; ++i) shared.push(i + TAG);
const box = { stop: 0 };

function sumInBounds(a) { let s = 0; for (let i = 0; i < a.length; ++i) { const v = a[i]; if (!ok(v, i)) return "bad " + i + ":" + String(v); s += v === undefined ? 0 : 1; } return s; }
function sumFixed(a, n) { let s = 0; for (let i = 0; i < n; ++i) { const v = a[i]; if (!ok(v, i)) return "bad " + i + ":" + String(v); s += v === undefined ? 0 : 1; } return s; } // reads past the end are allowed (undefined)
function probe(a) { const i = a.indexOf(5 + TAG); const h = a.includes(7 + 2 * TAG); const c = a.slice(0, 8); for (let k = 0; k < c.length; ++k) if (!ok(c[k], k)) return "bad slice " + k + ":" + String(c[k]); return (i === -1 || i === 5) && typeof h === "boolean" ? null : "bad probe " + i + " " + h; }
noInline(sumInBounds); noInline(sumFixed); noInline(probe);

function reader() {
    let bad = null, rounds = 0;
    for (; rounds < 20000 && !bad && !Atomics.load(box, "stop"); ++rounds) {
        const a = sumInBounds(shared); if (typeof a === "string") bad = a;
        const b = sumFixed(shared, 200); if (typeof b === "string") bad = b;
        bad = bad || probe(shared);
    }
    return bad || ("ok " + rounds);
}
function mutator(k) {
    // Grows the shared array (segmented conversion for the foreign thread, then
    // fragment growth), stores tagged values in place, pops and truncates now and then.
    let n = 0;
    for (; n < 4000 && !Atomics.load(box, "stop"); ++n) {
        shared.push(shared.length + k * TAG);
        const i = n % (shared.length || 1); shared[i] = i + k * TAG;
        if (!(n % 50) && shared.length > 70) shared.pop();
        if (shared.length > 3000) { shared.length = 64; for (let j = 0; j < 64; ++j) shared[j] = j + k * TAG; }
    }
    return n;
}
const readers = [new Thread(reader), new Thread(reader)];
const foreign = new Thread(() => mutator(3));
const mineSteps = mutator(2); // the main thread created `shared`: it is the owner
const foreignSteps = foreign.join();
Atomics.store(box, "stop", 1);
const results = readers.map(t => t.join());
if (typeof AMPLIFY_VERBOSE !== "undefined") print("mutator steps " + mineSteps + "/" + foreignSteps + "; readers: " + results.join(", "));
for (const r of results) if (!String(r).startsWith("ok")) throw new Error("reader saw " + r);

// (2) Private arrays: same answers in every mode.
function kernel(x, w, n) { let c = 0; for (let j = 0; j < n; ++j) { let l = x[j] & 0x3fff, h = x[j] >> 14; let m = h * l; l = l * l + ((m & 0x3fff) << 14) + w[j] + c; c = (l >> 28) + (m >> 14) + h * h; w[j] = l & 0xfffffff; } return c; }
noInline(kernel);
function run() { const x = [], w = []; for (let i = 0; i < 200; ++i) { x.push((i * 2654435761) & 0xfffffff); w.push(0); } let c = 0; for (let r = 0; r < 3000; ++r) c = (c + kernel(x, w, 200)) | 0; let h = c; for (let i = 0; i < 200; ++i) h = (h * 31 + w[i]) | 0; return h; }
const expected = run();
const workers = [new Thread(run), new Thread(run), new Thread(run)];
for (const t of workers) { const got = t.join(); if (got !== expected) throw new Error("kernel " + got + " vs " + expected); }
print("PASS");
