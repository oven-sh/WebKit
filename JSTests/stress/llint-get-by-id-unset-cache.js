//@ runDefault
//@ runDefault("--useJIT=0")
//@ runDefault("--useJIT=0", "--useLLIntUnsetCaching=0")
//@ runDefault("--useJIT=0", "--useLLIntPrototypeCacheRearming=0")
//@ runDefault("--useJIT=0", "--collectContinuously=1")

// The LLInt caches a get_by_id that finds no property (GetByIdMode::Unset). Everything that makes the
// property appear must make the next access see it.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

const warmUp = 20;
const options = jscOptions();
// The cache of a site can be told only while the function runs in the LLInt.
const interpreterOnly = !options.useJIT && options.useLLIntICs;
const cachesUnset = interpreterOnly && options.useLLIntUnsetCaching && options.prototypeHitCountForLLIntCaching > 0;
const rearms = options.useLLIntPrototypeCacheRearming;

// Functions of the same source share their code, and so their caches. Each getter has its own source.
let getters = 0;
function makeGetter() {
    return new Function("o", "return o.missing; // " + getters++);
}

// The prototype of each class has a structure of its own. A prototype that shares its structure with
// another prototype cannot be watched after the other one changed.
let classes = 0;
function makeClass() {
    class Unique { constructor() { this.a = 1; } }
    Unique.prototype["unique" + classes++] = true;
    return Unique;
}

function warm(get, receiver) {
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(receiver), undefined, "warm up");
}

function expectCache(get, expected, message) {
    if (cachesUnset)
        shouldBe($vm.llintGetByIdCaches(get)[0], expected, message);
}

// The fast path does not serve this receiver from the cache of the site.
function expectNoCacheFor(get, receiver, message) {
    if (cachesUnset)
        shouldBe($vm.llintGetByIdCacheHits(get, receiver)[0], false, message);
}

// The second cache of a site needs the countdown to start again.
function expectSecondCache(get, expected, message) {
    expectCache(get, rearms ? expected : "empty", message);
}

// The property is added to the direct prototype.
{
    const C = makeClass();
    const get = makeGetter();
    const o = new C;
    warm(get, o);
    expectCache(get, "unset", "class instance");
    C.prototype.missing = 42;
    shouldBe(get(o), 42, "added on the prototype");
    delete C.prototype.missing;
    warm(get, o);
    expectSecondCache(get, "unset", "class instance, second cache");
    C.prototype.missing = 43;
    shouldBe(get(o), 43, "added on the prototype again");
    delete C.prototype.missing;
}

// The property is added to Object.prototype, the end of the chain.
{
    const C = makeClass();
    const get = makeGetter();
    const o = new C;
    warm(get, o);
    expectCache(get, "unset");
    Object.prototype.missing = "object";
    shouldBe(get(o), "object", "added on Object.prototype");
    delete Object.prototype.missing;
    warm(get, o);
}

// The property is added as a getter on the prototype.
{
    const C = makeClass();
    const get = makeGetter();
    const o = new C;
    warm(get, o);
    let calls = 0;
    Object.defineProperty(C.prototype, "missing", { get() { calls++; return this.a + 1; }, configurable: true });
    shouldBe(get(o), 2, "getter on the prototype");
    shouldBe(calls, 1);
    delete C.prototype.missing;
    warm(get, o);
}

// The property is added to the receiver. The structure changes, so the cache does not match.
{
    const get = makeGetter();
    const o = { a: 1 };
    warm(get, o);
    o.missing = 5;
    shouldBe(get(o), 5, "added on the receiver");
    delete o.missing;
    shouldBe(get(o), undefined, "deleted from the receiver");
    warm(get, o);
    o.missing = 6;
    shouldBe(get(o), 6, "added on the receiver again");
}

// Another object of the same structure gets the property.
{
    function Point(x) { this.x = x; }
    const get = makeGetter();
    const a = new Point(1);
    const b = new Point(2);
    warm(get, a);
    b.missing = 7;
    shouldBe(get(b), 7, "other object with the property");
    shouldBe(get(a), undefined, "first object still has no property");
}

// The prototype of the receiver is replaced.
{
    const get = makeGetter();
    const o = { a: 1 };
    warm(get, o);
    Object.setPrototypeOf(o, { missing: 8 });
    shouldBe(get(o), 8, "prototype of the receiver replaced");
}

// The prototype of the prototype is replaced.
{
    const C = makeClass();
    const get = makeGetter();
    const o = new C;
    warm(get, o);
    Object.setPrototypeOf(C.prototype, { missing: 9 });
    shouldBe(get(o), 9, "prototype of the prototype replaced");
    Object.setPrototypeOf(C.prototype, Object.prototype);
    shouldBe(get(o), undefined);
    warm(get, o);
    Object.setPrototypeOf(C.prototype, { missing: 10 });
    shouldBe(get(o), 10, "prototype of the prototype replaced again");
}

// A deep chain, and the property appears in the middle.
{
    const top = { t: 1 };
    const middle = Object.create(top);
    middle.m = 1;
    const bottom = Object.create(middle);
    bottom.b = 1;
    const o = Object.create(bottom);
    o.a = 1;
    const get = makeGetter();
    warm(get, o);
    middle.missing = 11;
    shouldBe(get(o), 11, "added in the middle of the chain");
    delete middle.missing;
    warm(get, o);
    top.missing = 12;
    shouldBe(get(o), 12, "added on top of the chain");
}

// A receiver with no prototype.
{
    const get = makeGetter();
    const o = Object.create(null);
    o.a = 1;
    warm(get, o);
    expectCache(get, "unset", "null prototype receiver");
    o.missing = 13;
    shouldBe(get(o), 13, "null prototype receiver");
}

// A dictionary receiver: a property can be added with no new structure. The cache makes the dictionary flat, once.
for (const makeDictionary of [$vm.toCacheableDictionary, $vm.toUncacheableDictionary]) {
    const get = makeGetter();
    const o = { a: 1, b: 2 };
    makeDictionary(o);
    warm(get, o);
    expectCache(get, "unset", "dictionary receiver, made flat for the cache");
    o.missing = 14;
    shouldBe(get(o), 14, "dictionary receiver");
    delete o.missing;
    shouldBe(get(o), undefined, "dictionary receiver after delete");
    makeDictionary(o);
    warm(get, o);
    // It was made flat before, so it stays a dictionary and has no cache.
    expectNoCacheFor(get, o, "dictionary receiver, second time");
    o.missing = 15;
    shouldBe(get(o), 15, "dictionary receiver again");
    delete o.missing;
    shouldBe(get(o), undefined);
    o.missing = 16;
    shouldBe(get(o), 16);
}

// A dictionary prototype.
for (const makeDictionary of [$vm.toCacheableDictionary, $vm.toUncacheableDictionary]) {
    const proto = { p: 1, q: 2 };
    makeDictionary(proto);
    const o = Object.create(proto);
    o.a = 1;
    const get = makeGetter();
    warm(get, o);
    expectCache(get, "unset", "dictionary prototype, made flat for the cache");
    proto.missing = 16;
    shouldBe(get(o), 16, "dictionary prototype");
    delete proto.missing;
    shouldBe(get(o), undefined);
    makeDictionary(proto);
    warm(get, o);
    expectNoCacheFor(get, o, "dictionary prototype, second time");
    proto.missing = 17;
    shouldBe(get(o), 17, "dictionary prototype again");
    delete proto.missing;
    shouldBe(get(o), undefined);
}

// A proxy on the chain decides each time.
{
    let answer;
    const proxy = new Proxy({ }, { get(target, key) { return key === "missing" ? answer : undefined; } });
    const o = Object.create(proxy);
    o.a = 1;
    const get = makeGetter();
    warm(get, o);
    expectCache(get, "empty", "proxy on the chain");
    answer = 18;
    shouldBe(get(o), 18, "proxy on the chain");
    answer = undefined;
    shouldBe(get(o), undefined);
}

// A proxy receiver.
{
    let answer;
    const proxy = new Proxy({ }, { get(target, key) { return key === "missing" ? answer : undefined; } });
    const get = makeGetter();
    warm(get, proxy);
    expectCache(get, "empty", "proxy receiver");
    answer = 19;
    shouldBe(get(proxy), 19, "proxy receiver");
}

// Primitive receivers use the prototype of the global object of the code.
{
    const get = makeGetter();
    warm(get, "string");
    expectCache(get, "unset", "string receiver");
    String.prototype.missing = 20;
    shouldBe(get("string"), 20, "string receiver");
    shouldBe(get("other string"), 20);
    delete String.prototype.missing;
    warm(get, "string");
    Object.prototype.missing = 21;
    shouldBe(get("string"), 21, "string receiver, Object.prototype");
    delete Object.prototype.missing;
}
{
    const get = makeGetter();
    const symbol = Symbol();
    warm(get, symbol);
    Symbol.prototype.missing = 22;
    shouldBe(get(symbol), 22, "symbol receiver");
    delete Symbol.prototype.missing;
}
{
    const get = makeGetter();
    warm(get, 1n << 70n);
    BigInt.prototype.missing = 23;
    shouldBe(get(1n << 70n), 23, "heap BigInt receiver");
    delete BigInt.prototype.missing;
}
{
    const get = makeGetter();
    warm(get, 5);
    expectCache(get, "empty", "number receiver");
    Number.prototype.missing = 24;
    shouldBe(get(5), 24, "number receiver");
    delete Number.prototype.missing;
}

// Functions make their own properties on first use.
{
    const get = new Function("f", "return f.prototype;");
    const arrow = () => { };
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(arrow), undefined, "arrow function has no prototype");
    function ordinary() { }
    shouldBe(typeof get(ordinary), "object", "function has a prototype");
    shouldBe(get(ordinary), ordinary.prototype);
    shouldBe(get(arrow), undefined);
}
{
    const get = makeGetter();
    function f() { }
    warm(get, f);
    Function.prototype.missing = 25;
    shouldBe(get(f), 25, "function receiver");
    delete Function.prototype.missing;
    warm(get, f);
    f.missing = 26;
    shouldBe(get(f), 26);
}

// Arrays and typed arrays.
{
    const get = makeGetter();
    const array = [1, 2, 3];
    warm(get, array);
    Array.prototype.missing = 27;
    shouldBe(get(array), 27, "array receiver");
    delete Array.prototype.missing;
    warm(get, array);
    array.missing = 28;
    shouldBe(get(array), 28);
}
{
    const get = makeGetter();
    const typed = new Uint8Array(4);
    warm(get, typed);
    Uint8Array.prototype.missing = 29;
    shouldBe(get(typed), 29, "typed array receiver");
    delete Uint8Array.prototype.missing;
}
{
    // A canonical numeric string that is not an index never reaches the prototype of a typed array.
    const get = new Function("o", "return o['1.5'];");
    const typed = new Uint8Array(4);
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(typed), undefined);
    Object.prototype["1.5"] = 30;
    shouldBe(get(typed), undefined, "typed array stops the lookup");
    shouldBe(get({ }), 30);
    delete Object.prototype["1.5"];
}

// Built-in objects with properties that are made on first use.
{
    const get = new Function("o", "return o.notAMathFunction;");
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(Math), undefined);
    Math.notAMathFunction = 31;
    shouldBe(get(Math), 31, "Math");
    delete Math.notAMathFunction;
    shouldBe(get(Math), undefined);
}
{
    const get = new Function("o", "return o.sin;");
    const o = Object.create(Math);
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(o), Math.sin, "lazy property of the prototype");
}
{
    // A lazy static property of the prototype is not absent.
    const get = new Function("o", "return o.cos;");
    const o = Object.create(Math);
    shouldBe(typeof get(o), "function");
}

// The global object.
{
    const get = new Function("return this.missingGlobal;");
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(), undefined);
    expectCache(get, "empty", "global object");
    globalThis.missingGlobal = 32;
    shouldBe(get(), 32, "global object");
    delete globalThis.missingGlobal;
    shouldBe(get(), undefined);
}
// A variable of a later script is a property of the global object, and makes no new structure.
// (With the JIT, the inline cache of the Baseline JIT keeps "absent" for it. That is not what this test is about.)
if (!options.useJIT) {
    const get = new Function("return this.missingVariable;");
    const getThroughChain = new Function("o", "return o.missingFunction;");
    const o = Object.create(globalThis);
    o.a = 1;
    for (let i = 0; i < warmUp; ++i) {
        shouldBe(get(), undefined);
        shouldBe(getThroughChain(o), undefined);
    }
    expectCache(get, "empty", "global object");
    expectCache(getThroughChain, "empty", "global object on the chain");
    loadString("var missingVariable = 'variable'; function missingFunction() { }");
    shouldBe(get(), "variable", "variable of a later script");
    shouldBe(typeof getThroughChain(o), "function", "function of a later script");
}

// Objects of another realm, and primitives read by code of another realm.
if (typeof createGlobalObject === "function") {
    const other = createGlobalObject();
    const get = makeGetter();
    const otherObject = other.eval("({ a: 1 })");
    warm(get, otherObject);
    other.Object.prototype.missing = 33;
    shouldBe(get(otherObject), 33, "object of another realm");
    shouldBe(get({ a: 1 }), undefined);
    delete other.Object.prototype.missing;

    const otherGet = other.Function("o", "return o.missing;");
    for (let i = 0; i < warmUp; ++i)
        shouldBe(otherGet("string"), undefined);
    String.prototype.missing = 34;
    shouldBe(otherGet("string"), undefined, "string read by code of another realm");
    other.String.prototype.missing = 35;
    shouldBe(otherGet("string"), 35);
    delete String.prototype.missing;
}

// One site that sees a present property, then an absent one, then the present one again.
{
    const get = makeGetter();
    const present = { missing: 1 };
    const absent = { other: 1 };
    for (let round = 0; round < 10; ++round) {
        for (let i = 0; i < 5; ++i)
            shouldBe(get(present), 1);
        for (let i = 0; i < 5; ++i)
            shouldBe(get(absent), undefined);
    }
    Object.prototype.missing = 36;
    shouldBe(get(absent), 36, "absent after alternation");
    shouldBe(get(present), 1);
    delete Object.prototype.missing;
}

// Many structures at one site.
{
    const get = makeGetter();
    const objects = [];
    for (let i = 0; i < 40; ++i) {
        const o = { };
        for (let j = 0; j <= i; ++j)
            o["f" + j] = j;
        objects.push(o);
    }
    for (let round = 0; round < 5; ++round) {
        for (const o of objects)
            shouldBe(get(o), undefined);
    }
    Object.prototype.missing = 37;
    for (const o of objects)
        shouldBe(get(o), 37, "many structures");
    delete Object.prototype.missing;
}

// The cache outlives the objects it was made for.
{
    const get = makeGetter();
    (function () {
        const Temporary = makeClass();
        warm(get, new Temporary);
    })();
    gc();
    const Later = makeClass();
    const later = new Later;
    Later.prototype.missing = 38;
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(later), 38, "after collection");
    fullGC();
    shouldBe(get(later), 38);
}

// An absent property of the result of an iterator, read by a for-of loop.
{
    let done = 0;
    const iterable = {
        [Symbol.iterator]() {
            let i = 0;
            return {
                next() {
                    // "value" is absent from the last result.
                    return i++ < 3 ? { value: i, done: false } : { done: true };
                }
            };
        }
    };
    for (let round = 0; round < warmUp; ++round) {
        let sum = 0;
        for (const value of iterable)
            sum += value;
        shouldBe(sum, 6);
    }
    Object.prototype.done = true;
    let count = 0;
    for (const value of { [Symbol.iterator]() { return { next() { count++; return { value: 1 }; } }; } })
        throw new Error("the loop body must not run");
    shouldBe(count, 1, "done comes from Object.prototype");
    delete Object.prototype.done;
}
