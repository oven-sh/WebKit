//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--useAOTGuardsOverWholeFunctionsWithHandlers=1", "--useAOTGuardsOverWholeFunctionsInsteadOfLoopSplitting=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--minimumAOTGuardsOverWholeFunction=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTLoopSplitting=0")
//@ runDefault

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function Item(a, b, c) {
    this.a = a;
    this.b = b;
    this.c = c;
}
function build(...pairs) {
    let object = { };
    for (let i = 0; i < pairs.length; i += 2)
        object[pairs[i]] = pairs[i + 1];
    return object;
}
const receivers = [
    () => new Item(1, 2, 3),
    () => build("c", 3, "b", 2, "a", 1),
    () => build("a", 1, "c", 3, "b", 2),
    () => build("a", 1, "b", 2, "other", 0, "c", 3),
    () => Object.defineProperty(new Item(1, 0, 3), "b", { get() { return 2; } }),
];
const theGlobal = this;

(function () {
    "use strict";
    let noted = "nothing";
    function receiver() { return this; }
    function receiverOf(first, second, third) { return [this, first + second + third]; }
    function notes() { noted = this; }

    function behindGuards(o) { let sum = o.a + o.b + o.c; return [receiver(), sum]; }
    function betweenGuards(o) { let first = o.a, one = receiver(), second = o.b, two = receiver(), third = o.c; return [one === two ? one : "differ", first + second + third]; }
    function amongArguments(o) { return receiverOf(o.a, o.b, o.c); }
    function behindJoin(o, flag) { let sum = o.a + o.b; if (flag) sum += o.c; else sum += 3; return [receiver(), sum]; }
    function inLoop(o) { let sum = 0, seen; for (let i = 0; i < 3; ++i) { sum += i == 0 ? o.a : i == 1 ? o.b : o.c; seen = receiver(); } return [seen, sum]; }
    function ignoresResult(o) { let sum = o.a + o.b + o.c; noted = "nothing"; notes(); return [noted, sum]; }
    function inTailPosition(o) { if (o.a + o.b + o.c !== 6) return "wrong sum"; return receiver(); }
    function behindHandler(o) { let sum = o.a; try { sum += o.b; sum += o.c; } catch { sum = -1; } return [receiver(), sum]; }

    for (let i = 0; i < 400; ++i) {
        let make = receivers[i % 7 < receivers.length ? i % 7 : 0];
        for (const caller of [behindGuards, betweenGuards, amongArguments, inLoop, ignoresResult, behindHandler]) {
            let [got, sum] = caller(make());
            check(got, undefined, "this of a strict function called from " + caller.name);
            check(sum, 6, "the sum in " + caller.name);
        }
        let [got, sum] = behindJoin(make(), i & 1);
        check(got, undefined, "this of a strict function called from behindJoin");
        check(sum, 6, "the sum in behindJoin");
        check(inTailPosition(make()), undefined, "this of a strict function called from inTailPosition");
    }
})();

(function () {
    function receiver() { return this; }
    function behindGuards(o) { let sum = o.a + o.b + o.c; return [receiver(), sum]; }
    function betweenGuards(o) { let first = o.a, one = receiver(), second = o.b, two = receiver(), third = o.c; return [one === two ? one : "differ", first + second + third]; }
    function throughWith(o, holder) { let sum = o.a + o.b + o.c; with (holder) { return [method(), sum]; } }
    const holder = { method() { return this; } };

    for (let i = 0; i < 400; ++i) {
        let make = receivers[i % 7 < receivers.length ? i % 7 : 0];
        for (const caller of [behindGuards, betweenGuards]) {
            let [got, sum] = caller(make());
            check(got, theGlobal, "this of a sloppy function called from " + caller.name);
            check(sum, 6, "the sum in " + caller.name);
        }
        let [got, sum] = throughWith(make(), holder);
        check(got, holder, "this of a method found through with");
        check(sum, 6, "the sum in throughWith");
    }
})();
