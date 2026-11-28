/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = NewClosure function=f4
    v6 = Call v1, -, v2, v3, v4, v5
    Return
}
function f1 name="bindThis" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = PropGet v0 key="bind"
    v3 = Call v2, v0, v1 calleeKey="bind"
    Return v3
}
function f2 name="bindThree" parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    v5 = PropGet v0 key="bind"
    v6 = Call v5, v0, v1, v2, v3, v4 calleeKey="bind"
    Return v6
}
function f3 name="sum" flags=[HasThis] length=4 parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    v5 = Add v0, v1
    v6 = Add v5, v2
    v7 = Add v6, v3
    v8 = Add v7, v4
    Return v8
}
function f4 name="later" flags=[IsAsync] results=[{type=anything}] {
    Return
}
)IR"_s;

static constexpr ASCIILiteral statements = COUNTED_CALLS(R"("PrepareBoundFunctionAllocation")") R"JS(
const prepared = () => countsCalls ? ", " + countedCalls() : "";
const describe = bound => [bound.name, bound.length, typeof bound, Object.getOwnPropertyNames(bound).sort().join("+")].join(" ");
globalThis.publish = (bindThis, bindThree, sum, later) => {
    print("host function: " + describe(bindThis(Math.max, null)) + prepared());
    print("async function: " + describe(bindThis(later, null)) + prepared());
    print("object: " + (() => { try { return bindThis({ bind: Function.prototype.bind }, null); } catch (error) { return error.constructor.name; } })() + prepared());
    const boundSum = bindThis(sum, null);
    print("bound function: " + describe(bindThis(boundSum, null)) + prepared());
    for (let i = 0; i < 3; i++)
        print("closure: " + describe(bindThis(sum, "a")) + ", " + bindThis(sum, "a")("b", "c", "d", "e") + prepared());
    print("three arguments: " + describe(bindThree(sum, "a", "b", "c", "d")) + ", " + bindThree(sum, "a", "b", "c", "d")("e") + prepared());
    sum.extra = 1;
    print("closure with a property: " + describe(bindThis(sum, "a")) + ", " + bindThis(sum, "a")("b", "c", "d", "e"));
};
)JS"_s;

static void testBindFoundByName()
{
    checkOutputOfProgramInShell(programText,
        "host function: bound max 2 function length+name\n"
        "async function: bound later 0 function length+name\n"
        "object: TypeError\n"
        "bound function: bound bound sum 4 function length+name\n"
        "closure: bound sum 4 function length+name, abcde\n"
        "closure: bound sum 4 function length+name, abcde\n"
        "closure: bound sum 4 function length+name, abcde\n"
        "three arguments: bound sum 1 function length+name, abcde\n"
        "closure with a property: bound sum 4 function length+name, abcde\n"_s,
        { }, statements);
    AOT_TEST_CHECK(outputOfProgramInShell(programText, { }, statements, { "--aotCountsPath=/dev/null"_s }, waysToCompilePrograms[0]) == "host function: bound max 2 function length+name, 0\nasync function: bound later 0 function length+name, 0\nobject: TypeError, 0\nbound function: bound bound sum 4 function length+name, 1\nclosure: bound sum 4 function length+name, abcde, 1\nclosure: bound sum 4 function length+name, abcde, 1\nclosure: bound sum 4 function length+name, abcde, 1\nthree arguments: bound sum 1 function length+name, abcde, 1\nclosure with a property: bound sum 4 function length+name, abcde\n"_s);
}

static constexpr ASCIILiteral programTextOfCallsByValue = R"IR(# LIBRARY: own
module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = Builtin builtin=RegExp
    v6 = PropGet v5 key="lastMatch"
    v7 = Call v1, -, v2, v3, v4
    Return
}
function f1 name="callWithNothing" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Call v0, v1
    Return v2
}
function f2 name="callWithOne" parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = Call v0, v1, v2
    Return v3
}
function f3 name="callWithTwo" parameters=[{type=anything}, {type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    v4 = Call v0, v1, v2, v3
    Return v4
}
)IR"_s;

static constexpr ASCIILiteral statementsComparingWithHostFunctions = R"JS(
const thenOfPromises = Promise.prototype.then;
const stateOf = promise => {
    let state = "pending";
    thenOfPromises.call(promise, value => { state = "fulfilled " + show(value); }, reason => { state = "rejected " + show(reason); });
    drainMicrotasks();
    return state;
};
const show = value => {
    if (typeof value === "string")
        return JSON.stringify(value);
    if (value instanceof Promise)
        return String(value.constructor.name) + "<" + stateOf(value) + ">";
    if (typeof value === "function" && value.seen)
        return "handler" + show(value.seen);
    if (Object.is(value, -0))
        return "-0";
    if (Array.isArray(value))
        return "[" + Array.from(value, show).join(",") + "]";
    if (value instanceof Map || value instanceof Set)
        return value.constructor.name + show([...value]);
    if (value instanceof RegExp)
        return String(value) + "@" + show(value.lastIndex);
    if (value instanceof WeakMap)
        return "WeakMap" + show(weakKeysLookedFor.map(sought => value.get(sought)));
    if (value instanceof WeakSet)
        return "WeakSet" + show(weakKeysLookedFor.map(sought => value.has(sought)));
    return String(value);
};
const attempt = run => { try { return show(run()); } catch (error) { return error.constructor.name; } };
const getter = (holder, name) => Object.getOwnPropertyDescriptor(holder, name).get;
const rope = (left, right) => () => left + right;
const key = {};
const otherKey = {};
const symbolKey = Symbol("key");
const weakKeysLookedFor = [key, otherKey, symbolKey, Symbol.iterator];
const revoked = Proxy.revocable([], {});
revoked.revoke();
class Pattern extends RegExp { exec(text) { return text === "own" ? ["own"] : null; } }
const withOwnExec = () => Object.assign(/a/, { exec: () => null });
const atLastIndex = (pattern, lastIndex) => () => Object.assign(new RegExp(pattern), { lastIndex });
const detached = () => { const view = new Uint8Array(4); view.buffer.transfer(); return view; };

globalThis.publish = (...calls) => {
    const check = (name, callee, makeReceiver, ...operands) => {
        const results = [(receiver, ...rest) => calls[rest.length](callee, receiver, ...rest), (receiver, ...rest) => callee.call(receiver, ...rest)].map(call => {
            const receiver = makeReceiver();
            const passed = operands.map(operand => typeof operand === "function" && operand.makesOperand ? operand() : operand);
            return [attempt(() => call(receiver, ...passed)), attempt(() => receiver), attempt(() => passed), show(RegExp.lastMatch)].join(" ");
        });
        if (results[0] !== results[1])
            print(name + "(" + operands.map(operand => attempt(() => operand)).join(", ") + "): " + results[0] + ", the host function: " + results[1]);
    };
    const fresh = make => Object.assign(make, { makesOperand: true });
    const each = (name, callee, receivers, lists) => {
        for (const makeReceiver of receivers) {
            for (const operands of lists)
                check(name, callee, makeReceiver, ...operands);
        }
    };
    const nothing = [() => undefined];
    const values = [0, -0, 1, 1.5, NaN, Infinity, -Infinity, 2 ** 31, "", "a", "1", true, false, null, undefined, 1n, symbolKey, key, [], () => 1];
    const texts = [() => "", () => "abcabc", () => " \u{a0}\u{feff}ab\u{2003}\n", rope("abc,", "abc,own"), () => "a\u{f1}\u{20ac}\u{1f600}\ud800", rope("\u{20ac}", "a,b"), () => 5, () => null, () => undefined, () => new String("abc"), () => key, () => symbolKey];
    const positions = [0, 1, 2, 5, 6, 100, -1, -100, 1.5, NaN, 2 ** 31, -(2 ** 31), "1", undefined, null, { valueOf: () => 1 }, 1n];

    each("Array.isArray", Array.isArray, nothing, [...values, new Proxy([], {}), new Proxy({}, {}), revoked.proxy, new (class extends Array {})()].map(value => [value]));
    each("Object.is", Object.is, nothing, values.flatMap(first => [...values, "a".repeat(1), 2n - 1n, {}].map(second => [first, second])));
    each("isNaN", isNaN, nothing, [...values, { valueOf: () => NaN }, "x"].map(value => [value]));
    each("JSON.stringify", JSON.stringify, nothing, [...values, "a\"b\\c\n", "\u{e9}", "\u007f", "\u{2028}", "\ud800", "\u{1f600}", "x".repeat(15), "x".repeat(16) + "\"", "y".repeat(100) + "\u0001", "\u{20ac}".repeat(40), { toJSON: () => 1 }, new String("a")].flatMap(value => [[value], [value, null], [value, undefined], [value, () => 2], [value, ["a"]]]));

    const maps = [() => new Map(), () => new Map([[1, 2], ["a", key], [key, NaN], [NaN, 0]]), () => new (class extends Map {})([[1, 2]]), () => new Set([1]), () => new WeakMap(), () => key, () => 1];
    const sets = [() => new Set(), () => new Set([1, "a", key, NaN, 0]), () => new (class extends Set {})([1]), () => new Map([[1, 2]]), () => new WeakSet(), () => key, () => undefined];
    each("Map.prototype.size", getter(Map.prototype, "size"), maps, [[]]);
    each("Set.prototype.size", getter(Set.prototype, "size"), sets, [[]]);
    each("Set.prototype.delete", Set.prototype.delete, sets, values.map(value => [value]));
    for (const callee of [Map.prototype.get, Map.prototype.has, Map.prototype.delete])
        each("Map.prototype." + callee.name, callee, maps, values.map(value => [value]));
    for (const callee of [Set.prototype.has, Set.prototype.add])
        each("Set.prototype." + callee.name, callee, sets, values.map(value => [value]));
    each("Map.prototype.set", Map.prototype.set, maps, values.flatMap(value => [[value, 7], [value, key]]));

    const weakKeys = [...values, otherKey, Symbol.for("registered"), Symbol.iterator];
    const weakMaps = [() => new WeakMap(), () => new WeakMap([[key, 1], [symbolKey, -0]]), () => new (class extends WeakMap {})([[key, key]]), () => new WeakSet([key]), () => new Map([[key, 1]]), () => key, () => null];
    const weakSets = [() => new WeakSet(), () => new WeakSet([key, symbolKey]), () => new (class extends WeakSet {})([key]), () => new WeakMap([[key, 1]]), () => new Set([key]), () => key, () => null];
    each("WeakMap.prototype.get", WeakMap.prototype.get, weakMaps, weakKeys.map(value => [value]));
    each("WeakMap.prototype.has", WeakMap.prototype.has, weakMaps, weakKeys.map(value => [value]));
    each("WeakSet.prototype.has", WeakSet.prototype.has, weakSets, weakKeys.map(value => [value]));
    each("WeakMap.prototype.set", WeakMap.prototype.set, weakMaps, weakKeys.flatMap(value => [[value, 7], [value, key]]));

    const views = [() => new Uint8Array(3), () => new Float64Array(0), () => new BigInt64Array(2), () => new Uint8Array(new ArrayBuffer(8), 2, 4), () => new Uint8Array(new ArrayBuffer(4, { maxByteLength: 8 })), () => new Uint8Array(new ArrayBuffer(4, { maxByteLength: 8 }), 1, 2), detached, () => new DataView(new ArrayBuffer(4)), () => new ArrayBuffer(4), () => [], () => 1];
    each("%TypedArray%.prototype.length", getter(Object.getPrototypeOf(Uint8Array.prototype), "length"), views, [[]]);

    const lists = [() => [], () => ["b"], () => ["b", "a", "c", "a"], () => ["b", 1, "a", undefined], () => ["b", , "a"], () => [2, 10, 1], () => ["\u{20ac}", "\u{1f600}", "a", "B"], () => Object.freeze(["b", "a"]), () => Object.freeze(["a", "b"]), () => Array.from({ length: 64 }, (_, index) => "k" + (63 - index)), () => Array.from({ length: 65 }, (_, index) => "k" + (64 - index)), () => ["ab" + "c".repeat(2), rope("a", "b")()], () => new (class extends Array {})("b", "a"), () => ({ 0: "b", 1: "a", length: 2 }), () => "ba", () => null];
    each("Array.prototype.sort", Array.prototype.sort, lists, [[], [undefined], [(left, right) => (left < right ? 1 : -1)], [null], [1]]);

    each("String.prototype.charAt", String.prototype.charAt, texts, positions.map(position => [position]));
    each("String.prototype.charCodeAt", String.prototype.charCodeAt, texts, positions.map(position => [position]));
    each("String.prototype.codePointAt", String.prototype.codePointAt, texts, positions.map(position => [position]));
    for (const callee of [String.prototype.substring, String.prototype.slice])
        each("String.prototype." + callee.name, callee, texts, positions.flatMap(start => [[start], ...positions.map(end => [start, end])]));
    for (const callee of [String.prototype.toUpperCase, String.prototype.toLowerCase, String.prototype.trim, String.prototype.trimEnd, String.prototype.trimStart])
        each("String.prototype." + callee.name, callee, [...texts, () => "\u{df}\u{131}i", () => "  ", () => "\u{a0}\u{2003}", () => "a ", () => "\u{feff}a"], [[]]);

    const sought = ["", "a", "abc", "c,own", "\u{20ac}", "\ud83d", rope("b", "c")(), "abcabcabc", 5, null, undefined, key, /a/, Object.assign(/a/, { [Symbol.match]: false }), symbolKey];
    for (const callee of [String.prototype.endsWith, String.prototype.includes, String.prototype.startsWith, String.prototype.indexOf])
        each("String.prototype." + callee.name, callee, texts, sought.map(value => [value]));

    const patterns = ["", "a", "bc", ",", "$", /a/, /a/g, /(b)(?<last>c)/g, /a/y, /^/g, /\u{20ac}/u, /(?:)/g, /z/g, 5, null, undefined, key, { [Symbol.replace]: () => "replaced", [Symbol.split]: () => "split" }]
        .map(pattern => pattern instanceof RegExp ? fresh(() => new RegExp(pattern)) : pattern)
        .concat([fresh(atLastIndex(/a/g, 4)), fresh(atLastIndex(/a/y, 3)), fresh(atLastIndex(/a/g, "2")), fresh(atLastIndex(/a/, { valueOf: () => 1 })), fresh(() => Object.freeze(/a/g)), fresh(withOwnExec), fresh(() => new Pattern("a", "g"))]);
    const replacements = ["", "x", "$&$&", "[$1|$<last>|$`|$'|$$]", match => match.toUpperCase(), (...all) => all.length, () => "$&", 5, undefined, key];
    each("String.prototype.replace", String.prototype.replace, texts, patterns.flatMap(pattern => replacements.map(replacement => [pattern, replacement])));
    each("String.prototype.replaceAll", String.prototype.replaceAll, texts, patterns.flatMap(pattern => replacements.map(replacement => [pattern, replacement])));
    each("String.prototype.split", String.prototype.split, texts, patterns.flatMap(pattern => [[pattern], [pattern, 2], [pattern, undefined]]));

    const testers = [() => /a/, () => /a/g, () => /a/y, () => /(b)c/, () => /\ud83d/u, atLastIndex(/a/g, 1), atLastIndex(/a/g, 100), atLastIndex(/a/y, -1), atLastIndex(/a/g, "3"), atLastIndex(/a/, { valueOf: () => { throw new RangeError(); } }), atLastIndex(/a/g, { valueOf: () => { throw new RangeError(); } }), () => Object.freeze(/a/g), () => Object.freeze(/z/g), withOwnExec, () => new Pattern("a"), () => ({ exec: () => key }), () => key, () => "a", () => null];
    each("RegExp.prototype.test", RegExp.prototype.test, testers, ["", "a", "abcabc", rope("xx", "abc")(), "\u{1f600}", "own", 5, null, undefined, key, symbolKey].map(value => [value]));

    const later = settle => () => new Promise((resolve, reject) => { thenOfPromises.call(Promise.resolve(), () => settle(resolve, reject)); });
    const promises = [() => Promise.resolve(1), () => Promise.reject(2), () => new Promise(() => { }), later(resolve => resolve(3)), later((resolve, reject) => reject(4)), later(resolve => resolve(Promise.reject(5))),
        () => { const made = Promise.resolve(6); made.then(() => { }); return made; }, () => { const made = later(resolve => resolve(7))(); made.then(() => { }); return made; },
        () => Object.assign(Promise.resolve(8), { extra: 1 }), () => new (class Below extends Promise { })(resolve => resolve(9)), () => Object.assign(Promise.resolve(10), { then: () => "own" }),
        () => Object.assign(Promise.resolve(11), { constructor: { [Symbol.species]: class Other extends Promise { } } }), () => Object.assign(Promise.reject(12), { constructor: undefined }),
        () => Object.setPrototypeOf(Promise.resolve(13), { }), () => ({ then: () => "thenable" }), () => key, () => 1, () => undefined];
    const handlerThat = answer => fresh(() => {
        const handler = value => { handler.seen.push(value); return answer(value); };
        return Object.assign(handler, { seen: [] });
    });
    const handlers = [handlerThat(value => value + 100), handlerThat(value => { throw value; }), handlerThat(value => Promise.resolve(value)), handlerThat(value => Promise.reject(value)), handlerThat(() => ({ then: resolve => resolve("thenable") })),
        handlerThat(() => { }), fresh(() => Math.abs), fresh(() => (value => value).bind(null)), fresh(() => new Proxy(value => value, { })), fresh(() => class { }), undefined, null, 1, "a", key];
    const bothKinds = () => {
        each("Promise.prototype.then", thenOfPromises, promises, [[], ...handlers.map(handler => [handler]), ...handlers.flatMap(first => handlers.slice(0, 3).concat([undefined]).map(second => [first, second]))]);
        each("Promise.prototype.catch", Promise.prototype.catch, promises, [[], ...handlers.map(handler => [handler]), [handlers[0], handlers[1]]]);
    };
    bothKinds();

    print("done");
};
)JS"_s;

static void testAnswersOfHostFunctionsCalledByValue()
{
    checkOutputOfProgramInShell(programTextOfCallsByValue, "done\n"_s, { }, statementsComparingWithHostFunctions);
}

std::span<const NativeTest> boxedCallProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "boxed-calls/programs/bind-found-by-name"_s, testBindFoundByName },
        { "boxed-calls/programs/answers-of-host-functions"_s, testAnswersOfHostFunctionsCalledByValue },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
