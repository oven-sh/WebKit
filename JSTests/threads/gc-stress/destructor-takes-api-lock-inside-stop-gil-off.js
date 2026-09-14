//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// A destructor may take the VM's API lock, as it may on main (SPEC-heap §10G): an embedder's teardown often does - Bun's
// JSValue unprotect takes a JSLockHolder for its assertions. GIL off the shared collection's conductor runs destructors
// inside its own stop window with its heap access released (the cycle-end sweep of weak-bearing blocks, the
// requested-Full sweep, weak finalizers), and every JSLock::lock runs the gated access acquire: it saw the conductor's
// own stop pending and waited for it to clear, which only the conductor does - a self-deadlock, every other thread
// parked behind it (AUDIT R9-23, SPEC-heap history §36). The main thread creates probes whose destructor takes the API
// lock and drops them while threads allocate and call gc(); the test passes if it finishes and every probe is destroyed
// exactly once.
load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 25;
const PER_ROUND = 2000;
// Probes a conservative stack scan may still find at the end.
const SLACK = 64;

const [createdBefore, onCreatorBefore, elsewhereBefore] = $vm.destructionProbeCounts();

function makeProbes() {
    let probes = [];
    for (let i = 0; i < PER_ROUND; ++i)
        probes.push($vm.createDestructionProbe(true));
    probes = null;
}
noInline(makeProbes);

for (let round = 0; round < ROUNDS; ++round) {
    makeProbes();
    const threads = [];
    for (let t = 0; t < THREADS; ++t) {
        threads.push(new Thread(() => {
            let junk = [];
            for (let i = 0; i < 40000; ++i) {
                junk.push({ i, t });
                if (junk.length > 500)
                    junk = [];
            }
            if ((round + t) % 3 === 0)
                gc();
        }));
    }
    for (const thread of threads)
        thread.join();
}

let counts;
for (let i = 0; i < 20; ++i) {
    gc();
    counts = $vm.destructionProbeCounts();
    if (counts[0] - createdBefore - (counts[1] + counts[2] - onCreatorBefore - elsewhereBefore) <= SLACK)
        break;
}
const created = counts[0] - createdBefore;
const destroyed = counts[1] + counts[2] - onCreatorBefore - elsewhereBefore;
if (created !== ROUNDS * PER_ROUND)
    throw new Error(`created ${created}`);
if (destroyed > created)
    throw new Error(`${destroyed} destructions for ${created} probes`);
if (created - destroyed > SLACK)
    throw new Error(`${created - destroyed} of ${created} probes never destroyed`);
