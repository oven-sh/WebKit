//@ runDefault
//@ runDefault("--useJIT=0")
//@ runDefault("--useJIT=0", "--useLLIntPrototypeCacheRearming=0")
//@ runDefault("--useJIT=0", "--useLLIntUnsetCaching=0")
//@ runDefault("--useJIT=0", "--collectContinuously=1")

// The LLInt caches a get_by_id that finds its value on the prototype chain (GetByIdMode::ProtoLoad) after
// a countdown. The countdown starts again when the cache is cleared by a watchpoint, when an own property
// replaces it, and when the cache is made, so that a receiver of another structure can replace it. The
// cache made after that must be as correct as the first one.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + String(expected) + " but got " + String(actual));
}

const warmUp = 20;
const options = jscOptions();
// The cache of a site can be told only while the function runs in the LLInt.
const interpreterOnly = !options.useJIT && options.useLLIntICs && options.prototypeHitCountForLLIntCaching > 0;
const rearms = options.useLLIntPrototypeCacheRearming;

// Functions of the same source share their code, and so their caches. Each getter has its own source.
let getters = 0;
function makeGetter() {
    return new Function("o", "return o.value; // " + getters++);
}

// The prototype of each class has a structure of its own. A prototype that shares its structure with
// another prototype cannot be watched after the other one changed.
let classes = 0;
function makeClass() {
    class Unique { constructor() { this.a = 1; } }
    Unique.prototype["unique" + classes++] = true;
    return Unique;
}

// The site has a cache of this kind, and the fast path serves the receiver from it.
function expectCache(get, receiver, kind, message) {
    if (!interpreterOnly)
        return;
    shouldBe($vm.llintGetByIdCaches(get)[0], kind, message);
    shouldBe($vm.llintGetByIdCacheHits(get, receiver)[0], true, message);
}

// The same, for a cache that needs the countdown to start again. Without that the site has what it had.
function expectLaterCache(get, receiver, kind, kindWithNoRearming, message) {
    if (!interpreterOnly)
        return;
    if (rearms) {
        expectCache(get, receiver, kind, message);
        return;
    }
    shouldBe($vm.llintGetByIdCaches(get)[0], kindWithNoRearming, message);
    shouldBe($vm.llintGetByIdCacheHits(get, receiver)[0], false, message);
}

// The receiver gets a new structure after the cache is made: "re-armed after a transition".
{
    const C = makeClass();
    C.prototype.value = "proto";
    const get = makeGetter();
    const o = new C;
    const before = new C;
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(o), "proto");
    expectCache(get, o, "proto", "first cache");
    o.b = 2; // Transition.
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(o), "proto", "after the transition");
    expectLaterCache(get, o, "proto", "proto", "cache after the transition");
    o.c = 3; // And again.
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(o), "proto", "after the second transition");
    expectLaterCache(get, o, "proto", "proto", "cache after the second transition");
    shouldBe(get(before), "proto");

    // The value is replaced on the prototype.
    C.prototype.value = "replaced";
    shouldBe(get(o), "replaced");
    shouldBe(get(before), "replaced");
    // The property is deleted from the prototype.
    delete C.prototype.value;
    shouldBe(get(o), undefined, "deleted from the prototype");
    shouldBe(get(before), undefined);
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(o), undefined);
    // And comes back.
    C.prototype.value = "back";
    shouldBe(get(o), "back");
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(o), "back");
    expectLaterCache(get, o, "proto", "empty", "cache after the property came back");
    // The receiver gets the property.
    o.value = "own";
    shouldBe(get(o), "own");
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(o), "own");
    expectCache(get, o, "self");
    delete o.value;
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(o), "back", "after the own property is deleted");
    shouldBe(get(before), "back");
}

// The site sees an own property first. Without rearming that stops the prototype cache for the life of the code.
{
    const C = makeClass();
    C.prototype.value = "proto";
    const get = makeGetter();
    const own = { value: "own" };
    shouldBe(get(own), "own");
    expectCache(get, own, "self");
    const o = new C;
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(o), "proto");
    expectLaterCache(get, o, "proto", "self", "cache after an own property");
    // Shadow the value in the middle of the chain.
    const middle = Object.create(C.prototype);
    const p = Object.create(middle);
    p.a = 1;
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(p), "proto");
    expectLaterCache(get, p, "proto", "self", "cache for a longer chain");
    middle.value = "middle";
    shouldBe(get(p), "middle", "shadowed in the middle");
    shouldBe(get(o), "proto");
    shouldBe(get(own), "own");
    delete middle.value;
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(p), "proto");
}

// A watchpoint clears the cache, and the same structure is cached again.
{
    const top = { value: "top" };
    const middle = Object.create(top);
    middle.m = 1;
    const o = Object.create(middle);
    o.a = 1;
    const get = makeGetter();
    for (let round = 0; round < 6; ++round) {
        for (let i = 0; i < warmUp; ++i)
            shouldBe(get(o), "top", "round " + round);
        if (!round)
            expectCache(get, o, "proto");
        else
            expectLaterCache(get, o, "proto", "empty", "cache in round " + round);
        // The prototype of an object of the chain is replaced and put back: its structure changes and the watchpoint fires.
        Object.setPrototypeOf(middle, { value: "other top" });
        shouldBe(get(o), "other top", "after the watchpoint fired, round " + round);
        Object.setPrototypeOf(middle, top);
        shouldBe(get(o), "top");
    }
    middle.value = "middle";
    shouldBe(get(o), "middle");
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(o), "middle");
    delete middle.value;
    shouldBe(get(o), "top");
    Object.setPrototypeOf(middle, { value: "new top" });
    shouldBe(get(o), "new top", "prototype of the prototype replaced");
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(o), "new top");
    top.value = "old top";
    shouldBe(get(o), "new top");
}

// Array methods on arrays of each indexing type.
{
    const get = new Function("a", "return a.indexOf;");
    const arrays = [[], [1, 2], [1.5, 2.5], ["a", { }], [, 1], new Array(100000)];
    arrays[5][99999] = 1;
    for (let round = 0; round < 5; ++round) {
        for (const array of arrays) {
            for (let i = 0; i < warmUp; ++i)
                shouldBe(get(array), Array.prototype.indexOf);
            // Without rearming the site has the structure of the first array for the life of the code.
            if (array === arrays[0])
                expectCache(get, array, "proto");
            else
                expectLaterCache(get, array, "proto", "proto", "array " + arrays.indexOf(array) + " in round " + round);
        }
    }
    const original = Array.prototype.indexOf;
    const replacement = function () { };
    Array.prototype.indexOf = replacement;
    for (const array of arrays)
        shouldBe(get(array), replacement, "Array.prototype.indexOf replaced");
    Array.prototype.indexOf = original;
    for (const array of arrays) {
        array.indexOf = replacement;
        shouldBe(get(array), replacement, "own indexOf");
        delete array.indexOf;
        shouldBe(get(array), original);
    }
}

// Two structures that alternate at one site.
{
    const proto = { value: "shared" };
    const a = Object.create(proto);
    a.a = 1;
    const b = Object.create(proto);
    b.b = 1;
    const get = makeGetter();
    for (let i = 0; i < warmUp * 4; ++i)
        shouldBe(get(i & 1 ? a : b), "shared");
    proto.value = "changed";
    shouldBe(get(a), "changed");
    shouldBe(get(b), "changed");
    delete proto.value;
    shouldBe(get(a), undefined);
    shouldBe(get(b), undefined);
    for (let i = 0; i < warmUp * 4; ++i)
        shouldBe(get(i & 1 ? a : b), undefined);
    Object.prototype.value = "object";
    shouldBe(get(a), "object");
    shouldBe(get(b), "object");
    delete Object.prototype.value;
}

// A present value, an absent value and an own value at one site, in turns. After 255 misses the site keeps what it has.
{
    const WithProto = makeClass();
    WithProto.prototype.value = "proto";
    const Without = makeClass();
    const get = makeGetter();
    const receivers = [[new WithProto, "proto"], [new Without, undefined], [{ value: "own" }, "own"]];
    for (let round = 0; round < 200; ++round) {
        for (const [receiver, expected] of receivers) {
            for (let i = 0; i < 3; ++i)
                shouldBe(get(receiver), expected, "round " + round);
        }
    }
    Without.prototype.value = "now present";
    shouldBe(get(receivers[1][0]), "now present");
    delete WithProto.prototype.value;
    shouldBe(get(receivers[0][0]), undefined);
    shouldBe(get(receivers[2][0]), "own");
    delete Without.prototype.value;
    for (let round = 0; round < 10; ++round) {
        shouldBe(get(receivers[0][0]), undefined);
        shouldBe(get(receivers[1][0]), undefined);
        shouldBe(get(receivers[2][0]), "own");
    }
    Object.prototype.value = "object";
    shouldBe(get(receivers[0][0]), "object");
    shouldBe(get(receivers[1][0]), "object");
    shouldBe(get(receivers[2][0]), "own");
    delete Object.prototype.value;
}

// The objects of the cache die, and the site is used again.
{
    const get = makeGetter();
    for (let round = 0; round < 4; ++round) {
        (function () {
            const proto = { value: round };
            const o = Object.create(proto);
            o.a = round;
            for (let i = 0; i < warmUp; ++i)
                shouldBe(get(o), round);
        })();
        fullGC();
    }
    const proto = { value: "last" };
    const o = Object.create(proto);
    o.a = 1;
    for (let i = 0; i < warmUp; ++i)
        shouldBe(get(o), "last");
    proto.value = "changed";
    shouldBe(get(o), "changed");
    delete proto.value;
    shouldBe(get(o), undefined);
}

// instanceof reads Symbol.hasInstance and "prototype" of the constructor with the same cache.
{
    function check(value, constructor) { return value instanceof constructor; }
    class A { }
    class B extends A { }
    class C { }
    const b = new B;
    for (let round = 0; round < 5; ++round) {
        for (let i = 0; i < warmUp; ++i) {
            shouldBe(check(b, A), true);
            shouldBe(check(b, B), true);
            shouldBe(check(b, C), false);
        }
    }
    Object.defineProperty(C, Symbol.hasInstance, { value() { return true; }, configurable: true });
    shouldBe(check(b, C), true, "own Symbol.hasInstance");
    delete C[Symbol.hasInstance];
    shouldBe(check(b, C), false);
}

// for-of reads "next" of the iterator, and "done" and "value" of each result.
{
    function sum(iterable) {
        let total = 0;
        for (const value of iterable)
            total += value;
        return total;
    }
    class Range {
        constructor(n) { this.n = n; }
        [Symbol.iterator]() { return new RangeIterator(this.n); }
    }
    class Result {
        constructor(value, done) { this.value = value; this.done = done; }
    }
    class ResultWithProtoDone { constructor(value) { this.value = value; } }
    ResultWithProtoDone.prototype.done = false;
    class RangeIterator {
        constructor(n) { this.n = n; this.i = 0; }
        next() {
            if (this.i >= this.n)
                return new Result(undefined, true);
            return this.i & 1 ? new ResultWithProtoDone(this.i++) : new Result(this.i++, false);
        }
    }
    for (let round = 0; round < warmUp; ++round)
        shouldBe(sum(new Range(10)), 45);
    ResultWithProtoDone.prototype.done = true;
    shouldBe(sum(new Range(10)), 0, "done from the prototype");
    ResultWithProtoDone.prototype.done = false;
    shouldBe(sum(new Range(10)), 45);
    const originalNext = RangeIterator.prototype.next;
    RangeIterator.prototype.next = function () { return new Result(undefined, true); };
    shouldBe(sum(new Range(10)), 0, "next replaced");
    RangeIterator.prototype.next = originalNext;
    shouldBe(sum(new Range(10)), 45);
}
