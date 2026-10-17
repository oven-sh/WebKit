//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=200")
//@ runDefault

// VM::shrinkFootprintWhenIdle returns code that nothing links against to the bytecode cache it was decoded from. A
// generator or an async function that is suspended has no CodeBlock once that got old, but it is about to link its code
// again: that code stays, and the closures the resumed activation makes read the right bindings either way
// (suspended-activation-keeps-scope-inference-when-code-is-generated-again.js).

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

const iterations = 3;
const calls = testLoopCount * 2;

function* generatorBody(out) {
    for (let iter = 0; iter < iterations; iter++) {
        const box = { iter };
        if (!iter)
            yield;
        let stale = 0;
        for (let k = 0; k < calls; k++) {
            if ([0].reduce(acc => acc + box.iter, 0) !== iter)
                stale++;
        }
        out.push(stale);
    }
}

async function asyncBody(out, gate) {
    for (let iter = 0; iter < iterations; iter++) {
        const box = { iter };
        if (!iter)
            await gate;
        let stale = 0;
        for (let k = 0; k < calls; k++) {
            if ([0].reduce(acc => acc + box.iter, 0) !== iter)
                stale++;
        }
        out.push(stale);
    }
}

function check(name, out) {
    assert(out.length === iterations, name + ": ran " + out.length + " iterations");
    for (let i = 0; i < out.length; i++)
        assert(!out[i], name + ": " + out[i] + " of " + calls + " closures made in iteration " + i + " read another iteration's binding");
}

function shrinkThen(step) {
    $vm.shrinkFootprintWhenIdle(true, false);
    setTimeout(() => {
        fullGC();
        setTimeout(step, 0);
    }, 0);
}

// The object behind an async function's activation cannot be reached from script: count instead. Each of these is a
// function of its own, in this file (so from the cache), suspended at its await.
async function suspended0(gate) { await gate; return 0; }
async function suspended1(gate) { await gate; return 1; }
async function suspended2(gate) { await gate; return 2; }
async function suspended3(gate) { await gate; return 3; }
async function suspended4(gate) { await gate; return 4; }
async function suspended5(gate) { await gate; return 5; }
async function suspended6(gate) { await gate; return 6; }
async function suspended7(gate) { await gate; return 7; }
async function suspended8(gate) { await gate; return 8; }
async function suspended9(gate) { await gate; return 9; }
async function suspended10(gate) { await gate; return 10; }
async function suspended11(gate) { await gate; return 11; }
async function suspended12(gate) { await gate; return 12; }
async function suspended13(gate) { await gate; return 13; }
async function suspended14(gate) { await gate; return 14; }
async function suspended15(gate) { await gate; return 15; }
async function suspended16(gate) { await gate; return 16; }
async function suspended17(gate) { await gate; return 17; }
async function suspended18(gate) { await gate; return 18; }
async function suspended19(gate) { await gate; return 19; }
async function suspended20(gate) { await gate; return 20; }
async function suspended21(gate) { await gate; return 21; }
async function suspended22(gate) { await gate; return 22; }
async function suspended23(gate) { await gate; return 23; }
async function suspended24(gate) { await gate; return 24; }
async function suspended25(gate) { await gate; return 25; }
async function suspended26(gate) { await gate; return 26; }
async function suspended27(gate) { await gate; return 27; }
async function suspended28(gate) { await gate; return 28; }
async function suspended29(gate) { await gate; return 29; }
const asyncFunctions = 30;
let openAll;
const allOpen = new Promise(resolve => { openAll = resolve; });
const pending = [];
for (let i = 0; i < asyncFunctions; i++)
    pending.push(globalThis["suspended" + i](allOpen));

const generatorOut = [];
const generator = generatorBody(generatorOut);
generator.next();
const asyncOut = [];
let open;
const finished = asyncBody(asyncOut, new Promise(resolve => { open = resolve; }));

shrinkThen(() => {
    // undefined when this is not running from a bytecode cache whose payload stays (nothing can be returned then).
    const fromCache = !!$vm.codeBlockCensus().persistentPayloads;
    if (fromCache)
        assert($vm.isGeneratorBodyCodeInBytecodeCache(generator) === false, "the suspended generator's code stays");
    const cachedWhileSuspended = $vm.codeBlockCensus().cachedExecutables;
    generator.next();
    check("generator", generatorOut);
    open();
    finished.then(() => {
        check("async function", asyncOut);
        openAll();
        Promise.all(pending).then((values) => {
            assert(values.length === asyncFunctions && values[asyncFunctions - 1] === asyncFunctions - 1, "the async functions finished");
            shrinkThen(() => {
                if (!fromCache)
                    return;
                assert($vm.isGeneratorBodyCodeInBytecodeCache(generator) === true, "the generator's code goes back once it has finished");
                // Once they have finished, the functions they ran in are garbage, in the cache or not. While they were
                // suspended those functions were alive, and would have counted had their code gone back.
                const cachedAfterwards = $vm.codeBlockCensus().cachedExecutables;
                assert(cachedWhileSuspended - cachedAfterwards < asyncFunctions, "the suspended async functions' code stayed: " + cachedWhileSuspended + " executables had their code in the cache while they were suspended, " + cachedAfterwards + " afterwards");
            });
        });
    });
});
