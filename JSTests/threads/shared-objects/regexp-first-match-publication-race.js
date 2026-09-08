//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// GIL off, RegExp::compileIfNecessary reads "has code for this width"
// lock-free and takes the RegExp's cell lock only to compile (seventh landing
// round; before, every match took the lock - 27 M times in one JetStream
// test). The lock-free read is sound only because the compile publishes its
// code pointers before the fenced state store and the reader fences after it.
// Part 1 races the FIRST matches of freshly created RegExps from eight
// threads, 8-bit and 16-bit subjects interleaved (the second width compiles
// into the same YarrCodeBlock while the first is in use), and checks every
// result. Part 2 checks that a compiled RegExp's matches no longer take the
// lock (the diagnostic counter counts lock acquisitions on this path).
load("../harness.js", "caller relative");

const THREADS = 8, ROUNDS = 60;
function makeBatch(round) {
    const res = [];
    for (let i = 0; i < 12; ++i) res.push(new RegExp("a(b+)" + ((round * 12 + i) % 97) + "c|(x)yz" + i)); // no /g: a lastIndex shared by eight threads would be a program-level race
    return res;
}
const sab = new SharedArrayBuffer(8); const ctl = new Int32Array(sab);
const batches = []; for (let r = 0; r < ROUNDS; ++r) batches.push(makeBatch(r));

function worker(tid) {
    let checked = 0;
    for (let r = 0; r < ROUNDS; ++r) {
        while (Atomics.load(ctl, 0) < r) { } // all threads start round r together
        const res = batches[r];
        for (let k = 0; k < res.length; ++k) {
            const i = (k + tid) % res.length; // different threads hit different regexps first
            const n = (r * 12 + i) % 97;
            const s8 = "..a" + "b".repeat(1 + (i % 3)) + n + "c..";
            const s16 = "Āāa" + "b".repeat(1 + (i % 3)) + n + "cĂ";
            const useWide = ((tid + k) & 1) === 1;
            const m = res[i].exec(useWide ? s16 : s8);
            if (!m || m[1] !== "b".repeat(1 + (i % 3))) throw new Error("thread " + tid + " round " + r + " regexp " + i + ": bad match " + JSON.stringify(m));
            if (!res[i].test(useWide ? s8 : s16)) throw new Error("thread " + tid + " round " + r + " regexp " + i + ": test() false on the other width");
            checked += 2;
        }
        if (tid === 0) Atomics.store(ctl, 0, r + 1); else while (Atomics.load(ctl, 0) < r + 1 && r + 1 < ROUNDS) { }
    }
    return checked;
}
const threads = []; for (let t = 1; t < THREADS; ++t) threads.push(new Thread(worker.bind(null, t)));
const mine = worker(0);
let total = mine; for (const t of threads) total += t.join();
if (total !== THREADS * ROUNDS * 12 * 2) throw new Error("checked " + total);

// Part 2: steady-state matches of compiled RegExps take no lock GIL off.
const re = /q(\d+)r/; const subj = "xxq12345rxx";
for (let i = 0; i < 1000; ++i) re.exec(subj);
const locked0 = $vm.jsThreadsCounter("regExpCompileCheckLocked");
let hits = 0;
for (let i = 0; i < 200000; ++i) if (re.exec(subj)) hits++;
const locked = $vm.jsThreadsCounter("regExpCompileCheckLocked") - locked0;
if (hits !== 200000) throw new Error("hits " + hits);
if (typeof AMPLIFY_VERBOSE !== "undefined") print("cell-lock acquisitions for 200000 compiled matches: " + locked);
if (locked > 100) throw new Error(locked + " of 200000 matches of a compiled RegExp took its cell lock");
print("PASS");
