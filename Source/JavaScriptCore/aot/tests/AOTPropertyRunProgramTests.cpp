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
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = Call v1, -, v2
    Return
}
function f1 name="storeSix" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = TypeTest v0 testedType=object
    Branch v2 -> taken b1(| 0), notTaken b2()
  b1(v3):
    PropSet v3, v1 key="a" mode=Strict
    PropSet v3, v1 key="b" mode=Strict
    PropSet v3, v1 key="c" mode=Strict
    PropSet v3, v1 key="d" mode=Strict
    PropSet v3, v1 key="e" mode=Strict
    PropSet v3, v1 key="f" mode=Strict
    Return
  b2():
    Return
}
)IR"_s;

// Each case runs for longer than the longest back-off of the one site cache, 255 misses, so that one of its misses is due for a refill. The very first miss of the site fills nothing: the site may never run again.
// The six names lie in a row in inline storage, on both sides of its end, in another order, or in a dictionary.
static constexpr ASCIILiteral statements = R"JS(
globalThis.publish = storeSix => {
    const calls = () => Number(aotOperationCount("operationAOTTryPutProperties"));
    const create = (before, names) => {
        const object = {};
        for (let i = 0; i < before; i++) object["before" + i] = 0;
        for (let i = 0; i < names.length; i++) object[names[i]] = 0;
        return object;
    };
    const measure = (what, object) => {
        const callsBefore = calls();
        for (let i = 1; i <= 300; i++) storeSix(object, i);
        let numberOfKeys = 0;
        for (const key in object) numberOfKeys++;
        print(what + ": " + (calls() - callsBefore) + " calls, " + [object.a, object.b, object.c, object.d, object.e, object.f] + ", " + numberOfKeys + " keys");
    };
    measure("in a row", create(0, "abcdef"));
    measure("across the end of inline storage", create(4, "abcdef"));
    measure("other order", create(0, "fedcba"));
    measure("dictionary", create(200, "abcdef"));
};
)JS"_s;

static void testRefusedRunCallsOperationOnce()
{
    checkOutputOfProgramInShell(programText,
        "in a row: 2 calls, 300,300,300,300,300,300, 6 keys\n"
        "across the end of inline storage: 1 calls, 300,300,300,300,300,300, 10 keys\n"
        "other order: 1 calls, 300,300,300,300,300,300, 6 keys\n"
        "dictionary: 1 calls, 300,300,300,300,300,300, 206 keys\n"_s,
        { }, statements, { "--aotCountsPath=/dev/null"_s });
}

// Each object is new and gets the six names: under no prototype, under prototypes that can change, where the cell has no room for all six, and in cells of other classes.
static constexpr ASCIILiteral statementsWithNewObjects = R"JS(
globalThis.publish = storeSix => {
    const calls = () => Number(aotOperationCount("operationAOTTryPutProperties"));
    const measure = (what, create) => {
        const callsBefore = calls();
        let object;
        for (let i = 1; i <= 300; i++) {
            object = create();
            storeSix(object, i);
        }
        print(what + ": " + (calls() - callsBefore) + " calls, " + Object.keys(object) + ", " + [object.a, object.b, object.c, object.d, object.e, object.f]);
    };
    const prototype = { inherited: 0 };
    const below = Object.create(prototype);
    measure("no prototype", () => Object.create(null));
    measure("Object.prototype", () => ({}));
    measure("other prototype", () => Object.create(prototype));
    measure("two prototypes", () => Object.create(below));
    measure("no room in the cell", () => { const object = Object.create(null); object.before = 0; return object; });
    measure("array", () => []);
    measure("function", () => function () { });
};
)JS"_s;

static void testAddedRunCallsOperationOnce()
{
    checkOutputOfProgramInShell(programText,
        "no prototype: 2 calls, a,b,c,d,e,f, 300,300,300,300,300,300\n"
        "Object.prototype: 1 calls, a,b,c,d,e,f, 300,300,300,300,300,300\n"
        "other prototype: 1 calls, a,b,c,d,e,f, 300,300,300,300,300,300\n"
        "two prototypes: 1 calls, a,b,c,d,e,f, 300,300,300,300,300,300\n"
        "no room in the cell: 1 calls, before,a,b,c,d,e,f, 300,300,300,300,300,300\n"
        "array: 1 calls, a,b,c,d,e,f, 300,300,300,300,300,300\n"
        "function: 1 calls, a,b,c,d,e,f, 300,300,300,300,300,300\n"_s,
        { }, statementsWithNewObjects, { "--aotCountsPath=/dev/null"_s });
}

// Of two Structures in turn one is in the site cache. The other one misses and takes single stores, unless the miss is due for a refill: the first two, then after 3, 15, 63 and 255 misses.
#define STATEMENTS_WITH_TWO_STRUCTURES_IN_TURN R"JS(
globalThis.publish = storeSix => {
    const prototypes = [{ inherited: 0 }, { inherited: 1 }];
    let wrong = 0;
    for (let i = 1; i <= 1000; i++) {
        const object = Object.create(prototypes[i & 1]);
        storeSix(object, i);
        if (Object.keys(object).join() !== "a,b,c,d,e,f" || object.a !== i || object.f !== i || object.inherited !== (i & 1))
            wrong++;
    }
    print(wrong + " wrong");
    if (globalThis.printsCallCounts)
        print(aotOperationCount("operationAOTTryPutProperties") + " calls");
};
)JS"

static void testTwoStructuresInTurn()
{
    checkOutputOfProgramInShell(programText, "0 wrong\n6 calls\n"_s, { }, "globalThis.printsCallCounts = true;" STATEMENTS_WITH_TWO_STRUCTURES_IN_TURN ""_s, { "--aotCountsPath=/dev/null"_s });
    for (ASCIILiteral word : { "validate"_s, "heat=EverythingIsHot"_s, "heat=EverythingIsCold"_s })
        checkOutputOfProgramInShell(programText, "0 wrong\n"_s, { word }, STATEMENTS_WITH_TWO_STRUCTURES_IN_TURN ""_s);
}

#define STATEMENTS_CHANGING_THE_CHAIN R"JS(
globalThis.publish = storeSix => {
    const calls = () => Number(aotOperationCount("operationAOTTryPutProperties"));
    const base = { inherited: 0 };
    const callCounts = [];
    const kept = [];
    let seen = 0;
    const measure = what => {
        const callsBefore = calls();
        let object;
        for (let i = 1; i <= 300; i++) {
            object = Object.create(base);
            try { storeSix(object, i); } catch (error) { if (!(error instanceof TypeError)) print(error); }
        }
        kept[kept.length] = object;
        callCounts[callCounts.length] = calls() - callsBefore;
        print(what + ": " + Object.keys(object) + ", " + [object.a, object.b, object.c, object.d, object.e, object.f] + ", seen " + seen);
    };
    measure("nothing inherited");
    base.other = 0;
    measure("other name on the prototype");
    Object.defineProperty(base, "c", { set(value) { seen += value; }, configurable: true });
    measure("inherited setter");
    delete base.c;
    measure("setter deleted");
    Object.defineProperty(base, "e", { value: -1, writable: false, configurable: true });
    measure("inherited read-only property");
    Object.defineProperty(base, "e", { value: -2, writable: true });
    measure("inherited writable property");
    Object.setPrototypeOf(base, { set f(value) { seen = -value; } });
    measure("setter above the prototype");
    Object.setPrototypeOf(base, null);
    measure("nothing above the prototype");
    if (globalThis.printsCallCounts)
        print("calls: " + callCounts);
};
)JS"

#define OUTPUT_WHILE_THE_CHAIN_CHANGES \
    "nothing inherited: a,b,c,d,e,f, 300,300,300,300,300,300, seen 0\n" \
    "other name on the prototype: a,b,c,d,e,f, 300,300,300,300,300,300, seen 0\n" \
    "inherited setter: a,b,d,e,f, 300,300,,300,300,300, seen 45150\n" \
    "setter deleted: a,b,c,d,e,f, 300,300,300,300,300,300, seen 45150\n" \
    "inherited read-only property: a,b,c,d, 300,300,300,300,-1,, seen 45150\n" \
    "inherited writable property: a,b,c,d,e,f, 300,300,300,300,300,300, seen 45150\n" \
    "setter above the prototype: a,b,c,d,e, 300,300,300,300,300,, seen -300\n" \
    "nothing above the prototype: a,b,c,d,e,f, 300,300,300,300,300,300, seen -300\n"

static void testChangeOfChainDropsRememberedRun()
{
    checkOutputOfProgramInShell(programText, OUTPUT_WHILE_THE_CHAIN_CHANGES "calls: 2,0,1,1,1,1,1,1\n"_s,
        { }, "globalThis.printsCallCounts = true;" STATEMENTS_CHANGING_THE_CHAIN ""_s, { "--aotCountsPath=/dev/null"_s });
    for (ASCIILiteral word : { "validate"_s, "references=narrow"_s, "heat=EverythingIsHot"_s, "heat=EverythingIsCold"_s, "disable=rule:PropertyRun"_s })
        checkOutputOfProgramInShell(programText, OUTPUT_WHILE_THE_CHAIN_CHANGES ""_s, { word }, STATEMENTS_CHANGING_THE_CHAIN ""_s, { "--aotCountsPath=/dev/null"_s });
}

std::span<const NativeTest> propertyRunProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "property-run/programs/refused-run-calls-operation-once"_s, testRefusedRunCallsOperationOnce },
        { "property-run/programs/added-run-calls-operation-once"_s, testAddedRunCallsOperationOnce },
        { "property-run/programs/change-of-chain-drops-remembered-run"_s, testChangeOfChainDropsRememberedRun },
        { "property-run/programs/two-structures-in-turn"_s, testTwoStructuresInTurn },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
