/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral typedFunctionsCalledByValue = R"IR(module m0 path="main.ts" displayPath="main.ts" body=f0
entry module=m0
typesite w0 module=m0 position=10 kind=Result
typesite w1 module=m0 position=20 kind=Result
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = PropGet v0 key="id" [isFreeName]
    v3 = NewClosure function=f1
    v4 = Call v2, -, v3
    v5 = Integer integer=400000
    v6 = Call v4, -, v5, v4
    v7 = Call v1, -, v6
    v8 = Call v3, -, v5, v4
    v9 = Call v1, -, v8
    v10 = NewClosure function=f2
    v11 = Call v2, -, v10
    Jump -> next b1()
  b1() handler=b2():
    v12 = Call v4, -, v5, v11
    v13 = Call v1, -, v12
    Jump -> next b3()
  b2() role=CatchHandler:
    v14 = CatchValue
    v15 = PropGet v14 key="name"
    v16 = Call v1, -, v15
    Jump -> next b3()
  b3():
    v17 = Call v11, -, v5, v11
    v18 = Call v1, -, v17
    Return
}
function f1 name="down" flags=[IsChecked] parameters=[{type=anything}, {type=anything}] results=[{type=undefined|null|number site=w0}] {
    v2 = Integer
    v3 = StrictEq v0, v2
    Branch v3 -> taken b1(), notTaken b2()
  b1():
    v4 = Double number=0.5
    Return v4
  b2():
    v5 = Integer integer=1
    v6 = Sub v0, v5
    v7 = Call v1, -, v6, v1
    ReturnCall v7 casts=[{declared={type=undefined|null|number site=w0}}]
}
function f2 name="text" flags=[IsChecked] parameters=[{type=anything}, {type=anything}] results=[{type=undefined|null|string site=w1}] {
    v2 = Integer
    v3 = StrictEq v0, v2
    Branch v3 -> taken b1(), notTaken b2()
  b1():
    v4 = String string="text"
    Return v4
  b2():
    v5 = Integer integer=1
    v6 = Sub v0, v5
    v7 = Call v1, -, v6, v1
    ReturnCall v7 casts=[{declared={type=undefined|null|string site=w1}}]
}
)IR"_s;

static constexpr ASCIILiteral ordinaryCallUnderWaitingFrame = R"IR(module m0 path="main.ts" displayPath="main.ts" body=f0
entry module=m0
typesite w0 module=m0 position=10 kind=Result
typesite w1 module=m0 position=20 kind=Result
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = PropGet v0 key="id" [isFreeName]
    v3 = NewClosure function=f1
    v4 = Call v2, -, v3
    v5 = NewClosure function=f2
    v6 = Call v2, -, v5
    v7 = NewClosure function=f3
    v8 = Call v2, -, v7
    Jump -> next b1()
  b1() handler=b2():
    v9 = Call v8, -, v4, v6
    v10 = Call v1, -, v9
    Jump -> next b3()
  b2() role=CatchHandler:
    v11 = CatchValue
    v12 = PropGet v11 key="name"
    v13 = Call v1, -, v12
    Jump -> next b3()
  b3():
    Return
}
function f1 name="down" flags=[IsChecked] parameters=[{type=anything}, {type=anything}] results=[{type=undefined|null|number site=w0}] {
    v2 = Integer
    v3 = StrictEq v0, v2
    Branch v3 -> taken b1(), notTaken b2()
  b1():
    v4 = Double number=0.5
    Return v4
  b2():
    v5 = Integer integer=1
    v6 = Sub v0, v5
    v7 = Call v1, -, v6, v1
    ReturnCall v7 casts=[{declared={type=undefined|null|number site=w0}}]
}
function f2 name="wrong" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = String string="text"
    Return v2
}
function f3 name="outer" flags=[IsChecked] parameters=[{type=anything}, {type=anything}] results=[{type=undefined|null|number site=w1}] {
    v2 = Integer integer=1
    v3 = Call v0, -, v2, v1
    v4 = String string="text"
    v5 = StrictEq v3, v4
    Branch v5 -> taken b1(), notTaken b2()
  b1():
    v6 = Call v0, -, v2, v0
    ReturnCall v6 casts=[{declared={type=undefined|null|number site=w1}}]
  b2():
    Return v2
}
)IR"_s;

static void checkUnderEveryOption(ASCIILiteral programText, ASCIILiteral expectedOutput)
{
    static constexpr ASCIILiteral words[] = {
        "validate"_s,
        "heat=EverythingIsHot"_s,
        "heat=EverythingIsCold"_s,
        "disable=flow:CalleeSets"_s,
        "disable=flow:NarrowedTypes"_s,
        "disable=pass:ResolveCalls"_s,
        "disable=pass:InlineCalls"_s,
        "disable=pass:ChooseCallingConventions"_s,
    };
    checkOutputOfProgramInShell(programText, expectedOutput, { });
    for (ASCIILiteral word : words)
        checkOutputOfProgramInShell(programText, expectedOutput, { word });
}

static void testTypedFunctionsCalledByValue()
{
    checkUnderEveryOption(typedFunctionsCalledByValue, "0.5\n0.5\ntext\ntext\n"_s);
}

static void testOrdinaryCallUnderWaitingFrame()
{
    checkUnderEveryOption(ordinaryCallUnderWaitingFrame, "0.5\n"_s);
}

std::span<const NativeTest> tailCallProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "tail-call-programs/typed-functions-called-by-value"_s, testTypedFunctionsCalledByValue },
        { "tail-call-programs/ordinary-call-under-a-frame-that-waits"_s, testOrdinaryCallUnderWaitingFrame },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
