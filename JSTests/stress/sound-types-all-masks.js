//@ requireOptions("--useSoundTypes=1")

const Undefined = 1, Null = 2, Boolean_ = 4, Number_ = 8, String_ = 16, Symbol_ = 32, BigInt_ = 64, Function_ = 128, Array_ = 256, OtherObject = 512;

function referenceTag(value) {
    if (value === undefined)
        return Undefined;
    if (value === null)
        return Null;
    switch (typeof value) {
    case "boolean": return Boolean_;
    case "number": return Number_;
    case "string": return String_;
    case "symbol": return Symbol_;
    case "bigint": return BigInt_;
    case "function": return Function_;
    }
    if (Array.isArray(value) && !proxies.has(value))
        return Array_;
    return OtherObject;
}

function describeMask(mask) {
    let names = [];
    let take = (bits, name) => {
        if ((mask & bits) === bits) {
            mask &= ~bits;
            names.push(name);
        }
    };
    take(Boolean_, "boolean");
    take(Number_, "number");
    take(String_, "string");
    take(Symbol_, "symbol");
    take(BigInt_, "bigint");
    take(Function_ | Array_ | OtherObject, "object");
    take(Function_, "function");
    take(Array_, "array");
    take(OtherObject, "object");
    take(Null, "null");
    take(Undefined, "undefined");
    return names.join(" | ");
}

class DerivedArray extends Array { }
class DerivedFunction extends Function { }

const proxies = new Set;
function proxy(target) {
    let result = new Proxy(target, { });
    proxies.add(result);
    return result;
}

const values = [
    undefined, null, true, false,
    0, -0, 1, -1, 0x7fffffff, 0x80000000, 4.5, NaN, Infinity, 2 ** 53,
    "", "a", "a".repeat(3) + String(Math.random()),
    Symbol.iterator, Symbol("s"),
    0n, 1n, -1n, 2n ** 31n, 2n ** 70n,
    function () { }, () => { }, class { }, async function () { }, function* () { }, (function () { }).bind(null), new DerivedFunction,
    Array, Object, Proxy, Math.max,
    proxy(function () { }), proxy(class { }), proxy(Array),
    [], [1], [1.5], ["a"], new Array(100), new DerivedArray, [].concat([1]),
    { }, { a: 1 }, Object.create(null), new (class { }), /re/, new Date, new Map, new Set, new WeakMap, new Error, Promise.resolve(),
    new Uint8Array(1), new ArrayBuffer(1), new DataView(new ArrayBuffer(1)), new String("s"), new Number(1), new Boolean(false), Object(1n), Object(Symbol()),
    proxy({ }), proxy([]), proxy(new DerivedArray), Math, JSON, globalThis, (function () { return arguments; })(),
    (function () { "use strict"; return arguments; })(), [][Symbol.iterator](), (function* () { })(),
];
const tags = values.map(referenceTag);

let seen = 0;
for (let tag of tags)
    seen |= tag;
if (seen !== 1023)
    throw new Error("the value list does not cover every tag: " + seen);

const warmup = Math.max(4, Math.ceil(testLoopCount / 50));

for (let mask = 1; mask <= 1022; ++mask) {
    let check = new Function("v", `$$t(v, ${mask}); return $$t(v, ${mask});`);
    noInline(check);

    let accepted = [];
    let rejected = [];
    for (let i = 0; i < values.length; ++i)
        (tags[i] & mask ? accepted : rejected).push(i);

    for (let round = 0; round < 2; ++round) {
        for (let i = 0; i < warmup; ++i) {
            for (let index of accepted) {
                if (!Object.is(check(values[index]), values[index]))
                    throw new Error(`mask ${mask}: value ${index} was not returned unchanged`);
            }
        }
        for (let index of rejected) {
            let error = null;
            try {
                check(values[index]);
            } catch (e) {
                error = e;
            }
            if (!(error instanceof TypeError))
                throw new Error(`mask ${mask}: value ${index} (tag ${tags[index]}) was accepted`);
            let expected = `Type check failed: expected ${describeMask(mask)}, got ${describeMask(tags[index])}`;
            if (error.message !== expected)
                throw new Error(`mask ${mask}: bad message "${error.message}", expected "${expected}"`);
        }
    }
}
