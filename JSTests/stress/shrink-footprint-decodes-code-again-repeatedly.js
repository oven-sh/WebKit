//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--forceCodeBlockToJettisonDueToOldAge=1")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--destroy-vm")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useThinChildExecutables=0")

// Drop after drop of code that a persistent bytecode cache hands back: every function is called between the drops, so no
// executable stays behind naming the payload's Decoder and each drop has to come by one; functions are constructed as
// well as called after every drop (the code block for construct is the second one an executable names); one round keeps
// the code in use; children of dropped code are remembered while they live and forgotten when they have died. The test
// ends right after a drop, so that with --destroy-vm the VM is torn down with children remembered.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

function P(a) { let tdzUser = () => later + a; let later = 1; function q() { return tdzUser() + 1; } return { q, tdzUser, k: class { constructor() { this.v = later; } static m() { return later; } } }; }
function F(a) { this.a = a; }
function G(a) { if (new.target) this.g = a; return a; }
function all() { const p = P(1); return [p.q(), p.tdzUser(), p.k.m(), new p.k().v, new F(2).a, G(3), new G(4).g].join(); }

const options = jscOptions();
const recovers = options.useCodeRecoveryFromBytecodeCache && options.useLeanBytecodeCacheDecoder && options.useBorrowedBytecodeFromCache && options.diskCachePayloadIsPersistentForTesting && options.forceDiskCache;
const expected = all();
const keep = P(10);
const rounds = 5;
let round = 0;
let before;
function step() {
    try {
        if (round) {
            fullGC();
            const dropped = $vm.codeBlockCensus();
            const keptCodeInUse = round === 3;
            if (recovers && !keptCodeInUse) {
                assert(dropped.cachedExecutables > before.cachedExecutables, "executables name their cache records again after drop " + round + ": " + JSON.stringify(dropped));
                assert(dropped.unlinkedFunction < before.unlinkedFunction, "their unlinked code went, drop " + round);
                assert(dropped.parentsWithRememberedChildren >= 1, "keep's parent remembers its live children, drop " + round);
            }
            if (recovers && keptCodeInUse && !options.forceCodeBlockToJettisonDueToOldAge)
                assert(dropped.unlinkedFunction >= before.unlinkedFunction - 1, "code in use keeps its unlinked code: " + before.unlinkedFunction + " -> " + dropped.unlinkedFunction);
            assert(all() === expected, "results after drop " + round);
            assert(keep.q() === 12 && keep.k.m() === 1 && new keep.k().v === 1, "closures from before the drops, drop " + round);
            fullGC();
            const used = $vm.codeBlockCensus();
            if (recovers)
                assert(used.parentsWithRememberedChildren === 0, "what was remembered was taken or has died, drop " + round + ": " + used.parentsWithRememberedChildren);
        }
        fullGC();
        before = $vm.codeBlockCensus();
        $vm.shrinkFootprintWhenIdle(true, round + 1 === 3);
        if (round++ < rounds)
            setTimeout(step, 0);
    } catch (e) {
        print("FAIL: " + e + "\n" + e.stack);
        $vm.abort();
    }
}
step();
