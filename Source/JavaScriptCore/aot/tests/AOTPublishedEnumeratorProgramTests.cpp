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
    v1 = NewObject -, v0, v0 layout=L0 fields=[0, 1]
    v2 = FinishObject v1
    Return v2
}
function f2 name="makeReversedPair" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = NewObject -, v0, v0 layout=L0 fields=[1, 0]
    v2 = FinishObject v1
    Return v2
}
function f3 name="walk" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
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
    ElemSet v0, v1, v2
    Return v0
}
function f5 name="remove" parameters=[{type=anything}, {type=string}] results=[{type=anything}] {
    v2 = ElemDelete v0, v1
    Return v0
}
function f6 name="setPrototype" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Builtin builtin=ObjectSetPrototypeOf
    v3 = Call v2, -, v0, v1
    Return v0
}
)IR"_s;

static constexpr ASCIILiteral statements = COUNTED_CALLS(R"("GetPropertyEnumerator", "OpenEnumeration")") R"JS(
globalThis.publish = (makePair, makeReversedPair, walk, set, remove, setPrototype) => {
    let keys = "";
    const note = key => { keys += " " + key; };
    const show = (what, create) => {
        const before = countedCalls();
        for (let i = 0; i < 50; i++) {
            keys = "";
            walk(create(i), note);
        }
        print(what + ":" + keys + servedBy(before, 50, 4));
    };
    show("pairs", makePair);
    show("reversed pairs", makeReversedPair);
    show("both orders in turn", i => i & 1 ? makePair(i) : makeReversedPair(i));
    show("one field deleted", i => remove(makePair(i), "first"));
    show("extra property", i => set(makePair(i), "third", i));
    show("another extra property", i => set(makePair(i), "fourth", i));
    show("extra property stored by the interpreter", i => { const pair = makePair(i); pair.third = i; return pair; });
    show("index", i => set(makePair(i), "7", i));

    const prototype = { inherited: 0 };
    show("other prototype", i => setPrototype(makePair(i), prototype));
    prototype[3] = 0;
    show("index on the other prototype", i => setPrototype(makePair(i), prototype));
    show("no prototype", i => setPrototype(makePair(i), null));
    show("no prototype and an extra property", i => set(setPrototype(makePair(i), null), "third", i));
    show("pairs after all that", makePair);
    gc();
    show("pairs after a collection", makePair);
    show("ordinary objects", i => ({ first: i, second: i }));
    show("nothing to walk", i => i & 1 ? null : undefined);
};
)JS"_s;

static constexpr ASCIILiteral expectedOutput =
    "pairs: first second\n"
    "reversed pairs: second first\n"
    "both orders in turn: first second\n"
    "one field deleted: second\n"
    "extra property: first second third\n"
    "another extra property: first second fourth\n"
    "extra property stored by the interpreter: first second third\n"
    "index: 7 first second\n"
    "other prototype: first second inherited\n"
    "index on the other prototype: first second 3 inherited\n"
    "no prototype: first second\n"
    "no prototype and an extra property: first second third\n"
    "pairs after all that: first second\n"
    "pairs after a collection: first second\n"
    "ordinary objects: first second\n"
    "nothing to walk:\n"_s;

static void testForInListsTheKeysOfEachObject()
{
    checkOutputOfProgramInShell(programText, expectedOutput, { "references=wide"_s }, statements);
    checkOutputOfProgramInShell(programText, expectedOutput, { "references=narrow"_s }, statements);
    checkServicesInShell(__FILE__, __LINE__, programText, expectedOutput, statements, wordsOfCaches, "cccccccnnncccccc"_s, { "references=narrow"_s });
}

std::span<const NativeTest> publishedEnumeratorProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "published-enumerators/programs/for-in-lists-the-keys-of-each-object"_s, testForInListsTheKeysOfEachObject },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
