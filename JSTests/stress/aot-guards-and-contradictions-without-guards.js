//@ runDefault
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--failEveryNthAOTGuardForTesting=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1")

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
const options = typeof jscOptions === "function" ? jscOptions() : { };
const remarksOf = name => typeof aotRemarks === "function" && aotRemarks(name) || null;
const isOn = !!remarksOf("check") && !!options.useAOTGuardsOverWholeFunctions && !!options.useAOTFamilies && !!options.useAOTDataStubs;

(function () {
    function first() { return 2; }
    function second() { return 3; }
    function Item(tag, key, child) {
        this.tag = tag;
        this.key = key;
        this.child = child;
    }
    function build(...pairs) {
        let object = { };
        for (let i = 0; i < pairs.length; i += 2)
            object[pairs[i]] = pairs[i + 1];
        return object;
    }
    let mayBeEmpty;
    let kept = 0, keptInOneStep = 0;
    function keeps(e) { if (e !== undefined && e !== null) kept = e; }
    function keepsInOneStep(e) { if (e != null) keptInOneStep = e; }
    function readsThreeAndKeeps(o) { keeps(mayBeEmpty); return o.tag + o.key + o.child; }
    function readsThreeAndKeepsInOneStep(o) { keepsInOneStep(mayBeEmpty); return o.tag + o.key + o.child; }

    for (let i = 0; i < 400; ++i) {
        mayBeEmpty = (i & 3) === 0 ? undefined : (i & 3) === 1 ? null : (i & 3) === 2 ? first : second;
        let make = i % 10 == 9 ? () => build("child", 2, "key", 1, "tag", i) : () => new Item(i, 1, 2);
        check(readsThreeAndKeeps(make()), i + 3, "three reads behind a test in two steps");
        check(readsThreeAndKeepsInOneStep(make()), i + 3, "three reads behind a test in one step");
        let expected = i < 2 ? 1 : (i & 3) == 2 ? 2 : (i & 3) == 3 ? 3 : 3;
        check(kept === 0 ? 1 : kept(), expected, "what the test in two steps kept");
        check(keptInOneStep === 0 ? 1 : keptInOneStep(), expected, "what the test in one step kept");
    }

    if (!isOn)
        return;
    const reasons = name => remarksOf(name).filter(remark => remark.startsWith("no-guards-over-whole-function")).join();
    const has = (name, remark) => remarksOf(name).includes(remark);
    check(has("readsThreeAndKeepsInOneStep", "guards-over-whole-function"), true, "readsThreeAndKeepsInOneStep has guards over the whole function");
    check(has("readsThreeAndKeepsInOneStep", "contradicts-analysis-without-guards"), false, "readsThreeAndKeepsInOneStep contradicts the analysis");
    if (!options.useAOTInlining) {
        check(has("readsThreeAndKeeps", "guards-over-whole-function"), true, "readsThreeAndKeeps has guards over the whole function where nothing is inlined");
        check(has("readsThreeAndKeeps", "contradicts-analysis-without-guards"), false, "readsThreeAndKeeps contradicts the analysis where nothing is inlined");
        return;
    }
    check(has("readsThreeAndKeeps", "inlined-call:keeps"), true, "keeps is inlined");
    check(has("readsThreeAndKeeps", "guards-over-whole-function"), false, "readsThreeAndKeeps has guards over the whole function");
    check(reasons("readsThreeAndKeeps"), "no-guards-over-whole-function:contradicts-analysis-of-store", "why readsThreeAndKeeps has no guards");
    check(has("readsThreeAndKeeps", "contradicts-analysis-without-guards"), true, "readsThreeAndKeeps contradicts the analysis without guards too (once the inference follows a test in two steps, this needs another shape)");
})();
