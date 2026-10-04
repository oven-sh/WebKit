//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTInlining=0", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTDataStubs=0", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--validateAOTInferredTypes=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1")
//@ runDefault("--useDollarVM=1")

const failures = [];

function check(actual, expected, what)
{
    if (!Object.is(actual, expected))
        failures.push(what + ": " + String(actual) + " instead of " + String(expected));
}

function hasOwn(o, k) { return Object.hasOwn(o, k); }
function inByName(o) { return "wanted" in o; }
noInline(hasOwn);
noInline(inByName);

const isCounting = typeof aotOperationCount === "function" && !!aotRemarks("check") && !!jscOptions().useAOTOperationCounters;
const keys = ["key-is-constant", "key-is-symbol", "key-is-atom", "key-is-string-but-no-atom", "key-is-rope", "key-is-number", "key-is-index-string", "key-is-other"];
const receivers = ["receiver-is-not-object", "receiver-is-not-final", "receiver-is-dictionary", "receiver-has-field-ids", "receiver-has-out-of-line-properties", "receiver-has-properties-beyond-named-slots", "receiver-is-all-in-named-slots"];
const inSlot = ["property-has-id-in-slot", "property-is-in-slot-not-known-yet", "property-is-in-slot-marked-nameless", "property-is-in-slot-of-structure-that-records-nothing"];
const absentUnderObjectPrototype = ["property-is-absent-under-object-prototype", "property-is-absent-under-object-prototype-and-every-name-is-recorded", "property-may-be-on-prototype-that-cannot-be-asked"];
const absentUnderAnotherChain = ["property-is-absent-under-another-chain", "property-is-absent-under-another-chain-and-every-name-is-recorded"];
const inherited = ["value", "accessor"].flatMap(kind => ["at-depth-1", "at-depth-2", "deeper"].map(depth => "property-is-inherited-" + kind + "-" + depth));
const places = ["property-is-unknown", "property-has-attributes", "property-is-out-of-line", "property-is-beyond-named-slots", ...inSlot, ...absentUnderObjectPrototype, ...absentUnderAnotherChain, ...inherited];

function counts(operation)
{
    const result = { arrivals: aotOperationCount(operation) || 0, guests: aotOperationCount("guest:named-access") || 0 };
    for (const detail of [...keys, ...receivers, ...places])
        result[detail] = aotOperationCount(operation + ":" + detail) || 0;
    return result;
}

function isQuiet(operation)
{
    const before = counts(operation), after = counts(operation);
    for (const name in before) {
        if (before[name] !== after[name])
            return false;
    }
    return true;
}

function expect(operation, what, run, result, key, allowedReceivers, allowedPlaces, rounds = 3, mayStayAway = false)
{
    for (let round = 0; round < rounds; ++round) {
        if (isCounting) {
            let attempts = 0;
            while (!isQuiet(operation) && ++attempts < 100) { }
            check(attempts < 100, true, what + ": the bookkeeping itself comes to rest");
        }
        const before = isCounting ? counts(operation) : null;
        check(run(), result, what);
        if (!isCounting)
            continue;
        const after = counts(operation);
        const changed = list => list.filter(detail => after[detail] !== before[detail]);
        if (mayStayAway && after.arrivals === before.arrivals)
            continue;
        check(after.arrivals - before.arrivals, 1, what + ": one arrival");
        check(after.guests - before.guests, 1, what + ": one row");
        check(changed(keys).join(), key, what + ": the key");
        check(changed(receivers).length, 1, what + ": one receiver");
        check(allowedReceivers.includes(changed(receivers)[0]), true, what + ": the receiver is " + changed(receivers)[0]);
        check(changed(places).length, 1, what + ": one place");
        check(allowedPlaces.includes(changed(places)[0]), true, what + ": the place is " + changed(places)[0]);
        for (const detail of [...changed(keys), ...changed(receivers), ...changed(places)])
            check(after[detail] - before[detail], 1, what + ": " + detail + " once");
    }
}

const symbol = Symbol("s");
const small = { wanted: 1, other: 2 };
const symbolic = { };
symbolic[symbol] = 3;
const allInline = ["receiver-is-all-in-named-slots"], outOfLine = ["receiver-has-out-of-line-properties", "receiver-is-dictionary"];
const grown = { };
for (let i = 0; i < 40; ++i)
    grown["g" + i] = i;
const dictionary = { d8: 1, d9: 2 };
$vm.toCacheableDictionary(dictionary);
const withGetter = { get wanted() { return 1; } };
const suffix = String(Date.now()).length > 0 ? "ted" : "";
const operation = "operationAOTHasOwnProperty";

expect(operation, "an atom that is there", () => hasOwn(small, "wanted"), true, "key-is-atom", allInline, inSlot);
expect(operation, "an atom that is not there", () => hasOwn(small, "missing"), false, "key-is-atom", allInline, absentUnderObjectPrototype);
expect(operation, "a symbol", () => hasOwn(symbolic, symbol), true, "key-is-symbol", allInline, inSlot);
expect(operation, "a rope", () => hasOwn(small, "wan" + suffix), true, "key-is-rope", allInline, ["property-is-unknown"]);
expect(operation, "a number", () => hasOwn(small, 5), false, "key-is-number", allInline, ["property-is-unknown"]);
expect(operation, "an index as a string", () => hasOwn(small, "5"), false, "key-is-index-string", allInline, ["property-is-unknown"]);
expect(operation, "undefined as the key", () => hasOwn(small, undefined), false, "key-is-other", allInline, ["property-is-unknown"]);
expect(operation, "an accessor", () => hasOwn(withGetter, "wanted"), true, "key-is-atom", allInline, ["property-has-attributes"]);
expect(operation, "the last property of an object that grew", () => hasOwn(grown, "g39"), true, "key-is-atom", outOfLine, ["property-is-out-of-line", ...inSlot]);
expect(operation, "the first property of an object that grew", () => hasOwn(grown, "g0"), true, "key-is-atom", outOfLine, inSlot);
expect(operation, "a dictionary", () => hasOwn(dictionary, "d8"), true, "key-is-atom", ["receiver-is-dictionary"], inSlot);
expect(operation, "an array", () => hasOwn([1], "wanted"), false, "key-is-atom", ["receiver-is-not-final"], ["property-is-unknown"]);
expect(operation, "a function", () => hasOwn(check, "wanted"), false, "key-is-atom", ["receiver-is-not-final"], ["property-is-unknown"]);
expect("operationAOTInById", "in, there", () => inByName(small), true, "key-is-constant", allInline, inSlot, 1);
expect("operationAOTInById", "in, not there", () => inByName({ other: 1 }), false, "key-is-constant", allInline, absentUnderObjectPrototype, 1);
expect("operationAOTInById", "in an array", () => inByName([]), false, "key-is-constant", ["receiver-is-not-final"], ["property-is-unknown"], 1);

function get(o) { return o.wanted; }
noInline(get);
class Base { get wantedByGetter() { return 1; } }
Base.prototype.wanted = "base";
class Derived extends Base { constructor() { super(); this.own = 1; } }
class MoreDerived extends Derived { }
class WithGetter { constructor() { this.own = 1; } get wanted() { return "got"; } }
const far = { };
for (let i = 0; i < 40; ++i)
    far["f" + i] = i;
far["wan" + suffix] = "far";
const reads = "operationAOTGetById";
expect(reads, "read, own", () => get({ unrelated: 1, wanted: "own" }), "own", "key-is-constant", allInline, inSlot, 1, true);
expect(reads, "read, out of line", () => get(far), "far", "key-is-constant", outOfLine, ["property-is-out-of-line"], 1, true);
expect(reads, "read, an own accessor", () => get(withGetter), 1, "key-is-constant", allInline, ["property-has-attributes"], 1, true);
expect(reads, "read, inherited at depth 1", () => get(new Base), "base", "key-is-constant", allInline, ["property-is-inherited-value-at-depth-1"], 1, true);
expect(reads, "read, inherited at depth 2", () => get(new Derived), "base", "key-is-constant", allInline, ["property-is-inherited-value-at-depth-2"], 1, true);
expect(reads, "read, inherited deeper", () => get(new MoreDerived), "base", "key-is-constant", allInline, ["property-is-inherited-value-deeper"], 1, true);
expect(reads, "read, an inherited accessor", () => get(new WithGetter), "got", "key-is-constant", allInline, ["property-is-inherited-accessor-at-depth-1"], 1, true);
expect(reads, "read, absent", () => get({ unrelated: 2, another: 3 }), undefined, "key-is-constant", allInline, absentUnderObjectPrototype, 1, true);
expect(reads, "read, absent without a prototype", () => get(Object.create(null)), undefined, "key-is-constant", allInline, absentUnderAnotherChain, 1, true);
expect(reads, "read from a dictionary", () => get(dictionary), undefined, "key-is-constant", ["receiver-is-dictionary"], absentUnderObjectPrototype, 1, true);
expect(reads, "read from an array", () => get([]), undefined, "key-is-constant", ["receiver-is-not-final"], ["property-is-unknown"], 1, true);
expect(reads, "read from a number", () => get(5), undefined, "key-is-constant", ["receiver-is-not-object"], ["property-is-unknown"], 1, true);

if (failures.length)
    throw new Error(failures.length + " failures:\n" + [...new Set(failures)].slice(0, 30).join("\n"));
