//@ requireOptions("--useJSThreads=1")
// SPEC-ungil history, eighth landing round ("the known-atom bit and rope
// resolution GIL off"). A JSString carries a header bit saying its impl is
// known to be an atom; JIT code used it to skip the rope and atom checks on an
// impl it had loaded BEFORE testing the bit. GIL off another thread can
// resolve the same rope to an atom between that load and the test, so the
// reader paired rope bits (for a 16-bit substring rope, exactly 0x3) with a
// set bit and dereferenced them as a StringImpl. Each round main publishes
// fresh, unresolved 16-bit substring ropes and the threads, free-running, use
// them at once as keys at a megamorphic get-by-value site (the stub that
// crashed in cve/mc-tear-rope-resolve-race.js) and at a string-constant
// strict-equality site, so whichever thread resolves a rope first races the
// others' fast paths. The window is the resolver's publication landing
// between two adjacent loads of another thread, so a single run meets it
// rarely and longer runs do not meet it more often; the test is short and
// counts over many runs: before the reader rule 8 of 2,400 plain GIL-off
// runs crashed with SIGSEGV in the stub (19 of 7,200 at a quarter of the
// rounds; the cve test 13 of 24,000 amplified runs); after it 0 of 2,400
// and 0 of 9,600 (the cve test 0 of 24,000), and the values always match.
load("../harness.js", "caller relative");

const THREADS = 6;
const ROUNDS = typeof KA_ROUNDS !== "undefined" ? KA_ROUNDS : 600;
const NAMES = [];
for (let i = 0; i < 12; ++i) NAMES.push("kéሴ" + i + "n"); // 16-bit content
const PAD = "ሴpadሴ";

// 24 objects, all holding every name, each with its own structure (different
// insertion orders and a distinguishing extra property): the get-by-value
// site below goes megamorphic.
const objects = [];
for (let j = 0; j < 24; ++j) {
    const o = {};
    o["pre" + j] = j;
    for (let i = 0; i < NAMES.length; ++i) o[NAMES[(i + j) % NAMES.length]] = j * 100 + ((i + j) % NAMES.length);
    objects.push(o);
}

const box = { rec: null, stop: 0, started: 0 };
const gate = { go: 0 };
// GIL off the threads spin hot for the next round (the race needs them on the
// fresh ropes immediately); under the cooperative GIL a spinning thread never
// yields, so there they park briefly instead, and main sleeps between rounds.
const GIL_OFF = typeof jscOptions === "function" && !jscOptions().useThreadGIL;

function work(id) {
    Atomics.add(box, "started", 1);
    let last = -1, rounds = 0, sum = 0, idle = 0;
    while (Atomics.load(box, "stop") === 0) {
        const rec = box.rec;
        if (rec === null || rec.round === last) {
            if (!GIL_OFF || ++idle > 100000) { idle = 0; Atomics.wait(gate, "go", 0, 1); }
            continue;
        }
        idle = 0;
        last = rec.round;
        const keys = rec.keys;
        for (let i = 0; i < keys.length; ++i) {
            const key = keys[i];
            for (let n = 0; n < 4; ++n) {
                const j = (id * 5 + i * 3 + n + last) % objects.length;
                const v = objects[j][key]; // megamorphic get-by-value, rope key
                const want = j * 100 + i;
                if (v !== want) throw new Error("thread " + id + " round " + last + " key " + i + ": got " + v + " want " + want);
                sum += v;
            }
            if (i === 0 && !(key === "kéሴ0n")) throw new Error("thread " + id + " round " + last + ": key 0 !== its constant");
        }
        rounds++;
    }
    return rounds;
}

const threads = [];
for (let t = 0; t < THREADS; ++t) threads.push(new Thread(work.bind(null, t)));
waitUntil(() => Atomics.load(box, "started") === THREADS);

for (let r = 0; r < ROUNDS; ++r) {
    const keys = [];
    for (let i = 0; i < NAMES.length; ++i) {
        const flat = (PAD + NAMES[i] + PAD + r).split("").join(""); // fresh resolved 16-bit base
        keys.push(flat.substring(PAD.length, PAD.length + NAMES[i].length)); // unresolved substring rope
    }
    box.rec = { round: r, keys }; // one publication per round
    if (GIL_OFF) {
        const until = performance.now() + 0.3; // let the threads race on this round's ropes
        while (performance.now() < until) {}
    } else
        sleepMs(1); // hand the GIL to the threads
}
Atomics.store(box, "stop", 1);
let total = 0;
for (const t of threads) total += t.join();
if (typeof AMPLIFY_VERBOSE !== "undefined") print("rounds worked across threads: " + total);
if (total < THREADS) throw new Error("threads did no rounds: " + total);
print("PASS");
