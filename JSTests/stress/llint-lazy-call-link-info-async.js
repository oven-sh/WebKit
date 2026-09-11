//@ runDefault
//@ runDefault("--useLazyLLIntCallLinkInfos=false")
//@ runDefault("--useJIT=false")
//@ runDefault("--useConcurrentJIT=false", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20", "--thresholdForOptimizeSoon=20")
//@ runDefault("--collectContinuously=true", "--useGenerationalGC=false")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}`);
}

async function* asyncGenerator() { yield 1; yield 2; yield 3; }
let customAsyncIterable = {
    [Symbol.asyncIterator]() {
        let i = 0;
        return { next() { return Promise.resolve({ done: i >= 3, value: i++ }); } };
    }
};
async function iterate(iterable) { let s = 0; for await (let v of iterable) s += v; return s; }
async function delegating() { async function* outer() { yield* asyncGenerator(); } return iterate(outer()); }

let done = false;
(async function () {
    shouldBe(await iterate(asyncGenerator()), 6);
    shouldBe(await iterate(customAsyncIterable), 3);
    shouldBe(await iterate([1, Promise.resolve(2)]), 3);
    shouldBe(await delegating(), 6);
    for (let i = 0; i < 200; ++i) {
        shouldBe(await iterate(asyncGenerator()), 6);
        shouldBe(await iterate(customAsyncIterable), 3);
    }
    let error = null;
    try {
        await iterate({ [Symbol.asyncIterator]: 3 });
    } catch (e) {
        error = e;
    }
    shouldBe(error instanceof TypeError, true);
    done = true;
})().catch((e) => { print(String(e)); $vm.abort(); });
drainMicrotasks();
shouldBe(done, true);
