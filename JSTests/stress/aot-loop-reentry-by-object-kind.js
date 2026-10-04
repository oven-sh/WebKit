//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(f.name);
    if (!remarks && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
    }
}
function readsProperty(o) { return o.property; }
readsProperty({ property: 1 });
const usesDataStubs = (remarksOf(readsProperty) || []).some(remark => /^calls:(GetById|ReadNameInSlot\d+)$/.test(remark));
const one = { one: 1 };

function mapOrSet(o, start) { const other = new Set(); let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, other); let x = new Map(); let hits = 0; for (let i = start | 0; i < 6; i++) { if (x === other) hits += o.one; x = a[i]; } return hits; }
function setOrWeakMap(o, start) { const other = new WeakMap(); let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, other); let x = new Set(); let hits = 0; for (let i = start | 0; i < 6; i++) { if (x === other) hits += o.one; x = a[i]; } return hits; }
function weakMapOrWeakSet(o, start) { const other = new WeakSet(); let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, other); let x = new WeakMap(); let hits = 0; for (let i = start | 0; i < 6; i++) { if (x === other) hits += o.one; x = a[i]; } return hits; }
function weakSetOrRegExp(o, start) { const other = /a/; let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, other); let x = new WeakSet(); let hits = 0; for (let i = start | 0; i < 6; i++) { if (x === other) hits += o.one; x = a[i]; } return hits; }
function regExpOrPromise(o, start) { const other = new Promise(() => { }); let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, other); let x = /a/; let hits = 0; for (let i = start | 0; i < 6; i++) { if (x === other) hits += o.one; x = a[i]; } return hits; }
function promiseOrDate(o, start) { const other = new Date(0); let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, other); let x = new Promise(() => { }); let hits = 0; for (let i = start | 0; i < 6; i++) { if (x === other) hits += o.one; x = a[i]; } return hits; }
function dateOrError(o, start) { const other = new Error("e"); let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, other); let x = new Date(0); let hits = 0; for (let i = start | 0; i < 6; i++) { if (x === other) hits += o.one; x = a[i]; } return hits; }
function errorOrArrayBuffer(o, start) { const other = new ArrayBuffer(8); let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, other); let x = new Error("e"); let hits = 0; for (let i = start | 0; i < 6; i++) { if (x === other) hits += o.one; x = a[i]; } return hits; }
function arrayBufferOrDataView(o, start) { const other = new DataView(new ArrayBuffer(8)); let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, other); let x = new ArrayBuffer(8); let hits = 0; for (let i = start | 0; i < 6; i++) { if (x === other) hits += o.one; x = a[i]; } return hits; }
function dataViewOrStringObject(o, start) { const other = new String("s"); let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, other); let x = new DataView(new ArrayBuffer(8)); let hits = 0; for (let i = start | 0; i < 6; i++) { if (x === other) hits += o.one; x = a[i]; } return hits; }
function stringObjectOrMap(o, start) { const other = new Map(); let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, other); let x = new String("s"); let hits = 0; for (let i = start | 0; i < 6; i++) { if (x === other) hits += o.one; x = a[i]; } return hits; }
for (let f of [mapOrSet, setOrWeakMap, weakMapOrWeakSet, weakSetOrRegExp, regExpOrPromise, promiseOrDate, dateOrError, errorOrArrayBuffer, arrayBufferOrDataView, dataViewOrStringObject, stringObjectOrMap]) {
    noInline(f);
    for (let start = 0; start < 3; start++)
        check(f(one, start), 1, f.name + " from " + start);
    for (let start = 3; start < 7; start++)
        check(f(one, start), 0, f.name + " from " + start);
    if (usesDataStubs)
        applies(f, "split-loop");
}

function sameMapAgain(o, start) { const first = new Map(); let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, first); Reflect.set(a, 4, first); let x = first; let hits = 0; for (let i = start | 0; i < 6; i++) { if (x === first) hits += o.one; x = a[i]; } return hits; }
noInline(sameMapAgain);
check(sameMapAgain(one, 0), 3, "a map where a map is expected");
if (usesDataStubs)
    applies(sameMapAgain, "split-loop");

function callableWhereMapIsExpected(o, other, start) { let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, other); let x = new Map(); let hits = 0; for (let i = start | 0; i < 6; i++) { if (typeof x === "function") hits += o.one; x = a[i]; } return hits; }
function truthyWhereFunctionIsExpected(o, other, start) { let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, other); let x = new Function("return 1"); let hits = 0; for (let i = start | 0; i < 6; i++) { if (typeof x !== "number") { if (x) hits += o.one; } x = a[i]; } return hits; }
function callableWhereFunctionIsExpected(o, other, start) { let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, other); let x = new Function("return 1"); let hits = 0; for (let i = start | 0; i < 6; i++) { if (typeof x !== "number") { if (typeof x === "function") hits += o.one; } x = a[i]; } return hits; }
function undefinedWhereFunctionIsExpected(o, other, start) { let k = 1; const a = [k, k, k, k, k, k]; Reflect.set(a, 2, other); let x = new Function("return 1"); let hits = 0; for (let i = start | 0; i < 6; i++) { if (typeof x !== "number") { if (x == null) hits += o.one; } x = a[i]; } return hits; }
const expecting = [callableWhereMapIsExpected, truthyWhereFunctionIsExpected, callableWhereFunctionIsExpected, undefinedWhereFunctionIsExpected];
for (let f of expecting)
    noInline(f);
const intruders = [
    ["a proxy of a function", new Proxy(function () { }, { }), 1, 2, 2, 0],
    ["a proxy of an object", new Proxy({ }, { }), 0, 2, 1, 0],
    ["a function that masquerades as undefined", makeMasquerader(), 0, 1, 1, 1],
    ["a function that masquerades as undefined in another realm", createGlobalObject().makeMasquerader(), 1, 2, 2, 0],
    ["a constructor of a built-in class", Map, 1, 2, 2, 0],
    ["a native function", Math.max, 1, 2, 2, 0],
    ["a closure", () => 1, 1, 2, 2, 0],
    ["a class", class { }, 1, 2, 2, 0],
    ["a bound function", (function () { }).bind(null), 1, 2, 2, 0],
    ["a map", new Map(), 0, 2, 1, 0],
    ["an object", { }, 0, 2, 1, 0],
    ["an array", [], 0, 2, 1, 0],
    ["a typed array", new Uint8Array(1), 0, 2, 1, 0],
    ["a wrapped number", new Number(0), 0, 2, 1, 0],
    ["a number", 5, 0, 1, 1, 0],
    ["undefined", undefined, 0, 1, 1, 1],
    ["null", null, 0, 1, 1, 1],
    ["false", false, 0, 1, 1, 0],
    ["a string", "s", 0, 2, 1, 0],
    ["an empty string", "", 0, 1, 1, 0],
    ["a symbol", Symbol.iterator, 0, 2, 1, 0],
    ["a BigInt", 0n, 0, 1, 1, 0],
];
for (let [what, value, ...expected] of intruders) {
    for (let i = 0; i < expecting.length; i++)
        check(expecting[i](one, value, 0), expected[i], expecting[i].name + " with " + what);
}
for (let f of expecting)
    if (usesDataStubs)
        applies(f, "split-loop");
