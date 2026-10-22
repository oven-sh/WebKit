/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Pair" fields=[{key="first" declared={type=anything}}, {key="second" declared={type=anything}}]
module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = NewClosure function=f4
    v6 = NewClosure function=f5
    v7 = NewClosure function=f6
    v8 = Call v1, -, v2, v3, v4, v5, v6, v7
    Return
}
function f1 name="makePair" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = NewObject -, v0, v0 layout=L0 fields=[0, 1]
    v2 = FinishObject v1
    Return v2
}
function f2 name="makeReversedPair" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = NewObject -, v0, v0 layout=L0 fields=[1, 0]
    v2 = FinishObject v1
    Return v2
}
function f3 name="walk" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = ForInOpen v0
    Jump -> next b1()
  b1():
    LoopHint
    v4 = ForInStep v2, v0
    v5 = ResultAt v4 index=0
    v6 = ResultAt v4 index=1
    Branch v5 -> taken b3(), notTaken b2()
  b2():
    v7 = Call v1, -, v6
    Jump -> next b1()
  b3():
    Return
}
function f4 name="set" parameters=[{type=anything}, {type=string}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: string, v2: anything):
    ElemSet v0, v1, v2
    Return v0
}
function f5 name="remove" parameters=[{type=anything}, {type=string}] results=[{type=anything}] {
  b0(v0: anything, v1: string):
    v2 = ElemDelete v0, v1
    Return v0
}
function f6 name="setPrototype" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Builtin builtin=ObjectSetPrototypeOf
    v3 = Call v2, -, v0, v1
    Return v0
}
)IR"_s;

// Each line walks fifty new objects. Objects of one Structure and one key order ask the run time for their enumerator once, and once more after each collection, whenever that comes: "found".
// So do objects with extra properties, once their Structure names the Structure of those.
static constexpr ASCIILiteral statements = R"JS(
globalThis.publish = (makePair, makeReversedPair, walk, set, remove, setPrototype) => {
    const calls = () => Number(aotOperationCount("operationAOTGetPropertyEnumerator"));
    let keys = "";
    const note = key => { keys += " " + key; };
    const measure = (what, create) => {
        const callsBefore = calls();
        for (let i = 0; i < 50; i++) {
            keys = "";
            walk(create(i), note);
        }
        const numberOfCalls = calls() - callsBefore;
        print(what + ": " + (numberOfCalls < 5 ? "found" : numberOfCalls + " calls") + "," + keys);
    };
    measure("pairs", makePair);
    measure("reversed pairs", makeReversedPair);
    measure("both orders in turn", i => i & 1 ? makePair(i) : makeReversedPair(i));
    measure("one field deleted", i => remove(makePair(i), "first"));
    measure("extra property", i => set(makePair(i), "third", i));
    measure("another extra property", i => set(makePair(i), "fourth", i));
    measure("extra property stored by the interpreter", i => { const pair = makePair(i); pair.third = i; return pair; });
    measure("index", i => set(makePair(i), "7", i));

    const prototype = { inherited: 0 };
    measure("other prototype", i => setPrototype(makePair(i), prototype));
    prototype[3] = 0;
    measure("index on the other prototype", i => setPrototype(makePair(i), prototype));
    measure("no prototype", i => setPrototype(makePair(i), null));
    measure("no prototype and an extra property", i => set(setPrototype(makePair(i), null), "third", i));
    measure("pairs after all that", makePair);
    gc();
    measure("pairs after a collection", makePair);
    measure("ordinary objects", i => ({ first: i, second: i }));
    measure("nothing to walk", i => i & 1 ? null : undefined);
};
)JS"_s;

static constexpr ASCIILiteral expectedOutput =
    "pairs: found, first second\n"
    "reversed pairs: found, second first\n"
    "both orders in turn: found, first second\n"
    "one field deleted: found, second\n"
    "extra property: found, first second third\n"
    "another extra property: found, first second fourth\n"
    "extra property stored by the interpreter: found, first second third\n"
    "index: 50 calls, 7 first second\n"
    "other prototype: 50 calls, first second inherited\n"
    "index on the other prototype: 50 calls, first second 3 inherited\n"
    "no prototype: 50 calls, first second\n"
    "no prototype and an extra property: found, first second third\n"
    "pairs after all that: found, first second\n"
    "pairs after a collection: found, first second\n"
    "ordinary objects: found, first second\n"
    "nothing to walk: found,\n"_s;

static void testObjectsOfOneKeyOrderAskOnce()
{
    checkOutputOfProgramInShell(programText, expectedOutput, { }, statements, { "--aotCountsPath=/dev/null"_s });
    for (ASCIILiteral word : { "validate"_s, "references=narrow"_s, "heat=EverythingIsHot"_s, "heat=EverythingIsCold"_s })
        checkOutputOfProgramInShell(programText, expectedOutput, { word }, statements, { "--aotCountsPath=/dev/null"_s });
}

std::span<const NativeTest> publishedEnumeratorProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "published-enumerators/programs/objects-of-one-key-order-ask-once"_s, testObjectsOfOneKeyOrderAskOnce },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
