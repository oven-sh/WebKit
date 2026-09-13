//@ runDefault("--releaseIdleRegExpCodeWhenShrinkingFootprint=1", "--useConcurrentGC=0")
//@ runDefault("--releaseIdleRegExpCodeWhenShrinkingFootprint=1", "--useConcurrentGC=0", "--useJIT=0")

// VM::shrinkFootprintWhenIdle(KeepCodeInUse) with releaseIdleRegExpCodeWhenShrinkingFootprint drops the compiled code of
// the RegExps that have not matched since the last full collection began. They compile again when they next match.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

const count = 200;
const regExps = [];
for (let i = 0; i < count; ++i)
    regExps.push(new RegExp("^(a+)b" + i + "[c-f]*(?:x|y" + i + ")$"));
function matchAll() {
    let matched = 0;
    for (let i = 0; i < count; ++i) {
        if (regExps[i].test("aaab" + i + "cdefy" + i))
            ++matched;
        if (regExps[i].exec("aab" + i + "x")[1] !== "aa")
            throw new Error("bad capture");
    }
    return matched;
}
noInline(matchAll);

assert(matchAll() === count, "all match");
const withCodeBefore = $vm.codeBlockCensus().regExpsWithCode;
assert(withCodeBefore >= count, "the RegExps have code: " + withCodeBefore);

fullGC();
// These ten are in use in the current cycle.
for (let i = 0; i < 10; ++i)
    assert(regExps[i].test("ab" + i + "x"), "match after the collection");

const keepCodeThatNeedsParsing = true;
const keepCodeInUse = true;
$vm.shrinkFootprintWhenIdle(keepCodeThatNeedsParsing, keepCodeInUse);
setTimeout(() => {
    const withCodeAfter = $vm.codeBlockCensus().regExpsWithCode;
    assert(withCodeAfter <= withCodeBefore - (count - 10) + 5, "idle RegExps lost their code: " + withCodeBefore + " -> " + withCodeAfter);
    assert(withCodeAfter >= 10, "RegExps in use kept theirs: " + withCodeAfter);
    assert(matchAll() === count, "all match again");
    assert($vm.codeBlockCensus().regExpsWithCode >= count, "and have code again");
}, 0);
