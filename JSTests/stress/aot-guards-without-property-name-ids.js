//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--useAOTPropertyNameIDs=0", "--useAOTInlining=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--useAOTInlining=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--useAOTPropertyNameIDs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--useAOTPropertyNameIDs=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTPropertyNameIDs=0", "--useAOTInlining=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTPropertyNameIDs=0", "--useAOTInlining=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTPropertyNameIDs=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
const remarksOf = f => typeof aotRemarks === "function" && aotRemarks(f.name) || null;
const options = typeof jscOptions === "function" ? jscOptions() : { };
const count = name => typeof aotOperationCount === "function" && aotOperationCount(name) || 0;
const guesses = !!remarksOf(check) && !!options.useAOTGuessedPlaces && !!options.useAOTDataStubs;
if (remarksOf(check))
    check("useAOTPropertyNameIDs" in options, true, "the option exists");
const usesNameIDs = !("useAOTPropertyNameIDs" in options) || !!options.useAOTPropertyNameIDs;
const hasTwins = guesses && !!options.useAOTGuardsOverWholeFunctions;
const hasFamilies = hasTwins && !!options.useAOTFamilies;

const kept = [];
function keep(o) { kept.push(o); if (kept.length > 64) kept.length = 0; return o; }
noInline(keep);
function Item(tag, key, child) {
    this.tag = tag;
    this.key = key;
    this.child = child;
}
const makeItem = which => keep(new Item(which, which + 1, which + 2));
function makesShort(which) { return { shared: which, onlyShort: 1 }; }
function makesLong(which) { return { shared: which, onlyLong: 1, alsoLong: 2 }; }
function build(...pairs) {
    let object = { };
    for (let i = 0; i < pairs.length; i += 2)
        object[pairs[i]] = pairs[i + 1];
    return object;
}

function readsThreeOfFamily(o) { return o.tag + o.key + o.child; }
function storesThreeOfFamily(o, value) { o.tag = value; o.key = value + 1; o.child = value + 2; }
function readsOneOfFamily(o) { return o.key; }
function readsOfTwoShapes(x, y, z) { return x.shared + y.shared + z.shared; }
for (const f of [readsThreeOfFamily, storesThreeOfFamily, readsOneOfFamily, readsOfTwoShapes, makesShort, makesLong])
    noInline(f);

function runAll(make, i) {
    check(readsThreeOfFamily(make(i)), 3 * i + 3, "three reads");
    let stored = make(i);
    storesThreeOfFamily(stored, i + 10);
    check(stored.tag * 10000 + stored.key * 100 + stored.child, (i + 10) * 10000 + (i + 11) * 100 + i + 12, "three stores");
    check(readsOneOfFamily(make(i)), i + 1, "one read");
}
for (let i = 0; i < 300; ++i) {
    runAll(makeItem, i);
    check(readsOfTwoShapes(makesShort(i), makesLong(i + 1), i & 1 ? makesShort(i + 2) : makesLong(i + 2)), 3 * i + 3, "a name that two shapes hold in one slot");
}
const guessedBefore = count("operationAOTCountGuessedPlace") + count("operationAOTCountGuessedStore");
for (let i = 0; i < 100; ++i) {
    runAll(which => build("child", which + 2, "key", which + 1, "tag", which), i);
    runAll(which => Object.defineProperty(makeItem(which), "key", { get() { return this.tag + 1; }, set(value) { }, configurable: true }), 0);
    runAll(which => { let item = makeItem(which); delete item.tag; item.tag = which; return item; }, i);
    runAll(which => Object.assign(makeItem(which), { extra: 1 }), i);
    runAll(which => { let item = makeItem(which); keep(Object.create(item)); return item; }, i);
    let parent = makeItem(i), heir = Object.create(parent);
    check(readsThreeOfFamily(heir), 3 * i + 3, "inherited");
    storesThreeOfFamily(parent, i + 10);
    check(readsThreeOfFamily(heir), 3 * i + 33, "inherited after stores to the prototype");
    let frozen = Object.freeze(makeItem(i));
    storesThreeOfFamily(frozen, i + 10);
    check(readsThreeOfFamily(frozen), 3 * i + 3, "stores to a frozen object are dropped");
    runAll(makeItem, i);
    check(readsOfTwoShapes(build("other", 0, "shared", i), makesLong(i + 1), makesShort(i + 2)), 3 * i + 3, "the name in another slot");
    check(readsThreeOfFamily(i), NaN, "a number");
}

function has(f, remark, expected) {
    check(remarksOf(f).includes(remark), expected, f.name + (expected ? " lacks " : " has ") + remark + ": " + remarksOf(f).join(" | ") + "; it is there");
}
const startsWith = (f, prefix) => remarksOf(f).some(remark => remark.startsWith(prefix));
if (guesses) {
    for (const f of [readsThreeOfFamily, storesThreeOfFamily]) {
        has(f, "guards-over-whole-function", hasTwins && (usesNameIDs || hasFamilies));
        check(startsWith(f, "guard-checks-family"), hasFamilies, f.name + " has guards that compare the family");
        check(startsWith(f, "guard-checks-name"), hasTwins && !hasFamilies && usesNameIDs, f.name + " has guards that compare a name");
    }
    has(readsOfTwoShapes, "guards-over-whole-function", hasTwins && usesNameIDs);
    has(readsOfTwoShapes, "guessed-place:shared", usesNameIDs);
    has(readsOneOfFamily, "guards-over-whole-function", false);
    for (const f of [readsThreeOfFamily, storesThreeOfFamily, readsOneOfFamily, readsOfTwoShapes]) {
        if (usesNameIDs)
            continue;
        check(startsWith(f, "guessed-place-read") || startsWith(f, "guessed-place-store"), false, f.name + " takes a guessed place outside a guard");
        check(startsWith(f, "guard-checks-name"), false, f.name + " has guards that compare a name");
    }
    check(startsWith(readsOneOfFamily, "guessed-place-read"), usesNameIDs, "readsOneOfFamily takes a guessed place outside a guard");
}
if (guesses && options.useAOTOperationCounters && !options.validateAOTInferredTypes) {
    check(guessedBefore > 0, usesNameIDs, "comparisons of names are counted");
    if (!usesNameIDs)
        check(count("operationAOTCountGuessedPlace") + count("operationAOTCountGuessedStore"), 0, "comparisons of names");
    check(count("operationAOTCountFamilyGuard") > 0, hasFamilies, "comparisons of families are counted");
}
