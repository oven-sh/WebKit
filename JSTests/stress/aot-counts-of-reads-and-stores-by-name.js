//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTInlining=0", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--validateAOTInferredTypes=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1")
//@ runDefault("--useDollarVM=1")

const failures = [];

function check(actual, expected, what)
{
    if (!Object.is(actual, expected))
        failures.push(what + ": " + String(actual) + " instead of " + String(expected));
}

const options = typeof jscOptions === "function" ? jscOptions() : { };
const isCounting = typeof aotOperationCount === "function" && typeof aotRemarks === "function" && !!aotRemarks("check") && !!options.useAOTOperationCounters && !!options.useAOTDataStubs;
const count = name => aotOperationCount(name) || 0;
const withTable = name => [name + "-and-table-hits", name + "-and-table-misses"];

const inSlot = ["property-has-id-in-slot", "property-is-in-slot-not-known-yet", "property-is-in-slot-marked-nameless", "property-is-in-slot-of-structure-that-records-nothing"];
const absent = ["property-is-absent-under-object-prototype", "property-may-be-on-prototype-that-cannot-be-asked"];
const places = ["property-is-unknown", "property-has-attributes", "property-is-out-of-line", "property-is-beyond-named-slots", ...inSlot, ...absent, "property-is-absent-under-another-chain",
    ...["value", "accessor"].flatMap(kind => ["at-depth-1", "at-depth-2", "deeper"].map(depth => "property-is-inherited-" + kind + "-" + depth))];
const polymorphic = [0, 1, 2, 3, 4].flatMap(n => ["polymorphic-site-of-" + n + "-hits-by-name", "polymorphic-site-of-" + n + "-hits", ...withTable("polymorphic-site-of-" + n + "-misses")]);
const readServices = ["site-hits", "site-hits-indirectly", "site-hits-by-name", ...polymorphic, ...["site-has-nobodys-slot", "site-holds-another-structure", "site-is-abandoned", "site-is-empty"].flatMap(withTable)];
const storeOutcomes = ["store-to-no-object", "store-to-object-that-is-not-final", "store-to-dictionary", "store-calls-own-setter", "store-is-refused-by-own-read-only-property", "store-replaces-out-of-line", "store-replaces-beyond-named-slots",
    "store-replaces-in-slot-with-id", "store-replaces-in-slot-without-id", "store-may-meet-prototype-that-cannot-be-asked", "store-calls-inherited-setter", "store-is-refused-by-inherited-read-only-property", "store-to-object-that-is-not-extensible",
    "store-adds-inline", "store-adds-out-of-line", "store-adds-out-of-line-and-grows-storage", "store-redefines-property-with-attributes", "store-is-refused-by-own-read-only-property-and-throws",
    "store-is-refused-by-inherited-read-only-property-and-throws", "store-to-object-that-is-not-extensible-and-throws"];
const storeTables = ["-and-table-misses", "-and-table-hits", "-and-table-hits-and-reallocates", "-and-table-hits-and-allocates-storage"];
const storeServices = ["-at-direct-store", "-behind-another-path", "-to-object-born-here", "-with-constant-key", "-without-guess"].flatMap(site => ["store-site-hits", "store-site-hits-with-transition", "store-site-hits-typed-field",
    ...["store-site-has-nobodys-slot", "store-site-holds-another-structure", "store-site-is-abandoned", "store-site-is-empty"].flatMap(state => storeTables.map(table => state + table))].map(service => service + site));

function snapshot(names, into)
{
    for (let i = 0; i < names.length; ++i)
        into[i] = aotOperationCount(names[i]) || 0;
}

function expect(what, operation, outcomes, services, run, result, allowedOutcomes, allowedServices, outcomesOfPair = allowedOutcomes)
{
    if (!isCounting) {
        check(run(), result, what);
        return;
    }
    const names = [...outcomes.map(name => operation + ":" + name), ...services.map(name => operation + ":" + name), ...outcomesOfPair.flatMap(outcome => services.map(service => outcome + ":" + service))];
    const before = new Array(names.length).fill(0), after = new Array(names.length).fill(0);
    snapshot(names, before);
    snapshot(names, after);
    snapshot(names, before);
    const actual = run();
    snapshot(names, after);
    check(actual, result, what);
    const changed = (from, list) => list.filter((name, i) => after[from + i] !== before[from + i]);
    const seenOutcomes = changed(0, outcomes), seenServices = changed(outcomes.length, services);
    check(seenOutcomes.length, 1, what + ": one outcome (" + seenOutcomes.join() + ")");
    check(seenServices.length, 1, what + ": one way to be served (" + seenServices.join() + ")");
    check(allowedOutcomes.includes(seenOutcomes[0]), true, what + ": the outcome is " + seenOutcomes[0]);
    if (allowedServices)
        check(allowedServices.includes(seenServices[0]), true, what + ": it is served as " + seenServices[0]);
    let pairs = 0;
    for (let i = outcomes.length + services.length; i < names.length; ++i)
        pairs += after[i] - before[i];
    check(pairs, 1, what + ": one row for the pair");
}

const wanted = "wan" + "ted".slice(0), stored = "sto" + "red".slice(0);
const withName = (name, value, before = 0) => { const o = { }; for (let i = 0; i < before; ++i) o["b" + i] = i; o[name] = value; return o; };
const reads = (what, f, o, result, allowedOutcomes, allowedServices, outcomesOfPair) => expect(what, "operationAOTCountReadByName", places, readServices, () => f(o), result, allowedOutcomes, allowedServices, outcomesOfPair);
const stores = (what, f, o, allowedOutcomes, allowedServices) => expect(what, "operationAOTCountStoreByName", storeOutcomes, storeServices, () => { f(o, 7); return 0; }, 0, allowedOutcomes, allowedServices);

function getPresent(o) { return o.wanted; }
function getAbsent(o) { return o.wanted; }
function getInherited(o) { return o.wanted; }
function getSeveral(o) { return o.wanted; }
function getOnce(o) { return o.wanted; }
function setReplacing(o, v) { o.stored = v; }
function setAdding(o, v) { o.stored = v; }
function setOnce(o, v) { o.stored = v; }
for (const f of [getPresent, getAbsent, getInherited, getSeveral, getOnce, setReplacing, setAdding, setOnce])
    noInline(f);

{
    const o = withName(wanted, 1);
    for (let i = 0; i < 300; ++i)
        getPresent(o);
    reads("a warm read of one object", getPresent, o, 1, inSlot, ["site-hits", "site-hits-by-name"]);
}
{
    const o = withName("other", 1);
    for (let i = 0; i < 300; ++i)
        getAbsent(o);
    reads("a warm read of what is not there", getAbsent, o, undefined, absent, ["site-hits-indirectly"]);
}
{
    function Made() { this.own = 1; }
    Made.prototype[wanted] = "inherited";
    const o = new Made;
    for (let i = 0; i < 300; ++i)
        getInherited(o);
    reads("a warm read from the prototype", getInherited, o, "inherited", ["property-is-inherited-value-at-depth-1"], ["site-hits-indirectly"]);
}
{
    const objects = [withName(wanted, 1, 0), withName(wanted, 1, 1), withName(wanted, 1, 2)];
    for (let i = 0; i < 300; ++i)
        getSeveral(objects[i % 3]);
    reads("a warm read of three shapes", getSeveral, objects[0], 1, inSlot, polymorphic);
}
reads("a read out of line", getOnce, withName(wanted, 2, 30), 2, ["property-is-out-of-line"]);
reads("a read of an accessor", getOnce, Object.defineProperty({ }, wanted, { get() { return 3; } }), 3, ["property-has-attributes"]);
reads("a read without a prototype", getOnce, Object.create(null), undefined, ["property-is-absent-under-another-chain"]);
reads("a read from an array", getOnce, [], undefined, ["property-is-unknown"], undefined, ["receiver-is-not-final"]);
reads("a read from a number", getOnce, 5, undefined, ["property-is-unknown"], undefined, ["receiver-is-not-object"]);

{
    const o = withName(stored, 1);
    for (let i = 0; i < 300; ++i)
        setReplacing(o, i);
    stores("a warm store that replaces", setReplacing, o, ["store-replaces-in-slot-with-id", "store-replaces-in-slot-without-id"], ["store-site-hits-without-guess"]);
    check(o.stored, 7, "the value stored");
}
{
    for (let i = 0; i < 300; ++i)
        setAdding(withName("first", 1), i);
    const o = withName("first", 1);
    stores("a warm store that adds", setAdding, o, ["store-adds-inline"], ["store-site-hits-with-transition-without-guess"]);
    check(o.stored, 7, "the value added");
}
stores("a store out of line", setOnce, withName(stored, 1, 30), ["store-replaces-out-of-line"]);
stores("a store that adds out of line", setOnce, withName("first", 1, 30), ["store-adds-out-of-line", "store-adds-out-of-line-and-grows-storage"]);
let got = 0;
stores("a store to an own setter", setOnce, Object.defineProperty({ }, stored, { set(v) { got += v; } }), ["store-calls-own-setter"]);
stores("a store to an inherited setter", setOnce, Object.create(Object.defineProperty({ }, stored, { set(v) { got += v; } })), ["store-calls-inherited-setter"]);
check(got, 14, "both setters ran once");
stores("a store to a read-only property", setOnce, Object.defineProperty({ }, stored, { value: 1, writable: false }), ["store-is-refused-by-own-read-only-property"]);
stores("a store under a read-only property", setOnce, Object.create(Object.defineProperty({ }, stored, { value: 1, writable: false })), ["store-is-refused-by-inherited-read-only-property"]);
stores("a store to a frozen object", setOnce, Object.freeze(withName(stored, 1)), ["store-is-refused-by-own-read-only-property"]);
stores("a store to an object that cannot be extended", setOnce, Object.preventExtensions({ }), ["store-to-object-that-is-not-extensible"]);
{
    const dictionary = withName(stored, 1);
    $vm.toCacheableDictionary(dictionary);
    stores("a store to a dictionary", setOnce, dictionary, ["store-to-dictionary"]);
}
stores("a store to an array", setOnce, [], ["store-to-object-that-is-not-final"]);
stores("a store to a number", setOnce, 5, ["store-to-no-object"]);
{
    const before = count("operationAOTCountStoreByName:store-without-guess");
    setOnce({ }, 1);
    if (isCounting)
        check(count("operationAOTCountStoreByName:store-without-guess") - before, 1, "the kind of site");
}
function setStrictly(o, v) { "use strict"; o.stored = v; }
noInline(setStrictly);
expect("a strict store to a frozen object", "operationAOTCountStoreByName", storeOutcomes, storeServices, () => { try { setStrictly(Object.freeze(withName(stored, 1)), 7); } catch (error) { return error.constructor.name; } return "none"; }, "TypeError", ["store-is-refused-by-own-read-only-property-and-throws"]);

if (failures.length)
    throw new Error(failures.length + " failures:\n" + [...new Set(failures)].slice(0, 30).join("\n"));
