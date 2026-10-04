//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTOperationCounters=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTInlining=0", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1")
//@ runDefault("--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTFamilies=1", "--useAOTGuardsOverWholeFunctions=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTFamilies=1", "--useAOTGuardsOverWholeFunctions=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTFamilies=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTOperationCounters=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTFamilies=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTInlining=0", "--useDollarVM=1")

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function thrownBy(f) {
    try {
        f();
    } catch (error) {
        return error.constructor.name;
    }
    return "nothing";
}
const remarksOf = f => typeof aotRemarks === "function" && aotRemarks(f.name) || null;
const options = typeof jscOptions === "function" ? jscOptions() : { };
const isCompiled = !!remarksOf(check);
const isOn = isCompiled && !!options.useAOTGuardsOverWholeFunctions && !!options.useAOTDataStubs && !!options.useAOTGuessedPlaces;
const usesFamilies = isOn && !!options.useAOTFamilies;

const guardRemarksOf = f => remarksOf(f).filter(remark => remark.includes("guard")).join(" ");
const namesIn = (f, prefix) => [...new Set(remarksOf(f).filter(remark => remark.startsWith(prefix + ":")).map(remark => remark.slice(prefix.length + 1)))].sort().join(",");
function checks(f, mustHaveGuards, withNames, withFamilies) {
    if (!isOn)
        return;
    if (!remarksOf(f).includes("guards-over-whole-function")) {
        if (mustHaveGuards)
            throw new Error("no guards over the whole of " + f.name + ": " + remarksOf(f).join(" "));
        return;
    }
    const hasFamilies = remarksOf(f).some(remark => remark.startsWith("guard-checks-family"));
    if (hasFamilies !== usesFamilies)
        throw new Error(f.name + (hasFamilies ? " has" : " lacks") + " guards that check a family: " + guardRemarksOf(f));
    const expected = hasFamilies ? withFamilies : withNames;
    for (const kind of ["nothing", "byte", "name-of-known-cell"]) {
        if (!(kind in expected))
            continue;
        const actual = namesIn(f, "guard-checks-" + kind);
        const wanted = expected[kind].slice().sort().join(",");
        if (actual !== wanted)
            throw new Error(f.name + ": the guards that check " + kind + " are on [" + actual + "] instead of [" + wanted + "]: " + guardRemarksOf(f));
    }
}

const kept = [];
function keep(o) { kept.push(o); return o; }
function Item(tag, key, child, sibling, flags) {
    this.tag = tag;
    this.key = key;
    this.child = child;
    this.sibling = sibling;
    this.flags = flags;
}
function makeItem(which) { return keep(new Item("tag" + which, "key" + which, "child" + which, "sibling" + which, "flags" + which)); }
function build(...pairs) {
    const o = {};
    for (const [name, value] of pairs)
        o[name] = value;
    return keep(o);
}
let calls = 0;
function note(what) { ++calls; return what; }
noInline(note);

function readsTwice(a) {
    const first = a.tag;
    const second = a.key;
    const third = a.tag;
    return first + "|" + second + "|" + third;
}
function storesThenReads(a, value) {
    a.tag = value;
    const first = a.tag;
    const second = a.key;
    const third = a.child;
    return first + "|" + second + "|" + third;
}
function readsThenStoresTwice(a, value, other) {
    const before = a.tag;
    a.tag = value;
    a.tag = other;
    const after = a.tag;
    const key = a.key;
    return before + "|" + after + "|" + key;
}
function readsAcrossStoreToAnother(a, b) {
    const before = a.tag;
    b.tag = "stored";
    const after = a.tag;
    const key = a.key;
    return before + "|" + after + "|" + key;
}
function readsOnBothPaths(a, condition) {
    let result;
    if (condition)
        result = a.tag;
    else
        result = a.tag;
    const after = a.tag;
    const key = a.key;
    return result + (condition ? "1" : "2") + "|" + after + "|" + key;
}
function readsAcrossVariables(a) {
    let shared = "outer";
    const reader = () => shared;
    const before = a.tag;
    const between = shared;
    shared = between;
    const after = a.tag;
    const key = a.key;
    return before + "|" + keep(reader)() + "|" + after + "|" + key;
}
function readsAcrossArray(a) {
    const before = a.tag;
    const list = [before, 1];
    const after = a.tag;
    const key = a.key;
    return list[0] + "|" + after + "|" + key;
}
function readsAcrossLiteral(a) {
    const before = a.tag;
    const made = { holds: before, more: 1 };
    const after = a.tag;
    const key = a.key;
    keep(made);
    return before + "|" + after + "|" + key;
}
function readsAcrossClosure(a) {
    const before = a.tag;
    const closure = () => before;
    const after = a.tag;
    const key = a.key;
    return keep(closure)() + "|" + after + "|" + key;
}
function readsAcrossComparison(a, other) {
    const before = a.tag;
    const isSame = before === other;
    const after = a.tag;
    const key = a.key;
    return before + "|" + isSame + "|" + after + "|" + key;
}

function readsAcrossCall(a, callee) {
    const before = a.tag;
    callee(a);
    const after = a.tag;
    const key = a.key;
    const child = a.child;
    return before + "|" + String(after) + "|" + key + "|" + child;
}
function readsAcrossConstruct(a) {
    const before = a.tag;
    const made = new Item("tagn", "keyn", "childn", "siblingn", "flagsn");
    const after = a.tag;
    const key = a.key;
    return before + "|" + made.tag + "|" + after + "|" + key;
}
function readsAcrossAddedProperty(a, b) {
    const before = a.tag;
    b.added = 1;
    const after = a.tag;
    const key = a.key;
    const child = a.child;
    return before + "|" + after + "|" + key + "|" + child;
}
function readsOnOnePath(a, condition) {
    let result = "";
    if (condition)
        result = a.tag;
    const after = a.tag;
    const key = a.key;
    const child = a.child;
    return result + "|" + after + "|" + key + "|" + child;
}
function readsTwoValues(a, b) {
    const first = a.tag;
    const second = b.tag;
    const third = a.key;
    const fourth = b.key;
    return first + "|" + second + "|" + third + "|" + fourth;
}
function readsAcrossKeyedRead(a, name) {
    const before = a.tag;
    const keyed = a[name];
    const after = a.tag;
    const key = a.key;
    return before + "|" + String(keyed) + "|" + String(after) + "|" + key;
}
function readsAcrossDelete(a) {
    const before = a.tag;
    delete a.tag;
    const after = a.tag;
    const key = a.key;
    const child = a.child;
    return before + "|" + String(after) + "|" + key + "|" + child;
}
function readsAcrossConversion(a, other) {
    const before = a.tag;
    const text = "" + other;
    const after = a.tag;
    const key = a.key;
    return before + "|" + text + "|" + String(after) + "|" + key;
}
function readsInLoop(a, n) {
    let result = a.tag;
    for (let i = 0; i < n; ++i) {
        note(i);
        result += a.tag;
    }
    const key = a.key;
    const child = a.child;
    return result + "|" + key + "|" + child;
}
function readsMergedValue(a, b, condition) {
    const first = a.tag;
    const second = b.tag;
    const chosen = condition ? a : b;
    const third = chosen.tag;
    const fourth = chosen.key;
    return first + second + "|" + third + "|" + fourth;
}

for (let i = 0; i < 200; ++i) {
    const a = makeItem(i);
    const b = makeItem(i + "b");
    check(readsTwice(a), "tag" + i + "|key" + i + "|tag" + i, "readsTwice");
    check(storesThenReads(makeItem(i), "new"), "new|key" + i + "|child" + i, "storesThenReads");
    check(readsThenStoresTwice(makeItem(i), "new", "newer"), "tag" + i + "|newer|key" + i, "readsThenStoresTwice");
    check(readsAcrossStoreToAnother(a, b), "tag" + i + "|tag" + i + "|key" + i, "readsAcrossStoreToAnother");
    check(b.tag, "stored", "what readsAcrossStoreToAnother stored");
    check(readsOnBothPaths(a, i & 1), "tag" + i + (i & 1 ? "1" : "2") + "|tag" + i + "|key" + i, "readsOnBothPaths");
    check(readsAcrossVariables(a), "tag" + i + "|outer|tag" + i + "|key" + i, "readsAcrossVariables");
    check(readsAcrossClosure(a), "tag" + i + "|tag" + i + "|key" + i, "readsAcrossClosure");
    check(readsAcrossArray(a), "tag" + i + "|tag" + i + "|key" + i, "readsAcrossArray");
    check(readsAcrossLiteral(a), "tag" + i + "|tag" + i + "|key" + i, "readsAcrossLiteral");
    check(readsAcrossConstruct(a), "tag" + i + "|tagn|tag" + i + "|key" + i, "readsAcrossConstruct");
    check(readsAcrossAddedProperty(a, makeItem("z")), "tag" + i + "|tag" + i + "|key" + i + "|child" + i, "readsAcrossAddedProperty");
    check(readsAcrossComparison(a, i & 1 ? "tag" + i : i), "tag" + i + "|" + !!(i & 1) + "|tag" + i + "|key" + i, "readsAcrossComparison");
    check(readsAcrossCall(a, note), "tag" + i + "|tag" + i + "|key" + i + "|child" + i, "readsAcrossCall");
    check(readsOnOnePath(a, i & 1), (i & 1 ? "tag" + i : "") + "|tag" + i + "|key" + i + "|child" + i, "readsOnOnePath");
    check(readsTwoValues(a, makeItem("x")), "tag" + i + "|tagx|key" + i + "|keyx", "readsTwoValues");
    check(readsAcrossKeyedRead(a, "flags"), "tag" + i + "|flags" + i + "|tag" + i + "|key" + i, "readsAcrossKeyedRead");
    if (i == 100 && usesFamilies) {
        check(aotFamilyOf(a) > 0, true, "an item has a family");
        check(aotHasDepartedFamily(aotFamilyOf(a)), false, "a member has left the family before any could");
        if (options.useAOTOperationCounters) {
            check(aotOperationCount("Family::guard:passes") > 1000, true, "guards on the family and its byte pass");
            check(aotOperationCount("Family::guard:exits-departed") || 0, 0, "guards that left because a member had left, before any had");
        }
    }
    if (i >= 100)
        check(readsAcrossDelete(makeItem(i)), "tag" + i + "|undefined|key" + i + "|child" + i, "readsAcrossDelete");
    check(readsAcrossConversion(a, i), "tag" + i + "|" + i + "|tag" + i + "|key" + i, "readsAcrossConversion");
    check(readsInLoop(a, 2), "tag" + i + "tag" + i + "tag" + i + "|key" + i + "|child" + i, "readsInLoop");
    check(readsMergedValue(a, makeItem("y"), i & 1), "tag" + i + "tagy|" + (i & 1 ? "tag" + i + "|key" + i : "tagy|keyy"), "readsMergedValue");
    kept.length = 0;
}

{
    const same = makeItem("s");
    check(readsAcrossStoreToAnother(same, same), "tags|stored|keys", "the other object is the same object: the slot is read again");
}

const changes = {
    deletes: [a => { delete a.tag; }, "undefined"],
    definesGetter: [a => { Object.defineProperty(a, "tag", { get() { return "got"; }, configurable: true }); }, "got"],
    stores: [a => { a.tag = "changed"; }, "changed"],
    deletesAndAdds: [a => { delete a.tag; a.tag = "again"; }, "again"],
    deletesAnother: [a => { delete a.sibling; }, "tagc"],
    makesDictionary: [a => { for (let i = 0; i < 200; ++i) a["extra" + i] = i; delete a.extra7; }, "tagc"],
    freezes: [a => { Object.freeze(a); }, "tagc"],
    changesPrototype: [a => { Object.setPrototypeOf(a, { tag: "inherited" }); }, "tagc"],
    deletesUnderPrototype: [a => { Object.setPrototypeOf(a, { tag: "inherited" }); delete a.tag; }, "inherited"],
    usesAsPrototype: [a => { keep(Object.create(a)); }, "tagc"],
};
for (const [name, [change, expected]] of Object.entries(changes)) {
    for (let round = 0; round < 3; ++round)
        check(readsAcrossCall(makeItem("c"), change), "tagc|" + expected + "|keyc|childc", "readsAcrossCall, the callee " + name);
}
{
    const target = makeItem("t");
    const other = { toString() { delete target.tag; return "text"; } };
    check(readsAcrossConversion(target, other), "tagt|text|undefined|keyt", "a conversion that deletes the property");
    const keyed = makeItem("k");
    Object.defineProperty(keyed, "trap", { get() { delete this.tag; return "sprung"; }, configurable: true });
    check(readsAcrossKeyedRead(keyed, "trap"), "tagk|sprung|undefined|keyk", "a keyed read that deletes the property");
}

let gets = 0;
const variants = {
    otherOrder: () => build(["key", "K"], ["tag", "T"], ["child", "C"]),
    withGetter: () => Object.defineProperty(build(["key", "K"], ["child", "C"]), "tag", { get() { ++gets; return "T"; }, set(value) { }, configurable: true }),
    inherits: () => keep(Object.create(build(["tag", "T"], ["key", "K"], ["child", "C"]))),
    proxy: () => new Proxy(makeItem("p"), { get(target, name) { return name === "tag" ? "T" : name === "key" ? "K" : "C"; }, set() { return true; } }),
    dictionary: () => { const o = build(["tag", "T"], ["key", "K"], ["child", "C"]); for (let i = 0; i < 200; ++i) o["extra" + i] = i; delete o.extra3; return o; },
};
for (const [name, make] of Object.entries(variants)) {
    for (let round = 0; round < 3; ++round) {
        gets = 0;
        check(readsTwice(make()), "T|K|T", "readsTwice of an object that " + name);
        if (name === "withGetter")
            check(gets, 2, "the getter runs once for each read");
        check(readsOnBothPaths(make(), round & 1), "T" + (round & 1 ? "1" : "2") + "|T|K", "readsOnBothPaths of an object that " + name);
        check(readsAcrossVariables(make()), "T|outer|T|K", "readsAcrossVariables of an object that " + name);
    }
}
check(storesThenReads(build(["key", "K"], ["child", "C"]), "added"), "added|K|C", "a store that adds the property");
check(storesThenReads(variants.withGetter(), "ignored"), "T|K|C", "a store that meets a setter");
check(storesThenReads(Object.freeze(makeItem("f")), "ignored"), "tagf|keyf|childf", "a store to a frozen object in sloppy code");
check(readsTwice("text"), "undefined|undefined|undefined", "a string");
check(readsTwice(7), "undefined|undefined|undefined", "a number");
check(thrownBy(() => readsTwice(null)), "TypeError", "null");
check(thrownBy(() => readsTwice(undefined)), "TypeError", "undefined");
check(thrownBy(() => storesThenReads(null, 1)), "TypeError", "a store to null");
check(readsMergedValue(makeItem("m"), variants.otherOrder(), false), "tagmT|T|K", "the merged value is of another shape");

if (usesFamilies) {
    check(aotHasDepartedFamily(aotFamilyOf(makeItem("last"))), true, "a member has left the family");
    if (options.useAOTOperationCounters)
        check(aotOperationCount("Family::guard:exits-departed") > 100, true, "guards leave because a member has left");
}

const repeated = [{ nothing: ["tag"], byte: [], "name-of-known-cell": ["key"] }, { nothing: ["key", "tag"], byte: [] }];
checks(readsTwice, true, ...repeated);
checks(storesThenReads, true, { nothing: ["tag"], byte: [], "name-of-known-cell": ["child", "key"] }, { nothing: ["child", "key", "tag"], byte: [] });
checks(readsThenStoresTwice, true, ...repeated);
checks(readsAcrossStoreToAnother, true, ...repeated);
checks(readsOnBothPaths, true, ...repeated);
checks(readsAcrossVariables, true, ...repeated);
checks(readsAcrossClosure, true, ...repeated);
checks(readsAcrossArray, true, ...repeated);
checks(readsAcrossLiteral, true, ...repeated);
checks(readsAcrossComparison, true, ...repeated);

const interrupted = [{ nothing: [], byte: [], "name-of-known-cell": ["child", "key", "tag"] }, { nothing: ["child", "key"], byte: ["tag"] }];
const interruptedWithoutChild = [{ nothing: [], byte: [], "name-of-known-cell": ["key", "tag"] }, { nothing: ["key"], byte: ["tag"] }];
checks(readsAcrossCall, true, ...interrupted);
checks(readsAcrossAddedProperty, true, ...interrupted);
checks(readsAcrossDelete, true, ...interrupted);
checks(readsAcrossKeyedRead, true, ...interruptedWithoutChild);
checks(readsAcrossConversion, true, ...interruptedWithoutChild);
checks(readsAcrossConstruct, false, { nothing: [] }, { byte: ["tag"] });
checks(readsOnOnePath, true, { nothing: [], byte: [], "name-of-known-cell": ["child", "key"] }, { nothing: ["child", "key"], byte: [] });
checks(readsTwoValues, true, { nothing: [], byte: [], "name-of-known-cell": ["key"] }, { nothing: ["key"], byte: [] });
checks(readsMergedValue, true, { nothing: [], byte: [], "name-of-known-cell": ["key"] }, { nothing: ["key"], byte: [] });
checks(readsInLoop, false, { nothing: [], byte: [] }, { nothing: ["child"], byte: ["key", "tag"] });
