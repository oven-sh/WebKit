//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=0")
//@ runDefault
(function () {
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function neverMakes(c) {
    if (undefined) {
        const makes = () => ({ onlyBornInDeadCode: c, alsoBornThere: 2 });
        const reads = o => o.onlyReadInDeadCode;
        return reads(makes());
    }
    return "skipped";
}
function makeLive(a, b) { return { liveFirst: a, liveSecond: b }; }
function readsLiveSecond(o) { return o.liveSecond; }
function storesLiveSecond(o, value) { o.liveSecond = value; }
for (let f of [neverMakes, makeLive, readsLiveSecond, storesLiveSecond])
    globalThis[f.name + "Kept"] = f;

check(neverMakes(1), "skipped", "the branch");
const born = ["onlyBorn", "InDead", "Code"].join("");
const read = ["onlyRead", "InDead", "Code"].join("");
for (let round = 0; round < 50; ++round) {
    let assigned = { };
    assigned[born] = round;
    assigned[read] = -round;
    check(assigned[born] + ":" + assigned[read], round + ":" + -round, "names of code that is not compiled, as computed keys");
    check(Object.keys(assigned).join(), born + "," + read, "its keys");
    check((born in assigned) + ":" + Object.hasOwn(assigned, read), "true:true", "they are there");
    check(readsLiveSecond(assigned), undefined, "another name, which is absent");
    storesLiveSecond(assigned, round);
    check(readsLiveSecond(assigned) + ":" + assigned[born], round + ":" + round, "another name, added behind them");

    let parsed = JSON.parse('{"' + born + '":5,"liveSecond":6,"' + read + '":7}');
    check(parsed[born] + readsLiveSecond(parsed) + parsed[read], 18, "from JSON");

    let live = makeLive(1, round);
    check(readsLiveSecond(live), round, "an object as it was born");
    check(live[born], undefined, "the name of code that is not compiled, absent");
    live[born] = "late";
    check(readsLiveSecond(live) + live[born], round + "late", "added to an object as it was born");

    let fresh = { };
    for (let i = 0; i < 40; ++i)
        fresh["madeAtRunTime" + i] = i;
    check(fresh.madeAtRunTime39 + fresh["madeAtRunTime" + 0], 39, "names that get their IDs at run time");
    check(readsLiveSecond(fresh), undefined, "and a listed name is absent there");
}
})();
