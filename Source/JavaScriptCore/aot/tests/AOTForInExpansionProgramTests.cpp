/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Triple" fields=[{key="first" declared={type=anything}}, {key="second" declared={type=anything}}, {key="third" declared={type=anything}}]
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
    v8 = NewClosure function=f7
    v9 = NewClosure function=f8
    v10 = Call v1, -, v2, v3, v4, v5, v6, v7, v8, v9
    Return
}
function f1 name="makeTriple" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = NewObject -, v0, v0, v0 layout=L0 fields=[0, 1, 2]
    v2 = FinishObject v1
    Return v2
}
function f2 name="makeReversed" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = NewObject -, v0, v0, v0 layout=L0 fields=[2, 1, 0]
    v2 = FinishObject v1
    Return v2
}
function f3 name="walk" parameters=[{type=L0 flags=[IsExpected]}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything ~ L0, v1: anything):
    v2 = ForInOpen v0
    Jump -> next b1()
  b1():
    LoopHint
    v4 = ForInStep v2, v0
    v5 = ResultAt v4 index=0
    v6 = ResultAt v4 index=1
    Branch v5 -> taken b3(), notTaken b2()
  b2():
    v7 = Call v1, -, v6, v0
    Jump -> next b1()
  b3():
    Return
}
function f4 name="copy" parameters=[{type=anything}, {type=L0 flags=[IsExpected]}] results=[{type=anything}] {
  b0(v0: anything, v1: anything ~ L0):
    v2 = ForInOpen v1
    Jump -> next b1()
  b1():
    LoopHint
    v4 = ForInStep v2, v1
    v5 = ResultAt v4 index=0
    v6 = ResultAt v4 index=1
    Branch v5 -> taken b3(), notTaken b2()
  b2():
    v7 = ElemGet v1, v6
    ElemSet v0, v6, v7
    Jump -> next b1()
  b3():
    Return v0
}
function f5 name="firstKey" parameters=[{type=L0 flags=[IsExpected]}] results=[{type=anything}] {
  b0(v0: anything ~ L0):
    v1 = ForInOpen v0
    Jump -> next b1()
  b1():
    LoopHint
    v3 = ForInStep v1, v0
    v4 = ResultAt v3 index=0
    v5 = ResultAt v3 index=1
    Branch v4 -> taken b3(), notTaken b2()
  b2():
    Return v5
  b3():
    v6 = String string="none"
    Return v6
}
function f6 name="set" parameters=[{type=anything}, {type=string}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: string, v2: anything):
    ElemSet v0, v1, v2
    Return v0
}
function f7 name="remove" parameters=[{type=anything}, {type=string}] results=[{type=anything}] {
  b0(v0: anything, v1: string):
    v2 = ElemDelete v0, v1
    Return v0
}
function f8 name="setPrototype" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Builtin builtin=ObjectSetPrototypeOf
    v3 = Call v2, -, v0, v1
    Return v0
}
)IR"_s;

// The callbacks change the object while a loop that has a copy of its body for each name walks it.
static constexpr ASCIILiteral statements = R"JS(
globalThis.publish = (makeTriple, makeReversed, walk, copy, firstKey, set, remove, setPrototype) => {
    const show = (what, object, change) => {
        let seen = "";
        try {
            walk(object, (key, walked) => {
                seen += " " + key + "=" + walked[key];
                change(key, walked);
            });
        } catch (error) {
            seen += " threw " + error;
        }
        print(what + ":" + seen);
    };
    const nothing = () => { };
    for (let round = 0; round < 3; round++) {
        show("unchanged", makeTriple(round), nothing);
        show("other order", makeReversed(round), nothing);
    }
    show("next name deleted", makeTriple(1), (key, walked) => { if (key === "first") remove(walked, "second"); });
    show("last name deleted", makeTriple(1), (key, walked) => { if (key === "first") remove(walked, "third"); });
    show("own name deleted", makeTriple(1), (key, walked) => { remove(walked, key); });
    show("all deleted at once", makeTriple(1), (key, walked) => { remove(walked, "first"); remove(walked, "second"); remove(walked, "third"); });
    show("deleted and stored again", makeTriple(1), (key, walked) => { if (key === "first") { remove(walked, "second"); set(walked, "second", 9); } });
    show("name added", makeTriple(1), (key, walked) => { if (key === "first") set(walked, "fourth", 4); });
    show("index added", makeTriple(1), (key, walked) => { if (key === "second") set(walked, "5", 5); });
    show("value stored", makeTriple(1), (key, walked) => { set(walked, "third", key); });
    show("value of another kind stored", makeTriple(1), (key, walked) => { if (key === "first") set(walked, "second", { toString() { return "object"; } }); });
    show("many names added", makeTriple(1), (key, walked) => { if (key === "first") for (let i = 0; i < 200; i++) set(walked, "name" + i, i); });
    show("prototype changed", makeTriple(1), (key, walked) => { if (key === "first") setPrototype(walked, { inherited: 7 }); });
    show("prototype set before", setPrototype(makeTriple(1), { inherited: 7, second: 8 }), nothing);
    show("no prototype", setPrototype(makeTriple(1), null), nothing);
    show("made an accessor", makeTriple(1), (key, walked) => { if (key === "first") Object.defineProperty(walked, "second", { get() { return "got"; }, enumerable: true, configurable: true }); });
    show("hidden", makeTriple(1), (key, walked) => { if (key === "first") Object.defineProperty(walked, "second", { enumerable: false }); });
    show("frozen", makeTriple(1), (key, walked) => { Object.freeze(walked); });
    show("callback throws", makeTriple(1), key => { if (key === "second") throw "stop"; });
    show("extra name before", set(makeTriple(1), "fourth", 4), nothing);
    show("one name missing before", remove(makeTriple(1), "second"), nothing);
    show("ordinary object", { first: 1, second: 2, third: 3 }, nothing);
    show("array", [1, 2], nothing);
    show("string", "ab", nothing);
    show("null", null, nothing);
    show("undefined", undefined, nothing);
    show("number", 5, nothing);

    const list = object => { let text = ""; for (const key in object) text += " " + key + "=" + object[key]; return text; };
    print("copy:" + list(copy({}, makeTriple(2))));
    print("copy of the other order:" + list(copy({}, makeReversed(2))));
    print("copy into itself:" + list((triple => copy(triple, triple))(makeTriple(3))));
    print("copy into a triple:" + list(copy(makeTriple(1), makeReversed(2))));
    print("copy through a setter:" + list(copy({ set second(value) { this.seen = value; } }, makeTriple(4))));
    const source = makeTriple(5);
    print("copy into an object that changes the source:" + list(copy({ set first(value) { remove(source, "third"); } }, source)));
    print("first key: " + firstKey(makeTriple(1)) + " " + firstKey(makeReversed(1)) + " " + firstKey({}) + " " + firstKey(remove(makeTriple(1), "first")));
};
)JS"_s;

static constexpr ASCIILiteral expectedOutput =
    "unchanged: first=0 second=0 third=0\n"
    "other order: third=0 second=0 first=0\n"
    "unchanged: first=1 second=1 third=1\n"
    "other order: third=1 second=1 first=1\n"
    "unchanged: first=2 second=2 third=2\n"
    "other order: third=2 second=2 first=2\n"
    "next name deleted: first=1 third=1\n"
    "last name deleted: first=1 second=1\n"
    "own name deleted: first=1 second=1 third=1\n"
    "all deleted at once: first=1\n"
    "deleted and stored again: first=1 second=9 third=1\n"
    "name added: first=1 second=1 third=1\n"
    "index added: first=1 second=1 third=1\n"
    "value stored: first=1 second=1 third=second\n"
    "value of another kind stored: first=1 second=object third=1\n"
    "many names added: first=1 second=1 third=1\n"
    "prototype changed: first=1 second=1 third=1\n"
    "prototype set before: first=1 second=1 third=1 inherited=7\n"
    "no prototype: first=1 second=1 third=1\n"
    "made an accessor: first=1 second=got third=1\n"
    "hidden: first=1 third=1\n"
    "frozen: first=1 second=1 third=1\n"
    "callback throws: first=1 second=1 threw stop\n"
    "extra name before: first=1 second=1 third=1 fourth=4\n"
    "one name missing before: first=1 third=1\n"
    "ordinary object: first=1 second=2 third=3\n"
    "array: 0=1 1=2\n"
    "string: 0=a 1=b\n"
    "null:\n"
    "undefined:\n"
    "number:\n"
    "copy: first=2 second=2 third=2\n"
    "copy of the other order: third=2 second=2 first=2\n"
    "copy into itself: first=3 second=3 third=3\n"
    "copy into a triple: first=2 second=2 third=2\n"
    "copy through a setter: second=undefined first=4 seen=4 third=4\n"
    "copy into an object that changes the source: first=undefined second=5\n"
    "first key: first third none second\n"_s;

static void testLoopSeesWhatHappensToTheObject()
{
    for (ASCIILiteral word : { "heat=EverythingIsHot"_s, "heat=EverythingIsCold"_s, "disable=pass:ExpandOwnKeys"_s, "references=narrow"_s })
        checkOutputOfProgramInShell(programText, expectedOutput, { word }, statements);
    checkOutputOfProgramInShell(programText, expectedOutput, { }, statements);
}

std::span<const NativeTest> forInExpansionProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "for-in-expansion/programs/loop-sees-what-happens-to-the-object"_s, testLoopSeesWhatHappensToTheObject },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
