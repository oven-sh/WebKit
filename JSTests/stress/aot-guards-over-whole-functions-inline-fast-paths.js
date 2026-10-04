//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTInlineFastPathsInFunctionsWithGuards=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTInlineFastPathsInFunctionsWithGuards=1", "--validateGraphAtEachPhase=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTInlineFastPathsInFunctionsWithGuards=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTInlineFastPathsInFunctionsWithGuards=1", "--useAOTGuardsOverWholeFunctionsInsteadOfLoopSplitting=1", "--minimumAOTGuardsOverWholeFunction=1", "--validateGraphAtEachPhase=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlineFastPathsInFunctionsWithGuards=1")
//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
const remarksOf = f => typeof aotRemarks === "function" && aotRemarks(f.name) || null;
const options = typeof jscOptions === "function" ? jscOptions() : { };
const isCompiled = !!remarksOf(check) && !!options.useAOTGuessedPlaces && !!options.useAOTDataStubs && !!options.useAOTInlineFastPathsInLoops;
const hasTwins = isCompiled && !!options.useAOTGuardsOverWholeFunctions;
if (hasTwins)
    check("useAOTInlineFastPathsInFunctionsWithGuards" in options, true, "the option exists");
const isInline = hasTwins && !!options.useAOTInlineFastPathsInFunctionsWithGuards;

const kept = [];
function keep(o) { kept.push(o); return o; }
noInline(keep);
function build(...pairs) {
    let o = {};
    for (let [name, value] of pairs)
        o[name] = value;
    return keep(o);
}
function Item(which) {
    this.tag = which;
    this.key = which + 1;
    this.child = which + 2;
    this.flags = which + 3;
}
const makeItem = which => keep(new Item(which));

function readsInFrontOfEnvironment(w, x, y, z, mask) {
    let first = w.flags;
    let read;
    {
        let captured = first & mask;
        read = () => captured;
        keep(read);
    }
    let sum = x.tag + y.key + z.child;
    if (sum === first)
        return -1;
    return sum * 2 - (first | 1) + read();
}
function hasTooFewPlaces(w, mask) {
    return (w.flags & mask) + 1;
}
noInline(readsInFrontOfEnvironment);
noInline(hasTooFewPlaces);

function expected(w, x, y, z, mask) {
    let sum = x + (y + 1) + (z + 2);
    return sum === w + 3 ? -1 : sum * 2 - ((w + 3) | 1) + ((w + 3) & mask);
}
for (let i = 0; i < 300; ++i) {
    check(readsInFrontOfEnvironment(makeItem(i), makeItem(1), makeItem(2), makeItem(3), 5), expected(i, 1, 2, 3, 5), "readsInFrontOfEnvironment");
    check(hasTooFewPlaces(makeItem(i), 6), ((i + 3) & 6) + 1, "hasTooFewPlaces");
}
if (hasTwins && options.minimumAOTGuardsOverWholeFunction == 3 && !options.validateAOTInferredTypes) {
    let remarks = remarksOf(readsInFrontOfEnvironment);
    check(remarks.includes("guards-over-whole-function"), true, "readsInFrontOfEnvironment has guards over the whole function");
    check(remarks.includes("name-check-guards-read:flags"), false, "the read in front of the environment has a guard");
    check(remarks.includes("guessed-place-read-through-stub:flags"), !isInline, "the read in front of the first guard, which has no generic copy, goes through a stub");
    check(remarks.includes("guessed-place-read:flags"), isInline, "the first copy reads flags inline");
    check(remarksOf(hasTooFewPlaces).includes("guessed-place-read:flags"), false, "a function without guards reads flags inline");
}
if (isCompiled && !hasTwins)
    check(remarksOf(readsInFrontOfEnvironment).includes("guessed-place-read:flags"), false, "without guards over whole functions flags is read inline");

check(readsInFrontOfEnvironment(makeItem(0), makeItem(0.5), makeItem(2), makeItem(3), 5), (0.5 + 3 + 5) * 2 - 3 + 1, "a double");
check(readsInFrontOfEnvironment(makeItem(0), makeItem("s"), makeItem(2), makeItem(3), 5), NaN, "a string");
check(readsInFrontOfEnvironment(makeItem(2 ** 31 - 4), makeItem(2 ** 30), makeItem(2 ** 30), makeItem(3), -1), (2 ** 31 + 6) * 2 - (2 ** 31 - 1) + (2 ** 31 - 1), "sums that leave the int32 range");
let log = [];
let operand = { valueOf() { log.push("valueOf"); return 7; } };
check(readsInFrontOfEnvironment(makeItem(0), build(["tag", operand], ["key", 0], ["child", 0], ["flags", 0]), makeItem(2), makeItem(3), 5), (7 + 3 + 5) * 2 - 3 + 1, "an object as an operand");
check(log.join(), "valueOf", "its valueOf runs once");
check(readsInFrontOfEnvironment(build(["other", 0], ["flags", 12]), makeItem(1), makeItem(2), makeItem(3), 4), (1 + 3 + 5) * 2 - 13 + 4, "another shape at the read without a guard");
check(readsInFrontOfEnvironment(makeItem(0), build(["other", 0], ["tag", 1]), makeItem(2), makeItem(3), 5), (1 + 3 + 5) * 2 - 3 + 1, "another shape at the first guard");
