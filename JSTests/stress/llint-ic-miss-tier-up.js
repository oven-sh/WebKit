//@ skip if not $jitTests
//@ runDefault("--useConcurrentJIT=0", "--useDFGJIT=0", "--thresholdForJITAfterWarmUp=100000", "--thresholdForJITSoon=30", "--missCountForLLIntTierUp=12")
//@ runDefault("--useConcurrentJIT=0", "--useDFGJIT=0", "--thresholdForJITAfterWarmUp=100000", "--thresholdForJITSoon=30", "--missCountForLLIntTierUp=0")
//@ runDefault("--useConcurrentJIT=0", "--useDFGJIT=0", "--thresholdForJITAfterWarmUp=100000", "--thresholdForJITSoon=30", "--missCountForLLIntTierUp=12", "--useLLIntUnsetCaching=0", "--useLLIntStringLengthCaching=0", "--useLLIntPrototypeCacheRearming=0")
//@ runDefault("--useJIT=0", "--missCountForLLIntTierUp=12")

// A get_by_id or put_by_id site that calls its slow path Options::missCountForLLIntTierUp() times in the LLInt
// makes its function wait for thresholdForJITSoon executions, not thresholdForJITAfterWarmUp, before the
// Baseline JIT compiles it. A function with no such site waits for thresholdForJITAfterWarmUp.
//
// An execution counts 15 (5 at the start of a call, 10 at the return), and 1 for each turn of a loop.
// With thresholdForJITAfterWarmUp=100000, 200 calls are far from the Baseline JIT without the misses.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

const options = jscOptions();
const missCount = options.missCountForLLIntTierUp;
const expectTierUp = options.useJIT && options.useBaselineJIT && missCount > 0;
const calls = 200;

// Objects of this many structures, more than one cache entry can hold.
function makeObjects(count) {
    const objects = [];
    for (let i = 0; i < count; ++i) {
        const o = { };
        for (let j = 0; j < i; ++j)
            o["pad" + j] = j;
        o.tag = i;
        objects.push(o);
    }
    return objects;
}

// Returns the number of the call (from 1) in which the function first did not run in the LLInt, or 0.
function firstCallOutOfLLInt(f, argumentFor) {
    for (let i = 0; i < calls; ++i) {
        if (!f(argumentFor(i)))
            return i + 1;
    }
    return 0;
}

function expectEarly(name, call) {
    if (!expectTierUp) {
        shouldBe(call, 0, name + " must stay in the LLInt");
        return;
    }
    if (!call)
        throw new Error(name + " did not leave the LLInt in " + calls + " calls");
    // missCount misses, then thresholdForJITSoon (30) counts at 15 for each call, with some room.
    if (call > missCount + 8)
        throw new Error(name + " left the LLInt in call " + call + ", expected call " + (missCount + 8) + " or earlier");
    if (call < missCount)
        throw new Error(name + " left the LLInt in call " + call + ", before " + missCount + " misses");
}

// get_by_id of an own property of receivers with many structures.
{
    function polymorphicGet(o) {
        const tag = o.tag;
        return $vm.llintTrue();
    }
    const objects = makeObjects(16);
    expectEarly("polymorphicGet", firstCallOutOfLLInt(polymorphicGet, i => objects[i % objects.length]));
    for (let i = 0; i < 100; ++i)
        polymorphicGet(objects[i % objects.length]);
}

// The same site with one structure only: one miss, so no tier up.
{
    function monomorphicGet(o) {
        const tag = o.tag;
        return $vm.llintTrue();
    }
    const object = makeObjects(1)[0];
    shouldBe(firstCallOutOfLLInt(monomorphicGet, i => object), 0, "monomorphicGet must stay in the LLInt");
}

// A getter is never cached in the LLInt.
{
    class WithGetter {
        get tag() { return 1; }
    }
    function getterGet(o) {
        const tag = o.tag;
        return $vm.llintTrue();
    }
    const object = new WithGetter;
    expectEarly("getterGet", firstCallOutOfLLInt(getterGet, i => object));
}

// A receiver that is not a cell is never cached in the LLInt.
{
    function numberGet(n) {
        const f = n.toFixed;
        return $vm.llintTrue();
    }
    expectEarly("numberGet", firstCallOutOfLLInt(numberGet, i => i));
}

// put_by_id that replaces a property of receivers with many structures.
{
    function polymorphicPut(o) {
        o.tag = 1;
        return $vm.llintTrue();
    }
    const objects = makeObjects(16);
    expectEarly("polymorphicPut", firstCallOutOfLLInt(polymorphicPut, i => objects[i % objects.length]));
}

// put_by_id that adds a property to a new object each time: one transition, cached at the first visit.
{
    function transitionPut(o) {
        o.added = 1;
        return $vm.llintTrue();
    }
    shouldBe(firstCallOutOfLLInt(transitionPut, i => ({ })), 0, "transitionPut must stay in the LLInt");
}

// put_by_id through a setter is never cached in the LLInt.
{
    class WithSetter {
        set tag(value) { }
    }
    function setterPut(o) {
        o.tag = 1;
        return $vm.llintTrue();
    }
    const object = new WithSetter;
    expectEarly("setterPut", firstCallOutOfLLInt(setterPut, i => object));
}

// The misses of two sites do not add up: each site has its own count.
{
    const source = [];
    for (let i = 0; i < 40; ++i)
        source.push("t = o" + i + ".tag;");
    const manySites = new Function("objects", "let t; const [" + Array.from({ length: 40 }, (_, i) => "o" + i).join(", ") + "] = objects;" + source.join("") + "return $vm.llintTrue();");
    // Each of the 40 sites sees 4 structures once: 4 misses for each site, 160 in the function.
    const rounds = [makeObjects(40), makeObjects(41).slice(1), makeObjects(42).slice(2), makeObjects(43).slice(3)];
    if (missCount > 4 || !missCount) {
        for (let round = 0; round < 4; ++round)
            shouldBe(manySites(rounds[round]), true, "manySites, round " + round);
        shouldBe(manySites(rounds[3]), true, "manySites");
    }
}

// One call with a long loop: the loop enters the Baseline JIT code.
{
    function loop(objects, turns) {
        for (let i = 0; i < turns; ++i) {
            const tag = objects[i % objects.length].tag;
            if (!$vm.llintTrue())
                return i + 1;
        }
        return 0;
    }
    const objects = makeObjects(16);
    const turn = loop(objects, 2000);
    if (expectTierUp) {
        if (!turn)
            throw new Error("the loop did not leave the LLInt in 2000 turns");
        if (turn > missCount + 100)
            throw new Error("the loop left the LLInt in turn " + turn);
    } else
        shouldBe(turn, 0, "the loop must stay in the LLInt");
    shouldBe(loop(makeObjects(1), 2000) === 0 || expectTierUp, true);
}

// The startup deferral scale is not for a function with such a site. It is for the others.
if (expectTierUp && typeof $vm.setStartupJITDeferralScale === "function") {
    $vm.setStartupJITDeferralScale(50);
    function deferredPolymorphicGet(o) {
        const tag = o.tag;
        return $vm.llintTrue();
    }
    function deferredMonomorphicGet(o) {
        const tag = o.tag;
        return $vm.llintTrue();
    }
    const objects = makeObjects(16);
    const polymorphicCall = firstCallOutOfLLInt(deferredPolymorphicGet, i => objects[i % objects.length]);
    const monomorphicCall = firstCallOutOfLLInt(deferredMonomorphicGet, i => objects[0]);
    $vm.setStartupJITDeferralScale(1);
    expectEarly("deferredPolymorphicGet", polymorphicCall);
    shouldBe(monomorphicCall, 0, "deferredMonomorphicGet must stay in the LLInt");
}
