//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTDataStubs=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1")
//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault

const failures = [];

function check(actual, expected, what)
{
    if (!Object.is(actual, expected))
        failures.push(what + ": " + String(actual) + " instead of " + String(expected));
}

function store(o, k, v) { o[k] = v; }
function storeStrictly(o, k, v) { "use strict"; o[k] = v; }
function load(o, k) { return o[k]; }
for (const f of [store, storeStrictly, load])
    noInline(f);

const isCounting = typeof aotOperationCount === "function" && !!aotRemarks("check") && !!jscOptions().useAOTOperationCounters;
const arrivals = () => isCounting ? aotOperationCount("operationAOTPutByVal") || 0 : 0;
const errorOf = run => { try { run(); } catch (error) { return error.constructor.name; } return "none"; };

const state = Symbol("state"), registered = Symbol.for("registered"), nameless = Symbol();

for (const numberOfShapes of [1, 4, 40]) {
    for (const key of [state, registered, nameless, Symbol.toStringTag, "name"]) {
        const objects = [];
        for (let i = 0; i < numberOfShapes; ++i) {
            const o = { };
            o["p" + i] = i;
            o[key] = -1;
            objects.push(o);
        }
        for (let i = 0; i < 2000; ++i)
            store(objects[i % numberOfShapes], key, i);
        const before = arrivals();
        for (let i = 0; i < 10000; ++i)
            store(objects[i % numberOfShapes], key, i);
        const what = numberOfShapes + " shapes, key " + String(key);
        if (isCounting)
            check(arrivals() - before < 100, true, what + ": warm stores that replace stay out of the operation (" + (arrivals() - before) + " of 10000 arrive)");
        for (let i = 0; i < numberOfShapes; ++i)
            check(load(objects[(9999 - i) % numberOfShapes], key), 9999 - i, what + ": the value stored last");
    }
}

{
    const added = Symbol("added");
    function Made() { this.a = 1; this.b = 2; }
    const make = () => new Made;
    for (let i = 0; i < 2000; ++i)
        store(make(), added, i);
    const before = arrivals();
    let sum = 0;
    for (let i = 0; i < 10000; ++i) {
        const o = make();
        store(o, added, i);
        sum += o[added];
    }
    check(sum, 49995000, "stores that add a property");
    if (isCounting)
        check(arrivals() - before < 100, true, "warm stores that add a property stay out of the operation (" + (arrivals() - before) + " of 10000 arrive)");

    let seen = [];
    Object.defineProperty(Made.prototype, added, { set(v) { seen.push(v); }, configurable: true });
    const afterSetter = make();
    store(afterSetter, added, 7);
    check(seen.join(), "7", "a setter that appears on the prototype is called");
    check(Object.getOwnPropertySymbols(afterSetter).length, 0, "and nothing is added");
    Object.defineProperty(Made.prototype, added, { value: 1, writable: false, configurable: true });
    const afterReadOnly = make();
    store(afterReadOnly, added, 8);
    check(Object.getOwnPropertySymbols(afterReadOnly).length, 0, "a read-only property that appears on the prototype: nothing is added");
    check(errorOf(() => storeStrictly(make(), added, 9)), "TypeError", "and strict code throws");
    delete Made.prototype[added];
    const afterDelete = make();
    store(afterDelete, added, 10);
    check(afterDelete[added], 10, "after it is gone the property is added again");
}

{
    const key = Symbol("key");
    const warm = { x: 1, [key]: 0 };
    for (let i = 0; i < 3000; ++i)
        store(warm, key, i);
    const frozen = Object.freeze({ x: 1, [key]: 0 });
    store(frozen, key, 5);
    check(frozen[key], 0, "a frozen object");
    check(errorOf(() => storeStrictly(frozen, key, 5)), "TypeError", "a frozen object, strictly");
    const sealed = Object.seal({ x: 1, [key]: 0 });
    store(sealed, key, 5);
    check(sealed[key], 5, "a sealed object");
    const closed = Object.preventExtensions({ x: 1 });
    store(closed, key, 5);
    check(closed[key], undefined, "an object that cannot be extended");
    let got = 0;
    const withSetter = { x: 1, set [key](v) { got += v; } };
    for (let i = 0; i < 100; ++i)
        store(withSetter, key, 1);
    check(got, 100, "an own setter");
    const readOnly = Object.defineProperty({ x: 1 }, key, { value: 3, writable: false });
    store(readOnly, key, 5);
    check(readOnly[key], 3, "an own read-only property");
    const trapped = [];
    store(new Proxy({ }, { set(target, k, v) { trapped.push(typeof k + v); return true; } }), key, 5);
    check(trapped.join(), "symbol5", "a proxy");
    const dictionary = { };
    for (let i = 0; i < 300; ++i)
        dictionary["k" + i] = i;
    delete dictionary.k7;
    for (let i = 0; i < 100; ++i)
        store(dictionary, key, i);
    check(dictionary[key], 99, "a dictionary");
    const array = [1, 2];
    store(array, key, 5);
    check(array[key] + array.length, 7, "an array");
    const typed = new Int32Array(2);
    store(typed, key, 5);
    check(typed[key], 5, "a typed array");
    const f = function () { };
    store(f, key, 5);
    check(f[key], 5, "a function");
    store(5, key, 1);
    store("s", key, 1);
    check(errorOf(() => storeStrictly(5, key, 1)), "TypeError", "a number, strictly");
    check(errorOf(() => store(null, key, 1)), "TypeError", "null");
    check(errorOf(() => store(undefined, key, 1)), "TypeError", "undefined");
    const removed = { x: 1, [key]: 0 };
    store(removed, key, 1);
    delete removed[key];
    check(load(removed, key), undefined, "after delete");
    store(removed, key, 2);
    check(removed[key], 2, "stored again after delete");
    const twins = [Symbol("twin"), Symbol("twin")];
    const both = { [twins[0]]: 0, [twins[1]]: 0 };
    for (let i = 0; i < 3000; ++i)
        store(both, twins[i & 1], i);
    check(both[twins[0]] + "," + both[twins[1]], "2998,2999", "two symbols with one description");
}

if (failures.length)
    throw new Error(failures.length + " failures:\n" + [...new Set(failures)].join("\n"));
