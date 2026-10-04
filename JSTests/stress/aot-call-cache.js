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
function doesNotApply(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + f.name + ": " + remarks.join(" "));
    }
}
function repeat(f) {
    for (let i = 0; i < 200; i++)
        f(i);
}
function throwsTypeError(f, what) {
    try {
        f();
    } catch (error) {
        check(error instanceof TypeError, true, what);
        return;
    }
    throw new Error(what + ": did not throw");
}

function known(x) { return x + 1; }
function callsParameter(f, x) { return f(x); }
function callsParameterWithThis(o, x) { return o.method(x); }
function callsKnown(x) { return known(x); }
function callsWithManyArguments(f) { return f(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12); }
function constructs(F, x) { return new F(x); }
for (const f of [known, callsParameter, callsParameterWithThis, callsKnown, callsWithManyArguments, constructs])
    noInline(f);

let usesDataStubs = (remarksOf(callsParameterWithThis) || []).some(remark => /^calls:(GetById|ReadNameInSlot\d+)$/.test(remark));
if (usesDataStubs) {
    applies(callsParameter, "cached-call");
    applies(callsParameterWithThis, "cached-call");
}
doesNotApply(callsWithManyArguments, "cached-call");
doesNotApply(constructs, "cached-call");

function addOne(x) { return x + 1; }
function addTwo(x) { return x + 2; }
function twoParameters(x, y) { return String(x) + ":" + String(y); }
function noParameters() { return "none"; }
function restParameters(...all) { return all.length + ":" + all.join(); }
function usesArguments() { return arguments.length + ":" + arguments[0]; }
function withDefault(x, y = 5) { return x + y; }
function returnsThis() { "use strict"; return this; }
function* generator(x) { yield x; }
async function asynchronous(x) { return x; }
class Class { constructor(x) { this.x = x; } }

repeat(i => check(callsParameter(addOne, i), i + 1, "one callee"));
repeat(i => check(callsParameter(i & 1 ? addOne : addTwo, i), i + (i & 1 ? 1 : 2), "two callees"));

function callsSeveral(f, x) { return f(x); }
noInline(callsSeveral);
{
    const callees = [addOne, addTwo, twoParameters, noParameters, restParameters, usesArguments, withDefault, Math.abs, addOne.bind(null), String];
    const expected = [8, 9, "7:undefined", "none", "1:7", "1:7", 12, 7, 8, "7"];
    repeat(i => check(callsSeveral(callees[i % callees.length], 7), expected[i % callees.length], "callee " + i % callees.length + " of many"));
}

function callsPadded(f, x) { return f(x); }
function callsRest(f, x) { return f(x); }
function callsArguments(f, x) { return f(x); }
function callsHost(f, x) { return f(x); }
function callsBound(f, x) { return f(x); }
function callsProxy(f, x) { return f(x); }
function callsGenerator(f, x) { return f(x); }
function callsAsync(f, x) { return f(x); }
for (const f of [callsPadded, callsRest, callsArguments, callsHost, callsBound, callsProxy, callsGenerator, callsAsync])
    noInline(f);
repeat(() => check(callsPadded(twoParameters, 7), "7:undefined", "more parameters than arguments"));
repeat(() => check(callsRest(restParameters, 7), "1:7", "rest parameters"));
repeat(() => check(callsArguments(usesArguments, 7), "1:7", "the arguments object"));
repeat(() => check(callsHost(Math.abs, -7), 7, "a host function"));
repeat(() => check(callsBound(addOne.bind(null), 7), 8, "a bound function"));
repeat(() => check(callsProxy(new Proxy(addOne, { apply() { return "trapped"; } }), 7), "trapped", "a proxy"));
repeat(() => check(callsGenerator(generator, 7).next().value, 7, "a generator"));
repeat(() => check(callsAsync(asynchronous, 7) instanceof Promise, true, "an async function"));

function callsBad(f, x) { return f(x); }
noInline(callsBad);
repeat(() => check(callsBad(addOne, 1), 2, "before something that cannot be called"));
for (const bad of [undefined, null, 5, "text", { }, Symbol("s")])
    repeat(() => throwsTypeError(() => callsBad(bad, 1), "calling " + String(bad)));
repeat(() => throwsTypeError(() => callsBad(Class, 1), "calling a class"));
repeat(() => check(callsBad(addOne, 1), 2, "after something that cannot be called"));

{
    const first = { method: returnsThis }, second = { method: returnsThis };
    repeat(i => check(callsParameterWithThis(i & 1 ? first : second), i & 1 ? first : second, "the receiver"));
}

function callsClosure(f) { return f(); }
noInline(callsClosure);
repeat(i => check(callsClosure(() => i), i, "a new closure every time"));

function callsSurvivor(f) { return f(); }
noInline(callsSurvivor);
for (let round = 0; round < 6; round++) {
    (function () {
        const value = "round " + round;
        const closure = () => value;
        repeat(() => check(callsSurvivor(closure), value, "a closure that is about to die"));
    })();
    fullGC();
    const fillers = [];
    for (let i = 0; i < 2000; i++)
        fillers.push(() => "filler " + i);
    repeat(i => check(callsSurvivor(fillers[i]), "filler " + i, "closures made after a collection"));
    edenGC();
}

function callsRecursively(f, n) { return n ? f(f, n - 1) + 1 : 0; }
noInline(callsRecursively);
check(callsRecursively(callsRecursively, 100), 100, "recursion");

function callsOtherRealm(f, x) { return f(x); }
noInline(callsOtherRealm);
{
    const other = createGlobalObject().Function("x", "return x + 100");
    repeat(i => check(callsOtherRealm(i & 1 ? other : addOne, 1), i & 1 ? 101 : 2, "a function of another realm"));
}

function throwing(x) { throw new RangeError("thrown " + x); }
function callsThrowing(f, x) { try { return f(x); } catch (error) { return error.message; } }
noInline(callsThrowing);
repeat(i => check(callsThrowing(throwing, i), "thrown " + i, "a callee that throws"));

function tailCalls(f, x) { "use strict"; return f(x); }
function tailCallsMethod(o, x) { "use strict"; return o.method(x); }
function tailCallsWithManyArguments(f) { "use strict"; return f(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12); }
for (const f of [tailCalls, tailCallsMethod, tailCallsWithManyArguments])
    noInline(f);
if (usesDataStubs) {
    applies(tailCalls, "cached-call");
    applies(tailCallsMethod, "cached-call");
}
doesNotApply(tailCallsWithManyArguments, "cached-call");
repeat(i => check(tailCalls(addOne, i), i + 1, "a tail call"));
repeat(i => check(tailCalls(i & 1 ? addOne : addTwo, i), i + (i & 1 ? 1 : 2), "a tail call of two callees"));
{
    const callees = [addOne, addTwo, twoParameters, noParameters, restParameters, usesArguments, withDefault, Math.abs, addOne.bind(null), String];
    const expected = [8, 9, "7:undefined", "none", "1:7", "1:7", 12, 7, 8, "7"];
    repeat(i => check(tailCalls(callees[i % callees.length], 7), expected[i % callees.length], "a tail call of callee " + i % callees.length + " of many"));
    for (const bad of [undefined, null, 5, "text", { }])
        repeat(() => throwsTypeError(() => tailCalls(bad, 1), "a tail call of " + String(bad)));
    repeat(() => throwsTypeError(() => tailCalls(Class, 1), "a tail call of a class"));
    repeat(i => check(tailCalls(addOne, i), i + 1, "a tail call after something that cannot be called"));
    const first = { method: returnsThis }, second = { method: returnsThis };
    repeat(i => check(tailCallsMethod(i & 1 ? first : second), i & 1 ? first : second, "the receiver of a tail call"));
}

function countsDown(f, n) { "use strict"; return n ? f(f, n - 1) : "done"; }
function pingsThenPongs(ping, pong, n) { "use strict"; return n ? pong(pong, ping, n - 1) : "done"; }
function pongsThenPings(pong, ping, n) { "use strict"; return n ? ping(ping, pong, n - 1) : "done"; }
for (const f of [countsDown, pingsThenPongs, pongsThenPings])
    noInline(f);
check(countsDown(countsDown, 1000000), "done", "a million tail calls");
check(pingsThenPongs(pingsThenPongs, pongsThenPings, 1000001), "done", "a million tail calls between two functions");

function tailCallsSurvivor(f) { "use strict"; return f(); }
noInline(tailCallsSurvivor);
for (let round = 0; round < 6; round++) {
    (function () {
        const value = "round " + round;
        const closure = () => value;
        repeat(() => check(tailCallsSurvivor(closure), value, "a tail call of a closure that is about to die"));
    })();
    round & 1 ? fullGC() : edenGC();
    const fillers = [];
    for (let i = 0; i < 2000; i++)
        fillers.push(() => "filler " + i);
    repeat(i => check(tailCallsSurvivor(fillers[i]), "filler " + i, "tail calls of closures made after a collection"));
}

function tailCallsThrowing(f, x) { "use strict"; return f(x); }
function catchesTailCall(f, x) { try { return tailCallsThrowing(f, x); } catch (error) { return error.message; } }
noInline(tailCallsThrowing);
noInline(catchesTailCall);
repeat(i => check(catchesTailCall(throwing, i), "thrown " + i, "a tail call of a callee that throws"));

class Store {
    constructor() { this.log = []; }
    get(key) { return "got " + key; }
    has(key) { return "has " + key; }
    set(key, value) { this.log.push(key + "=" + value); return "set"; }
    add(value) { this.log.push("+" + value); return "added"; }
    push(value) { this.log.push("<" + value); return "pushed"; }
    pop() { return "popped"; }
    charAt(index) { return "char " + index; }
    charCodeAt(index) { return "code " + index; }
    codePointAt(index) { return "point " + index; }
    isArray(value) { return "array? " + value; }
}
function callsGet(o, k) { return o.get(k); }
function callsHas(o, k) { return o.has(k); }
function callsSet(o, k, v) { return o.set(k, v); }
function callsSetIgnoringResult(o, k, v) { o.set(k, v); }
function callsAdd(o, v) { return o.add(v); }
function callsAddIgnoringResult(o, v) { o.add(v); }
function callsPush(o, v) { return o.push(v); }
function callsPop(o) { return o.pop(); }
function callsCharAt(o, i) { return o.charAt(i); }
function callsCharCodeAt(o, i) { return o.charCodeAt(i); }
function callsCodePointAt(o, i) { return o.codePointAt(i); }
function callsIsArray(o, v) { return o.isArray(v); }
function tailCallsGet(o, k) { "use strict"; return o.get(k); }
for (const f of [callsGet, callsHas, callsSet, callsSetIgnoringResult, callsAdd, callsAddIgnoringResult, callsPush, callsPop, callsCharAt, callsCharCodeAt, callsCodePointAt, callsIsArray, tailCallsGet])
    noInline(f);
{
    const store = new Store;
    const map = new Map([["k", "v"]]);
    const set = new Set(["k"]);
    const weakKey = { };
    const weakMap = new WeakMap([[weakKey, "weak"]]);
    class DerivedMap extends Map { get(key) { return "derived " + super.get(key); } }
    const derived = new DerivedMap([["k", "v"]]);
    const overridden = new Map([["k", "v"]]);
    overridden.get = function (key) { return "own " + key; };
    repeat(() => {
        check(callsGet(store, "k"), "got k", "get of a class");
        check(callsGet(map, "k"), "v", "get of a Map");
        check(callsGet(map, "absent"), undefined, "get of a Map, absent");
        check(callsGet(weakMap, weakKey), "weak", "get of a WeakMap");
        check(callsGet(derived, "k"), "derived v", "get of a class that extends Map");
        check(callsGet(overridden, "k"), "own k", "get of a Map with its own get");
        check(tailCallsGet(store, "k"), "got k", "a tail call of get of a class");
        check(tailCallsGet(map, "k"), "v", "a tail call of get of a Map");
        check(tailCallsGet(overridden, "k"), "own k", "a tail call of get of a Map with its own get");
        check(callsHas(store, "k"), "has k", "has of a class");
        check(callsHas(map, "k"), true, "has of a Map");
        check(callsHas(set, "k"), true, "has of a Set");
        check(callsHas(set, "absent"), false, "has of a Set, absent");
        check(callsSet(store, "a", 1), "set", "set of a class");
        check(callsSet(map, "a", 1), map, "set of a Map");
        check(callsAdd(store, 1), "added", "add of a class");
        check(callsAdd(set, 1), set, "add of a Set");
        check(callsPush(store, 1), "pushed", "push of a class");
        check(callsPop(store), "popped", "pop of a class");
        check(callsCharAt(store, 1), "char 1", "charAt of a class");
        check(callsCharAt("abc", 1), "b", "charAt of a string");
        check(callsCharCodeAt(store, 1), "code 1", "charCodeAt of a class");
        check(callsCharCodeAt("abc", 1), 98, "charCodeAt of a string");
        check(callsCodePointAt(store, 1), "point 1", "codePointAt of a class");
        check(callsCodePointAt("abc", 1), 98, "codePointAt of a string");
        check(callsIsArray(store, 1), "array? 1", "isArray of a class");
        check(callsIsArray(Array, []), true, "Array.isArray");
        check(callsIsArray(Array, store), false, "Array.isArray of something else");
    });
    const array = [];
    repeat(i => check(callsPush(array, i), i + 1, "push of an array"));
    repeat(i => check(callsPop(array), 199 - i, "pop of an array"));
    check(callsPop(array), undefined, "pop of an empty array");
    store.log.length = 0;
    repeat(i => {
        callsSetIgnoringResult(i & 1 ? store : map, "k" + i, i);
        callsAddIgnoringResult(i & 1 ? store : set, i);
    });
    check(store.log.length, 200, "calls whose result is ignored, of a class");
    check(map.get("k198"), 198, "calls whose result is ignored, of a Map");
    check(set.has(198), true, "calls whose result is ignored, of a Set");
    for (const bad of [undefined, null])
        repeat(() => throwsTypeError(() => callsGet(bad, "k"), "get of " + bad));
    repeat(() => throwsTypeError(() => callsGet({ }, "k"), "get of an object without one"));
    repeat(() => throwsTypeError(() => callsGet({ get: 5 }, "k"), "get that is a number"));
    repeat(() => throwsTypeError(() => callsGet(5, "k"), "get of a number"));
    repeat(() => check(callsGet(store, "k"), "got k", "get of a class, after what cannot be called"));
    const stores = [];
    for (let i = 0; i < 12; i++)
        stores.push({ get: key => key + i });
    repeat(i => check(callsGet(stores[i % 12], "k"), "k" + i % 12, "get of twelve objects, each with its own function"));
}
