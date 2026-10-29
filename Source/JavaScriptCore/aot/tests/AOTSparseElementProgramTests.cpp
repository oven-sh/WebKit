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
    v5 = Call v1, -, v2, v3, v4
    Return
}
function f1 name="read" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = ElemGet v0, v1
    Return v2
}
function f2 name="readOfArray" parameters=[{type=Array}, {type=number}] results=[{type=anything}] {
    v2 = ElemGet v0, v1
    Return v2
}
function f3 name="hasInArray" parameters=[{type=Array}, {type=number}] results=[{type=anything}] {
    v2 = ElemHas v0, v1
    Return v2
}
)IR"_s;

// 300 elements in a map are enough for buckets that several of them begin at.
#define STATEMENTS R"JS(
const operations = ["GetByVal", "GetByValAndCache", "GetByValGeneric", "InByValAndCache", "InByValGeneric"];
const calls = () => {
    let sum = 0;
    for (const name of operations) sum += Number(aotOperationCount("operationAOT" + name));
    return sum;
};
const first = 100000;
const step = 7;
const count = 300;
const fill = object => {
    for (let i = 0; i < count; i++) object[first + i * step] = i;
    return object;
};
const measure = (what, access) => {
    let held = 0;
    let sum = 0;
    let others = "";
    const ask = isAsked => {
        const callsBefore = calls();
        for (let index = first - 1; index <= first + count * step; index++) {
            if (!isAsked(index))
                continue;
            const answer = access(index);
            if (typeof answer === "number" || answer === true) {
                held++;
                sum += Number(answer);
            } else if (answer !== undefined && answer !== false)
                others += ", " + index + " " + answer;
        }
        return calls() - callsBefore <= 3 ? "no calls" : "calls";
    };
    const isPlace = index => index >= first && !((index - first) % step);
    const atPlaces = ask(isPlace);
    const between = ask(index => !isPlace(index));
    print(what + ": " + held + " held, sum " + sum + others + (globalThis.printsCalls ? ", " + atPlaces + " at the places, " + between + " between them" : ""));
};
globalThis.publish = (read, readOfArray, hasInArray) => {
    const measureAll = (what, array) => {
        measure(what + ", any receiver", index => read(array, index));
        measure(what + ", an array", index => readOfArray(array, index));
        measure(what + ", in an array", index => hasInArray(array, index));
    };
    const array = fill([]);
    measureAll("filled", array);
    for (let i = 0; i < count; i += 3) delete array[first + i * step];
    measureAll("a third deleted", array);
    for (let i = 0; i < count; i += 6) array[first + i * step] = i;
    measureAll("a sixth stored again", array);
    array[0] = "at the start";
    array[4294967294] = "at the last index";
    array[4294967295] = "under a name";
    for (const index of [0, 1, 4294967294, 4294967295, -1, 0.5])
        print(index + ": " + read(array, index) + ", " + readOfArray(array, index) + ", " + hasInArray(array, index));

    measureAll("frozen", Object.freeze(fill([])));
    const object = fill({});
    measure("an object", index => read(object, index));

    const withGetter = fill([]);
    Object.defineProperty(withGetter, first + step, { get() { return "from the getter"; }, configurable: true });
    measureAll("with a getter", withGetter);

    const belowElement = fill([]);
    Object.setPrototypeOf(belowElement, { __proto__: Array.prototype, [first + 1]: "inherited" });
    measureAll("below an element", belowElement);
};
)JS"

#define OUTPUT_LINE(what, served) what served "\n"

#define OUTPUT(neverCalls, callsBetween, alwaysCalls) \
    OUTPUT_LINE("filled, any receiver: 300 held, sum 44850", neverCalls) \
    OUTPUT_LINE("filled, an array: 300 held, sum 44850", neverCalls) \
    OUTPUT_LINE("filled, in an array: 300 held, sum 300", neverCalls) \
    OUTPUT_LINE("a third deleted, any receiver: 200 held, sum 30000", neverCalls) \
    OUTPUT_LINE("a third deleted, an array: 200 held, sum 30000", neverCalls) \
    OUTPUT_LINE("a third deleted, in an array: 200 held, sum 200", neverCalls) \
    OUTPUT_LINE("a sixth stored again, any receiver: 250 held, sum 37350", neverCalls) \
    OUTPUT_LINE("a sixth stored again, an array: 250 held, sum 37350", neverCalls) \
    OUTPUT_LINE("a sixth stored again, in an array: 250 held, sum 250", neverCalls) \
    "0: at the start, at the start, true\n" \
    "1: undefined, undefined, false\n" \
    "4294967294: at the last index, at the last index, true\n" \
    "4294967295: under a name, under a name, true\n" \
    "-1: undefined, undefined, false\n" \
    "0.5: undefined, undefined, false\n" \
    OUTPUT_LINE("frozen, any receiver: 300 held, sum 44850", callsBetween) \
    OUTPUT_LINE("frozen, an array: 300 held, sum 44850", callsBetween) \
    OUTPUT_LINE("frozen, in an array: 300 held, sum 300", alwaysCalls) \
    OUTPUT_LINE("an object: 300 held, sum 44850", callsBetween) \
    OUTPUT_LINE("with a getter, any receiver: 299 held, sum 44849, 100007 from the getter", callsBetween) \
    OUTPUT_LINE("with a getter, an array: 299 held, sum 44849, 100007 from the getter", callsBetween) \
    OUTPUT_LINE("with a getter, in an array: 300 held, sum 300", alwaysCalls) \
    OUTPUT_LINE("below an element, any receiver: 300 held, sum 44850, 100001 inherited", callsBetween) \
    OUTPUT_LINE("below an element, an array: 300 held, sum 44850, 100001 inherited", callsBetween) \
    OUTPUT_LINE("below an element, in an array: 301 held, sum 301", alwaysCalls)

static void testElementsInMapAreReadWithoutCalls()
{
    checkOutputOfProgramInShell(programText, OUTPUT(", no calls at the places, no calls between them", ", no calls at the places, calls between them", ", calls at the places, calls between them") ""_s,
        { }, "globalThis.printsCalls = true;" STATEMENTS ""_s, { "--aotCountsPath=/dev/null"_s });
    for (ASCIILiteral word : { "validate"_s, "heat=EverythingIsHot"_s, "heat=EverythingIsCold"_s })
        checkOutputOfProgramInShell(programText, OUTPUT("", "", "") ""_s, { word }, STATEMENTS ""_s, { "--aotCountsPath=/dev/null"_s });
}

std::span<const NativeTest> sparseElementProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "sparse-elements/programs/elements-in-a-map-are-read-without-calls"_s, testElementsInMapAreReadWithoutCalls },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
