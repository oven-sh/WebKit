//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// A dead cell's destructor runs on whichever thread sweeps it (SPEC-heap
// §10G): the allocating thread, a thread that services a collection's
// epilogue or calls gc(), or GIL off the collection's conductor. Whatever
// thread that is, every destructor runs exactly once. The main thread creates
// destructible probes and drops them while other threads allocate and collect;
// at the end every probe has been destroyed once, and never more than once.
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
        probes.push($vm.createDestructionProbe());
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
    const [created, onCreator, elsewhere] = $vm.destructionProbeCounts();
    if (onCreator + elsewhere - onCreatorBefore - elsewhereBefore > created - createdBefore)
        throw new Error(`round ${round}: ${onCreator + elsewhere} destructions for ${created} probes`);
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
