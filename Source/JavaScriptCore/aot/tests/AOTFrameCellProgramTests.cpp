/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

// Each case makes an object or a closure that never leaves its function and gives it to a function that stays out of line. The callee collects while it
// holds the cell in an array, captures it, lists its keys, reads it under a computed key, reads what it holds, or captures it in a closure that is called.
static constexpr ASCIILiteral cellsGivenToCallees = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Point" fields=[{key="y" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Holder" fields=[{key="held" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = NewClosure function=f2
    v1 = CallDirect v0 function=f1
    v2 = NewClosure function=f3
    v3 = CallDirect v2 function=f1
    v4 = NewClosure function=f4
    v5 = CallDirect v4 function=f1
    v6 = NewClosure function=f5
    v7 = CallDirect v6 function=f1
    v8 = NewClosure function=f6
    v9 = CallDirect v8 function=f1
    v10 = NewClosure function=f7
    v11 = CallDirect v10 function=f1
    Return
}
function f1 name="repeat" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer
    Jump -> next b1(v1, v1)
  b1(v2, v3):
    LoopHint
    v5 = Integer integer=20
    v6 = Less v2, v5
    Branch v6 -> taken b2(), notTaken b3()
  b2():
    v7 = Call v0, -, v2 [isNeverInlined]
    v8 = Add v3, v7
    v9 = Increment v2
    Jump -> next b1(v9, v8)
  b3():
    v10 = Builtin builtin=GlobalThis
    v11 = PropGet v10 key="print" [isFreeName]
    v12 = Call v11, -, v3
    Return
}
function f2 name="givesToHolder" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = NewClosure function=f8
    v3 = Call v2, -, v1 [isNeverInlined]
    Return v3
}
function f3 name="givesToCapturer" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = NewClosure function=f9
    v3 = Call v2, -, v1 [isNeverInlined]
    Return v3
}
function f4 name="givesToLister" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = NewClosure function=f10
    v3 = Call v2, -, v1 [isNeverInlined]
    Return v3
}
function f5 name="givesToReaderByKey" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = NewClosure function=f11
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="key" [isFreeName]
    v5 = Call v2, -, v1, v4 [isNeverInlined]
    Return v5
}
function f6 name="givesHolderOfYoungObject" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = NewObject -, v1 layout=L1 fields=[0]
    v3 = NewClosure function=f12
    v4 = Call v3, -, v2 [isNeverInlined]
    Return v4
}
function f7 name="givesClosure" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = NewClosure v0 function=f15
    v2 = NewClosure function=f13
    v3 = Call v2, -, v1 [isNeverInlined]
    Return v3
}
function f8 name="holdsInArray" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = NewArray v0 contents=anything
    v2 = Builtin builtin=GlobalThis
    v3 = PropGet v2 key="collect" [isFreeName]
    v4 = Call v3
    v5 = Integer
    v6 = ElemGet v1, v5
    v7 = PropGet v6 key="y"
    Return v7
}
function f9 name="capturesInClosure" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = NewClosure v0 function=f17
    v2 = NewClosure function=f14
    v3 = Call v2, -, v1 [isNeverInlined]
    Return v3
}
function f10 name="listsKeys" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="collect" [isFreeName]
    v3 = Call v2
    v4 = PropGet v1 key="Object" [isFreeName]
    v5 = PropGet v4 key="keys"
    v6 = Call v5, v4, v0
    v7 = PropGet v6 key="length"
    Return v7
}
function f11 name="readsByKey" parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Builtin builtin=GlobalThis
    v3 = PropGet v2 key="collect" [isFreeName]
    v4 = Call v3
    v5 = ElemGet v0, v1
    Return v5
}
function f12 name="readsHeld" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="collect" [isFreeName]
    v3 = Call v2
    v4 = PropGet v0 key="held"
    v5 = PropGet v4 key="y"
    Return v5
}
function f13 name="capturesClosure" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = NewClosure v0 function=f16
    v2 = NewClosure function=f14
    v3 = Call v2, -, v1 [isNeverInlined]
    Return v3
}
function f14 name="callsDuringCollection" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="collect" [isFreeName]
    v3 = Call v2
    v4 = Call v0 [isNeverInlined]
    Return v4
}
function f15 name="returnsCapture" kind=Arrow captures=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Return v0
}
function f16 name="callsCapture" kind=Arrow captures=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Call v0 [isNeverInlined]
    Return v1
}
function f17 name="readsCapture" kind=Arrow captures=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="y"
    Return v1
}
)IR"_s;

static constexpr ASCIILiteral defineCollectAndKey = "globalThis.collect = () => fullGC(); globalThis.key = \"y\";"_s;
static constexpr ASCIILiteral sumsOfCellsGivenToCallees = "190\n190\n20\n190\n190\n190\n"_s;

static void testCellsGivenToCallees()
{
    checkOutputOfProgramInShell(cellsGivenToCallees, sumsOfCellsGivenToCallees, { }, defineCollectAndKey);
    checkOutputOfProgramInShell(cellsGivenToCallees, sumsOfCellsGivenToCallees, { "check=VerifyEachPass"_s }, defineCollectAndKey);
    checkOutputOfProgramInShell(cellsGivenToCallees, sumsOfCellsGivenToCallees, { "validate"_s }, defineCollectAndKey);
    checkOutputOfProgramInShell(cellsGivenToCallees, sumsOfCellsGivenToCallees, { "disable=pass:AllocateInFrames"_s }, defineCollectAndKey);
}

static void testCellsGivenToCalleesUnderContinuousCollection()
{
    checkOutputOfProgramInShell(cellsGivenToCallees, sumsOfCellsGivenToCallees, { }, defineCollectAndKey, { "--collectContinuously=1"_s });
    checkOutputOfProgramInShell(cellsGivenToCallees, sumsOfCellsGivenToCallees, { "validate"_s }, defineCollectAndKey, { "--collectContinuously=1"_s });
    checkOutputOfProgramInShell(cellsGivenToCallees, sumsOfCellsGivenToCallees, { "disable=pass:InlineCalls"_s }, defineCollectAndKey, { "--collectContinuously=1"_s });
}

// function countTrue(item, test) { if (item === undefined) return 0; return (test(item) ? 1 : 0) + countTrue(item.next, test) }
// print(countTrue(items, item => item.id > 2) + countTrue(items, () => true) * 100)
// The closures lie in the frame of the module body, and countTrue hands `test` on to itself.
static constexpr ASCIILiteral closureHandedToPartCutOff = R"IR(layout L0 name="Item" fields=[{key="id" declared={type=anything} flags=[IsRequired]}, {key="next" declared={type=anything} flags=[IsRequired]}]
variable g0 name="countTrue" declared={type=function} initialization=Function function=f1 flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    v0 = Undefined
    v1 = Integer integer=5
    v2 = NewObject -, v1, v0 layout=L0 fields=[0, 1]
    v3 = FinishObject v2
    v4 = Integer integer=1
    v5 = NewObject -, v4, v3 layout=L0 fields=[0, 1]
    v6 = FinishObject v5
    v7 = Builtin builtin=GlobalThis
    v8 = PropGet v7 key="print" [isFreeName]
    v9 = GetModuleVariable variable=g0
    v10 = NewClosure function=f2
    v11 = Call v9, -, v6, v10 calleeName="countTrue"
    v12 = NewClosure function=f3
    v13 = Call v9, -, v6, v12 calleeName="countTrue"
    v14 = Integer integer=100
    v15 = Mul v13, v14
    v16 = Add v11, v15
    v17 = Call v8, -, v16 calleeName="print"
    v18 = Undefined
    Return v18
}
function f1 name="countTrue" length=2 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = TypeTest v0 testedType=undefined
    Branch v2 -> taken b1(), notTaken b2()
  b1():
    v3 = Integer
    Return v3
  b2():
    v4 = Call v1, -, v0 calleeName="test"
    v5 = ToBoolean v4
    Branch v5 -> taken b3(), notTaken b4()
  b3():
    v6 = Integer integer=1
    Jump -> next b5(v6)
  b4():
    v7 = Integer
    Jump -> next b5(v7)
  b5(v8):
    v9 = GetModuleVariable variable=g0
    v10 = PropGet v0 key="next"
    v11 = Call v9, -, v10, v1 calleeName="countTrue"
    v12 = Add v8, v11
    Return v12
}
function f2 kind=Arrow length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="id"
    v2 = Integer integer=2
    v3 = Greater v1, v2
    Return v3
}
function f3 kind=Arrow thisType={type=anything} results=[{type=anything}] {
  b0():
    v0 = Boolean boolean=true
    Return v0
}
entry module=m0
)IR"_s;

static void testClosureHandedToPartCutOff()
{
    checkOutputOfProgramInShell(closureHandedToPartCutOff, "201\n"_s, { });
    for (ASCIILiteral option : { "validate"_s, "check=VerifyEachPass"_s, "disable=pass:AllocateInFrames"_s })
        checkOutputOfProgramInShell(closureHandedToPartCutOff, "201\n"_s, { option });
}

// function attempt(compute) { try { print(compute()) } catch (error) { print("threw") } }  const record = { name: "n", items: 3 }
// await { then(resolve) { resolve(1) } }
// attempt(() => "first " + record.name); attempt(() => "second " + record.items); attempt(() => "third " + Object.keys(record).join())
// await (async () => { await null })()
// print("after the second await " + record.items)
// The three closures lie in the frame of the module body, which is saved at the second await and comes back at another address.
static constexpr ASCIILiteral closuresBetweenTwoAwaits = R"IR(layout L0 fields=[{key="name" declared={type=string flags=[IsExpected]} flags=[IsRequired]}, {key="items" declared={type=number flags=[IsExpected]} flags=[IsRequired]}]
layout L1 fields=[{key="then" declared={type=function flags=[IsExpected]} flags=[IsRequired, IsMethod]}]
variable g0 name="record" declared={type=L0|absent flags=[IsExpected]} flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" flags=[HasTopLevelAwait] body=f0
function f0 kind=Module flags=[IsAsync] thisType={type=anything} results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = Convert v0 declared={type=function flags=[IsExpected]}
    v2 = Data data=d0
    v3 = Convert v2 declared={type=L0 flags=[IsExpected]}
    PutModuleVariable v3 variable=g0
    v5 = NewObject layout=L1
    v6 = NewClosure function=f2
    v7 = Convert v6 declared={type=function flags=[IsExpected]}
    PropDefine v5, v7 key="then"
    v9 = FinishObject v5
    v10 = Await v9
    v11 = Convert v1 declared={type=function flags=[IsExpected]}
    v12 = NewClosure function=f3
    v13 = Call v11, -, v12 calleeName="attempt"
    v14 = NewClosure function=f4
    v15 = Call v11, -, v14 calleeName="attempt"
    v16 = NewClosure function=f5
    v17 = Call v11, -, v16 calleeName="attempt"
    v18 = NewClosure function=f6
    v19 = Call v18
    v20 = Convert v19 declared={type=Promise flags=[IsExpected]}
    v21 = Await v20
    v22 = Builtin builtin=GlobalThis
    v23 = PropGet v22 key="print" [isFreeName]
    v24 = String string="after the second await "
    v25 = PropGet v3 key="items"
    v26 = Convert v25 declared={type=number flags=[IsExpected]}
    v27 = Add v24, v26
    v28 = Call v23, -, v27 calleeName="print"
    v29 = Undefined
    Return v29
}
function f1 name="attempt" length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    Jump -> next b2()
  b2() handler=b4():
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = Call v0 calleeName="compute"
    v4 = Call v2, -, v3 calleeName="print"
    Jump -> next b3()
  b3():
    Jump -> next b5()
  b4() role=CatchHandler:
    v5 = CatchValue
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="print" [isFreeName]
    v8 = String string="threw"
    v9 = Call v7, -, v8 calleeName="print"
    Jump -> next b5()
  b5():
    v10 = Undefined
    Return v10
}
function f2 name="then" kind=Method length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = Integer integer=1
    v2 = Call v0, -, v1 calleeName="resolve"
    v3 = Undefined
    Return v3
}
function f3 kind=Arrow thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = String string="first "
    v1 = GetModuleVariable variable=g0
    v2 = CheckInitialized v1 name="record"
    v3 = PropGet v2 key="name"
    v4 = Convert v3 declared={type=string flags=[IsExpected]}
    v5 = Add v0, v4
    v6 = Convert v5 declared={type=string flags=[IsExpected]}
    Return v6
}
function f4 kind=Arrow thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = String string="second "
    v1 = GetModuleVariable variable=g0
    v2 = CheckInitialized v1 name="record"
    v3 = PropGet v2 key="items"
    v4 = Convert v3 declared={type=number flags=[IsExpected]}
    v5 = Add v0, v4
    v6 = Convert v5 declared={type=string flags=[IsExpected]}
    Return v6
}
function f5 kind=Arrow thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = String string="third "
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="Object" [isFreeName]
    v3 = Convert v2 declared={type=builtin(Object) flags=[IsExpected]}
    v4 = PropGet v3 key="keys"
    v5 = Convert v4 declared={type=function flags=[IsExpected]}
    v6 = Convert v5 declared={type=builtin(ObjectKeys) flags=[IsExpected]}
    v7 = GetModuleVariable variable=g0
    v8 = CheckInitialized v7 name="record"
    v9 = Call v6, v3, v8 calleeKey="keys" calleeName="keys"
    v10 = Convert v9 declared={type=Array<string> flags=[IsExpected]}
    v11 = PropGet v10 key="join"
    v12 = Convert v11 declared={type=function flags=[IsExpected]}
    v13 = Call v12, v10 calleeKey="join" calleeName="join"
    v14 = Convert v13 declared={type=string flags=[IsExpected]}
    v15 = Add v0, v14
    v16 = Convert v15 declared={type=string flags=[IsExpected]}
    Return v16
}
function f6 kind=Arrow flags=[IsAsync] thisType={type=anything} results=[{type=Promise flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = Null
    v1 = Await v0
    v2 = Undefined
    Return v2
}
data d0 root=Object{layout=L0 members=[{key="name" value=String{value="n"}}, {key="items" value=Number{value=3}}]}
entry module=m0
)IR"_s;

static void testClosuresBetweenTwoAwaits()
{
    constexpr ASCIILiteral expected = "first n\nsecond 3\nthird name,items\nafter the second await 3\n"_s;
    checkOutputOfProgramInShell(closuresBetweenTwoAwaits, expected, { });
    for (ASCIILiteral option : { "validate"_s, "check=VerifyEachPass"_s, "disable=pass:AllocateInFrames"_s })
        checkOutputOfProgramInShell(closuresBetweenTwoAwaits, expected, { option });
}

std::span<const NativeTest> frameCellProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "frame-cell-programs/cells-given-to-callees"_s, testCellsGivenToCallees },
        { "frame-cell-programs/cells-given-to-callees-under-continuous-collection"_s, testCellsGivenToCalleesUnderContinuousCollection },
        { "frame-cell-programs/closure-handed-to-part-cut-off"_s, testClosureHandedToPartCutOff },
        { "frame-cell-programs/closures-between-two-awaits"_s, testClosuresBetweenTwoAwaits },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
