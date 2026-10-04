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
function thrownBy(f, ...args) {
    try {
        f(...args);
    } catch (error) {
        return error.constructor.name;
    }
    return "nothing";
}
function namesIn(stack) { return stack.split("\n").map(line => line.split("@")[0]).join(); }
const directCall = "direct-host-call", directConstruct = "direct-host-construct", cachedCall = "cached-call";
const other = createGlobalObject();

function hypotenuse(a, b) { let result = Math.hypot(a, b); return result; }
function hypotenuseOfNothing() { let result = Math.hypot(); return result; }
function hypotenuseOfFour(a, b, c, d) { let result = Math.hypot(a, b, c, d); return result; }
function hypotenuseOfTen(a) { let result = Math.hypot(a, 0, 0, 0, 0, 0, 0, 0, 0, 0); return result; }
function freezes(o) { Object.freeze(o); let result = Object.isFrozen(o); return result; }
function writes(value) { let result = JSON.stringify(value); return result; }
function writesNothing() { let result = JSON.stringify(); return result; }
function writesInTailPosition(value) { "use strict"; return JSON.stringify(value); }
function registered(name) { let result = Symbol.for(name); return result; }
function parsesNumber(text) { let result = parseFloat(text); return result; }
function escapes(text) { let result = encodeURIComponent(text); return result; }
function appliesFunction(f, thisValue, list) { let result = Reflect.apply(f, thisValue, list); return result; }
function characterAt(n, index) { let text = "text" + n; let result = text.at(index); return result; }
function searchesForNumber(n, needle) { let text = "text" + n; let result = text.indexOf(needle | 0); return result; }
for (let f of [hypotenuse, hypotenuseOfNothing, hypotenuseOfFour, hypotenuseOfTen, freezes, writes, writesNothing, writesInTailPosition, registered, parsesNumber, escapes, appliesFunction, characterAt, searchesForNumber])
    noInline(f);

function callsError(message) { let result = Error(message); return result; }
function callsErrorInTailPosition(message) { "use strict"; return Error(message); }
function callsSymbol(name) { let result = Symbol(name); return result; }
function callsBigInt(value) { let result = BigInt(value); return result; }
function callsObject(value) { let result = Object(value); return result; }
function callsMap() { let result = Map(); return result; }
function constructsError(message) { let result = new Error(message); return result; }
function constructsMap() { let result = new Map(); return result; }
function constructsMapOf(entries) { let result = new Map(entries); return result; }
function constructsMapWithTooMany(entries) { let result = new Map(entries, 1, 2, 3); return result; }
function constructsSetOf(values) { let result = new Set(values); return result; }
function constructsBytes(length) { let result = new Uint8Array(length); return result; }
function constructsProxy(target, handler) { let result = new Proxy(target, handler); return result; }
function constructsPromise(executor) { let result = new Promise(executor); return result; }
function constructsSymbol() { let result = new Symbol(); return result; }
function constructsHostFunction() { let result = new Math.hypot(); return result; }
function constructsDateOfNine() { let result = new Date(2000, 0, 1, 0, 0, 0, 0, 0, 0); return result; }
for (let f of [callsError, callsErrorInTailPosition, callsSymbol, callsBigInt, callsObject, callsMap, constructsError, constructsMap, constructsMapOf, constructsMapWithTooMany, constructsSetOf, constructsBytes, constructsProxy, constructsPromise, constructsSymbol, constructsHostFunction, constructsDateOfNine])
    noInline(f);

function shadowed(Math) { let result = Math.hypot(3, 4); return result; }
function shadowedConstructor(Map) { let result = new Map(); return result; }
function ofUnknownReceiver(text, index) { let result = text.at(index); return result; }
function callsWithNone(f) { let result = f(); return result; }
function callsWithOne(f, a) { let result = f(a); return result; }
function callsWithTwo(f, a, b) { let result = f(a, b); return result; }
function callsWithFour(f, a, b, c, d) { let result = f(a, b, c, d); return result; }
function callsMethod(o, a) { let result = o.method(a); return result; }
function callsInTailPosition(f, a) { "use strict"; return f(a); }
function callsAndCatches(f, a) { try { let result = f(a); return result; } catch (error) { return "caught " + error.constructor.name; } }
function parsesAndCatches(text) { try { let result = JSON.parse(text); return result; } catch (error) { return "caught " + error.constructor.name; } }
for (let f of [shadowed, shadowedConstructor, ofUnknownReceiver, callsWithNone, callsWithOne, callsWithTwo, callsWithFour, callsMethod, callsInTailPosition, callsAndCatches, parsesAndCatches])
    noInline(f);

function doubles(x) { return x * 2; }
function throws() { throw new RangeError("thrown by a callback"); }
function returnsThis() { "use strict"; return this; }
noInline(doubles);
noInline(throws);
noInline(returnsThis);

for (let round = 0; round < 4; ++round) {
    check(hypotenuse(3, 4), 5, "Math.hypot(3, 4)");
    check(hypotenuse("6", { valueOf() { return 8; } }), 10, "Math.hypot of values that are converted");
    check(hypotenuseOfNothing(), 0, "Math.hypot()");
    check(hypotenuseOfFour(1, 1, 1, 1), 2, "Math.hypot(1, 1, 1, 1)");
    check(hypotenuseOfTen(-7), 7, "Math.hypot of ten numbers");
    check(freezes({ a: 1 }), true, "Object.freeze(object)");
    check(freezes(1), true, "Object.freeze(1)");
    check(writes({ a: [1, "b", null] }), '{"a":[1,"b",null]}', "JSON.stringify(object)");
    check(writes({ toJSON() { return doubles(21); } }), "42", "JSON.stringify calling toJSON");
    check(writes(undefined), undefined, "JSON.stringify(undefined)");
    check(writesNothing(), undefined, "JSON.stringify()");
    check(writesInTailPosition([round]), "[" + round + "]", "JSON.stringify in tail position");
    check(registered("name"), Symbol.for("name"), "Symbol.for(name)");
    check(parsesNumber("1.5px"), 1.5, "parseFloat(text)");
    check(escapes("a b&c"), "a%20b%26c", "encodeURIComponent(text)");
    check(appliesFunction(doubles, undefined, [4]), 8, "Reflect.apply(doubles)");
    check(appliesFunction(returnsThis, 7, []), 7, "Reflect.apply with a this value");
    check(appliesFunction(Math.hypot, null, [5, 12]), 13, "Reflect.apply(Math.hypot)");
    check(characterAt(round, -1), String(round), "at(-1)");
    check(characterAt(round, 9), undefined, "at(9)");
    check(searchesForNumber(round, round), 4, "indexOf(number)");
    check(searchesForNumber(round, 9), -1, "indexOf(9)");

    check(thrownBy(registered, Symbol()), "TypeError", "Symbol.for(symbol)");
    check(thrownBy(escapes, "\ud800"), "URIError", "encodeURIComponent of a lone surrogate");
    check(thrownBy(writes, 1n), "TypeError", "JSON.stringify(1n)");
    check(thrownBy(writes, { toJSON: throws }), "RangeError", "JSON.stringify calling toJSON that throws");
    check(thrownBy(writesInTailPosition, { toJSON: throws }), "RangeError", "JSON.stringify in tail position calling toJSON that throws");
    check(thrownBy(appliesFunction, throws, undefined, []), "RangeError", "Reflect.apply(throws)");
    check(thrownBy(appliesFunction, 1, undefined, []), "TypeError", "Reflect.apply(1)");
    check(thrownBy(hypotenuse, 1, { valueOf: throws }), "RangeError", "Math.hypot calling valueOf that throws");
    check(parsesAndCatches("[1"), "caught SyntaxError", "JSON.parse of bad text");
    check(parsesAndCatches("[1]")[0], 1, "JSON.parse of good text");

    check(callsError("text").message, "text", "Error(text)");
    check(callsError("text") instanceof Error, true, "Error(text)");
    check(callsErrorInTailPosition("text").message, "text", "Error(text) in tail position");
    check(callsSymbol("name").description, "name", "Symbol(name)");
    check(callsBigInt(7), 7n, "BigInt(7)");
    check(thrownBy(callsBigInt, 1.5), "RangeError", "BigInt(1.5)");
    check(typeof callsObject(1), "object", "Object(1)");
    check(thrownBy(callsMap), "TypeError", "Map()");
    check(constructsError("text").message, "text", "new Error(text)");
    check(constructsMap().size, 0, "new Map()");
    check(constructsMapOf([[1, 2]]).get(1), 2, "new Map(entries)");
    check(constructsMapOf({ *[Symbol.iterator]() { yield [doubles(1), 3]; } }).get(2), 3, "new Map of an iterable");
    check(constructsMapWithTooMany([[1, 2]]).get(1), 2, "new Map(entries, 1, 2, 3)");
    check(thrownBy(constructsMapOf, 5), "TypeError", "new Map(5)");
    check(thrownBy(constructsMapOf, { [Symbol.iterator]: throws }), "RangeError", "new Map of an iterable that throws");
    check(constructsSetOf([1, 1, 2]).size, 2, "new Set(values)");
    check(constructsBytes(4).length, 4, "new Uint8Array(4)");
    check(thrownBy(constructsBytes, -1), "RangeError", "new Uint8Array(-1)");
    check(constructsProxy({ a: 1 }, {}).a, 1, "new Proxy(target, handler)");
    check(thrownBy(constructsProxy, 1, {}), "TypeError", "new Proxy(1, handler)");
    check(constructsPromise(resolve => resolve(doubles(1))) instanceof Promise, true, "new Promise(executor)");
    check(thrownBy(constructsPromise, 1), "TypeError", "new Promise(1)");
    check(thrownBy(constructsSymbol), "TypeError", "new Symbol()");
    check(thrownBy(constructsHostFunction), "TypeError", "new Math.hypot()");
    check(constructsDateOfNine().getFullYear(), 2000, "new Date of nine numbers");

    check(shadowed({ hypot() { return "mine"; } }), "mine", "a parameter called Math");
    check(shadowed(Math), 5, "Math as a parameter");
    check(shadowedConstructor(Set) instanceof Set, true, "a parameter called Map");
    check(ofUnknownReceiver("text", 1), "e", "at(1) of a string");
    check(ofUnknownReceiver([5, 6], 1), 6, "at(1) of an array");
    check(ofUnknownReceiver({ at() { return "mine"; } }, 1), "mine", "at(1) of an object");
}
applies(hypotenuse, directCall);
applies(hypotenuseOfNothing, directCall);
applies(hypotenuseOfFour, directCall);
doesNotApply(hypotenuseOfTen, directCall);
for (let f of [freezes, writes, writesNothing, writesInTailPosition, registered, parsesNumber, escapes, characterAt, searchesForNumber, parsesAndCatches])
    applies(f, directCall);
for (let f of [hypotenuse, freezes, writes, writesInTailPosition])
    doesNotApply(f, cachedCall, directConstruct);
doesNotApply(appliesFunction, directCall);
for (let f of [callsError, callsErrorInTailPosition, callsSymbol, callsBigInt, callsObject, callsMap])
    applies(f, directCall);
for (let f of [constructsError, constructsMapOf, constructsMapWithTooMany, constructsSetOf, constructsBytes, constructsProxy, constructsSymbol])
    applies(f, directConstruct);
doesNotApply(constructsPromise, directCall, directConstruct);
doesNotApply(constructsHostFunction, directCall, directConstruct);
doesNotApply(constructsDateOfNine, directConstruct);
for (let f of [shadowed, shadowedConstructor, ofUnknownReceiver, callsWithNone, callsWithOne, callsWithTwo, callsWithFour, callsMethod, callsInTailPosition, callsAndCatches])
    doesNotApply(f, directCall, directConstruct);
const usesDataStubs = (remarksOf(callsMethod) || []).includes("calls:GetById");
for (let f of usesDataStubs ? [callsWithNone, callsWithOne, callsWithTwo, callsWithFour, callsMethod, callsInTailPosition, callsAndCatches] : [])
    applies(f, cachedCall);

for (let round = 0; round < 8; ++round) {
    check(callsWithNone(Math.hypot), 0, "a cached Math.hypot()");
    check(callsWithTwo(Math.hypot, 3, 4), 5, "a cached Math.hypot(3, 4)");
    check(callsWithFour(Math.hypot, 1, 1, 1, 1), 2, "a cached Math.hypot(1, 1, 1, 1)");
    check(callsInTailPosition(parseFloat, "2.5"), 2.5, "a cached parseFloat in tail position");
    check(callsAndCatches(JSON.parse, "{"), "caught SyntaxError", "a cached JSON.parse that throws");
    check(callsMethod({ method: encodeURIComponent }, " "), "%20", "a cached method");
}
for (let round = 0; round < 8; ++round)
    check(callsWithOne(String, round), String(round), "a cached String(number)");
for (let round = 0; round < 3; ++round) {
    for (let [f, argument, expected] of [[parseFloat, "1.5", 1.5], [parseFloat, "2.5", 2.5], [doubles, 2, 4], [doubles, 3, 6], [Number, "7", 7], [Number, "8", 8], [x => x + 1, 1, 2], [Symbol.keyFor, Symbol.for("key"), "key"]])
        check(callsWithOne(f, argument), expected, "a call site with several callees");
}

function callsHostOnly(f, a) { let result = f(a); return result; }
function callsOtherRealm(f, a) { let result = f(a); return result; }
function callsBound(f, a) { let result = f(a); return result; }
function callsProxy(f, a) { let result = f(a); return result; }
function callsWithThis(o, a) { let result = o.method(a); return result; }
function callsNotCallable(f, a) { let result = f(a); return result; }
function callsResolver(f, a) { let result = f(a); return result; }
for (let f of [callsHostOnly, callsOtherRealm, callsBound, callsProxy, callsWithThis, callsNotCallable, callsResolver])
    noInline(f);
const hasOwn = Object.prototype.hasOwnProperty;
for (let round = 0; round < 10; ++round) {
    check(callsHostOnly(parseFloat, round + ".5"), round + 0.5, "one host function");
    check(callsOtherRealm(other.Array, 3) instanceof Array, false, "Array of another realm");
    check(callsOtherRealm(other.Array, 3) instanceof other.Array, true, "Array of another realm");
    check(callsOtherRealm(other.Object.keys, { a: 1 }) instanceof other.Array, true, "Object.keys of another realm");
    check(thrownBy(callsOtherRealm, other.Symbol.for, Symbol()), "TypeError", "Symbol.for of another realm");
    try {
        callsOtherRealm(other.JSON.parse, "{");
    } catch (error) {
        check(error instanceof other.SyntaxError, true, "the realm of an error thrown by JSON.parse of another realm");
    }
    check(callsBound(Math.hypot.bind(null, 3), 4), 5, "a bound Math.hypot");
    check(callsBound(String.bind(null), round), String(round), "a bound String");
    check(callsBound(doubles.bind(null), round), round * 2, "a bound function");
    check(callsProxy(new Proxy(parseFloat, {}), "1.5"), 1.5, "a proxy of parseFloat");
    check(callsProxy(new Proxy(parseFloat, { apply() { return "trapped"; } }), "1.5"), "trapped", "a proxy of parseFloat with a trap");
    check(callsWithThis({ method: hasOwn, a: 1 }, "a"), true, "hasOwnProperty of one object");
    check(callsWithThis({ method: hasOwn, b: 1 }, "a"), false, "hasOwnProperty of another object");
    check(callsWithThis({ method: returnsThis }, 0).method, returnsThis, "a method that returns this");
    for (let notCallable of [undefined, null, 1, "text", {}, Symbol(), Math])
        check(thrownBy(callsNotCallable, notCallable, 1), "TypeError", "calling " + typeof notCallable);
    check(callsNotCallable(parseFloat, "3"), 3, "a host function after values that cannot be called");
}
for (let round = 0; round < 6; ++round) {
    let resolver;
    let promise = new Promise(resolve => { resolver = resolve; });
    for (let i = 0; i < 3; ++i)
        check(callsResolver(resolver, round), undefined, "a function that resolves a promise");
    resolver = null;
    promise = null;
    gc();
    check(callsResolver(parseFloat, "4"), 4, "a host function after a collection");
}

function throwsFromCallback() { throw new Error("from a callback"); }
function reachesCallback() { let result = JSON.stringify({ toJSON: throwsFromCallback }); return result; }
function reachesCallbackFromConstructor() { let result = new Map({ [Symbol.iterator]: throwsFromCallback }); return result; }
function reachesCallbackThroughCache(f) { let result = f({ toJSON: throwsFromCallback }); return result; }
function makesErrors(E) { return [new Error("direct"), new E("generic"), Error("direct"), E("generic")]; }
for (let f of [throwsFromCallback, reachesCallback, reachesCallbackFromConstructor, reachesCallbackThroughCache, makesErrors])
    noInline(f);
function stackOfErrorThrownBy(f, ...args) {
    try {
        f(...args);
    } catch (error) {
        return namesIn(error.stack);
    }
    return "nothing was thrown";
}
for (let round = 0; round < 4; ++round) {
    check(stackOfErrorThrownBy(reachesCallback), "throwsFromCallback,stringify,reachesCallback,stackOfErrorThrownBy,global code", "the stack of an error thrown below JSON.stringify");
    check(stackOfErrorThrownBy(reachesCallbackFromConstructor), "throwsFromCallback,Map,reachesCallbackFromConstructor,stackOfErrorThrownBy,global code", "the stack of an error thrown below new Map");
    check(stackOfErrorThrownBy(reachesCallbackThroughCache, JSON.stringify), "throwsFromCallback,stringify,reachesCallbackThroughCache,stackOfErrorThrownBy,global code", "the stack of an error thrown below a cached JSON.stringify");
    let errors = makesErrors(Error);
    for (let error of errors) {
        check(namesIn(error.stack), "makesErrors,global code", "the stack of an error made by Error");
        check(error.line, errors[1].line, "the line of an error made by Error");
    }
    check(errors[0].column < errors[1].column && errors[1].column < errors[2].column && errors[2].column < errors[3].column, true, "the columns of errors made by Error");
}
applies(reachesCallback, directCall);
applies(reachesCallbackFromConstructor, directConstruct);
if (usesDataStubs)
    applies(reachesCallbackThroughCache, cachedCall);
applies(makesErrors, directCall, directConstruct);
