//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// SPEC-jit §5.6 watcher-less fast path (seventh landing round, history §36).
// A Class-A watchpoint set that nobody watches - here the property
// replacement sets that property ICs arm on the structures they cache - is
// fired by flipping its state, not by stopping the world. The workload is the
// shape of the scaling suite's string-heavy inner loop (fresh objects keyed by
// computed strings, read through an IC and then overwritten; a closure
// variable written from a nested function) on five threads. Before: one
// stop-the-world request per (structure, offset) first overwrite that an IC
// had armed - 6,445 in the threaded phase GIL on (GIL off the by-val ICs give
// up sooner and arm few, ~15); after: those fires take the watcher-less path
// (counter) and the stop count is the handful other sources produce. Values
// are checked on every thread in every mode.
load("../harness.js", "caller relative");

function work(seed) {
    // The shape of the scaling suite's string-heavy inner loop: a closure
    // variable written from a nested function (scope writes are what the
    // LLInt/Baseline scope caches arm for replacement) and fresh objects keyed
    // by computed strings.
    let hash = 0x811c9dc5 >>> 0;
    function mix(value) { hash = ((hash ^ (value & 0xffff)) * 0x01000193) >>> 0; }
    const pieces = [];
    for (let i = 0; i < 64; ++i) { let piece = ""; const len = 3 + (i % 7); for (let j = 0; j < len; ++j) piece += String.fromCharCode(97 + ((i * 31 + j * 7 + seed) % 26)); pieces.push(piece); }
    let total = 0;
    for (let o = 0; o < 600; ++o) {
        let rope = "";
        for (let i = 0; i < 120; ++i) rope += pieces[(i * 7 + o * 13) & 63];
        mix(rope.charCodeAt((o * 97) % rope.length));
        const table = {};
        const span = rope.length - 9;
        for (let k = 0; k < 48; ++k) {
            const key = rope.substring((k * 53 + o * 17) % span, (k * 53 + o * 17) % span + 8);
            table[key] = (table[key] === undefined ? 0 : table[key]) + 1;
        }
        for (const key in table) { total += table[key]; mix(key.charCodeAt(0)); }
    }
    return total + (hash & 0);
}

work(0); work(1); // warm the ICs on the main thread first (this is what arms sets other threads then fire)
const stops0 = $vm.jsThreadsCounter("stwRequest");
const watcherless0 = $vm.jsThreadsCounter("watchpointFireWatcherless");
const threads = [];
for (let t = 0; t < 4; ++t) threads.push(new Thread(() => work(t * 11)));
let sum = work(5);
for (const t of threads) sum += t.join();
const stops = $vm.jsThreadsCounter("stwRequest") - stops0;
const watcherless = $vm.jsThreadsCounter("watchpointFireWatcherless") - watcherless0;
if (sum !== 5 * 600 * 48) throw new Error("counts not conserved: " + sum);
if (typeof AMPLIFY_VERBOSE !== "undefined") print("stop-the-world requests during the threaded phase: " + stops + "; watcher-less fires: " + watcherless);
if (stops > 60) throw new Error(stops + " stop-the-world requests for a workload whose only Class-A fires have no watchers (watcher-less fires seen: " + watcherless + ")");
print("PASS");
