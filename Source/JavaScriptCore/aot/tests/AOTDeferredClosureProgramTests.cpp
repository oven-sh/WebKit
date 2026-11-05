/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral closuresCapturingNothing = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Record" fields=[{key="a" declared={type=anything} flags=[IsRequired]}, {key="m" declared={type=function} flags=[IsRequired]}]
variable g0 name="kept" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = CallDirect function=f2
    v3 = Call v1, -, v2
    v4 = CallDirect function=f3
    v5 = Call v1, -, v4
    v6 = CallDirect function=f4
    v7 = Call v1, -, v6
    v8 = CallDirect function=f6
    v9 = Call v1, -, v8
    v10 = CallDirect function=f7
    v11 = Call v1, -, v10
    v12 = CallDirect function=f8
    v13 = Call v1, -, v12
    v14 = PropGet v0 key="flag" [isFreeName]
    v15 = CallDirect v14 function=f10
    v16 = Call v1, -, v15
    Return
}
function f1 name="m" kind=Arrow results=[{type=anything}] {
    v0 = Integer integer=1
    Return v0
}
function f2 name="readsTwice" results=[{type=anything}] {
    v0 = Integer integer=1
    v1 = NewClosure function=f1
    v2 = NewObject -, v0, v1 layout=L0 fields=[0, 1]
    v3 = FinishObject v2
    v4 = PropGet v3 key="m"
    v5 = PropGet v3 key="m"
    v6 = StrictEq v4, v5
    Return v6
}
function f3 name="readsTwoObjects" results=[{type=anything}] {
    v0 = Integer integer=1
    v1 = NewClosure function=f1
    v2 = NewObject -, v0, v1 layout=L0 fields=[0, 1]
    v3 = FinishObject v2
    v4 = NewClosure function=f1
    v5 = NewObject -, v0, v4 layout=L0 fields=[0, 1]
    v6 = FinishObject v5
    v7 = PropGet v3 key="m"
    v8 = PropGet v6 key="m"
    v9 = StrictEq v7, v8
    Return v9
}
function f4 name="readsInCallee" results=[{type=anything}] {
    v0 = Integer integer=1
    v1 = NewClosure function=f1
    v2 = NewObject -, v0, v1 layout=L0 fields=[0, 1]
    v3 = FinishObject v2
    v4 = NewClosure function=f5
    v5 = Call v4, -, v3 [isNeverInlined]
    v6 = PropGet v3 key="m"
    v7 = StrictEq v5, v6
    Return v7
}
function f5 name="reads" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="m"
    Return v1
}
function f6 name="collectsBetweenReads" results=[{type=anything}] {
    v0 = Integer integer=1
    v1 = NewClosure function=f1
    v2 = NewObject -, v0, v1 layout=L0 fields=[0, 1]
    v3 = FinishObject v2
    v4 = PropGet v3 key="m"
    v5 = Integer integer=5
    PropSet v4, v5 key="tag"
    v7 = Builtin builtin=GlobalThis
    v8 = PropGet v7 key="collect" [isFreeName]
    v9 = Call v8
    v10 = PropGet v3 key="m"
    v11 = PropGet v10 key="tag"
    Return v11
}
function f7 name="escapesAfterRead" results=[{type=anything}] {
    v0 = Integer integer=1
    v1 = NewClosure function=f1
    v2 = NewObject -, v0, v1 layout=L0 fields=[0, 1]
    v3 = FinishObject v2
    v4 = PropGet v3 key="m"
    PutModuleVariable v3 variable=g0
    v6 = NewClosure function=f9
    v7 = Call v6 [isNeverInlined]
    v8 = StrictEq v4, v7
    Return v8
}
function f8 name="calls" results=[{type=anything}] {
    v0 = Integer integer=1
    v1 = NewClosure function=f1
    v2 = NewObject -, v0, v1 layout=L0 fields=[0, 1]
    v3 = FinishObject v2
    v4 = PropGet v3 key="m"
    v5 = Call v4, v3 calleeKey="m"
    Return v5
}
function f9 name="readsKept" results=[{type=anything}] {
    v0 = GetModuleVariable variable=g0
    v1 = PropGet v0 key="m"
    Return v1
}
function f10 name="readsJoined" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Integer integer=1
    v2 = NewClosure function=f1
    v3 = NewObject -, v1, v2 layout=L0 fields=[0, 1]
    v4 = FinishObject v3
    v5 = NewClosure function=f1
    v6 = NewObject -, v1, v5 layout=L0 fields=[0, 1]
    v7 = FinishObject v6
    v8 = ToBoolean v0
    Branch v8 -> taken b1(), notTaken b2()
  b1():
    Jump -> next b3(v4)
  b2():
    Jump -> next b3(v7)
  b3(v9):
    v10 = PropGet v9 key="m"
    v11 = PropGet v4 key="m"
    v12 = StrictEq v10, v11
    v13 = PropGet v9 key="m"
    v14 = PropGet v7 key="m"
    v15 = StrictEq v13, v14
    v16 = Add v12, v15
    Return v16
}
)IR"_s;

static constexpr ASCIILiteral answersOfClosuresCapturingNothing = "true\nfalse\ntrue\n5\ntrue\n1\n1\n"_s;

static constexpr ASCIILiteral closuresCapturingTheirObject = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Counter" constructor=f1 fields=[{key="x" declared={type=anything} flags=[IsRequired]}, {key="run" declared={type=function} flags=[IsRequired]}]
variable g0 name="kept" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = NewClass v0 layout=L0
    v2 = FinishClass v1 layout=L0
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="print" [isFreeName]
    v5 = CallDirect v2 function=f3
    v6 = Call v4, -, v5
    v7 = CallDirect v2 function=f4
    v8 = Call v4, -, v7
    v9 = CallDirect v2 function=f5
    v10 = Call v4, -, v9
    v11 = CallDirect v2 function=f7
    v12 = Call v4, -, v11
    v13 = CallDirect v2 function=f8
    v14 = Call v4, -, v13
    Return
}
function f1 name="Counter" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Integer integer=7
    PropDefine v2, v3 key="x"
    v5 = NewClosure v2 function=f2
    PropDefine v2, v5 key="run"
    Return v2
}
function f2 name="run" kind=Arrow captures=[{type=L0}] results=[{type=anything}] {
    v1 = PropGet v0 key="x"
    Return v1
}
function f3 name="readsTwice" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Construct v0
    v2 = PropGet v1 key="run"
    v3 = PropGet v1 key="run"
    v4 = StrictEq v2, v3
    Return v4
}
function f4 name="readsTwoObjects" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Construct v0
    v2 = Construct v0
    v3 = PropGet v1 key="run"
    v4 = PropGet v2 key="run"
    v5 = StrictEq v3, v4
    Return v5
}
function f5 name="readsInCallee" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Construct v0
    v2 = NewClosure function=f6
    v3 = Call v2, -, v1 [isNeverInlined]
    v4 = PropGet v1 key="run"
    v5 = StrictEq v3, v4
    Return v5
}
function f6 name="reads" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="run"
    Return v1
}
function f7 name="callsWhatItRead" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Construct v0
    v2 = PropGet v1 key="run"
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="collect" [isFreeName]
    v5 = Call v4
    v6 = Integer integer=9
    PropSet v1, v6 key="x"
    v8 = Call v2
    Return v8
}
function f8 name="escapesAfterRead" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Construct v0
    v2 = PropGet v1 key="run"
    PutModuleVariable v1 variable=g0
    v4 = NewClosure function=f9
    v5 = Call v4 [isNeverInlined]
    v6 = StrictEq v2, v5
    Return v6
}
function f9 name="readsKept" results=[{type=anything}] {
    v0 = GetModuleVariable variable=g0
    v1 = PropGet v0 key="run"
    Return v1
}
)IR"_s;

static constexpr ASCIILiteral answersOfClosuresCapturingTheirObject = "true\nfalse\ntrue\n9\ntrue\n"_s;

static constexpr ASCIILiteral closuresOfSeveralKindsInOneSlot = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Handler" fields=[{key="run" declared={type=function} flags=[IsRequired]}]
layout L1 kind=Instance name="Counter" constructor=f4 fields=[{key="x" declared={type=anything} flags=[IsRequired]}, {key="run" declared={type=function} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f4
    v1 = NewClass v0 layout=L1
    v2 = FinishClass v1 layout=L1
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="print" [isFreeName]
    v5 = NewClosure function=f3
    v6 = PropGet v3 key="flag" [isFreeName]
    v7 = Call v5, -, v6 [isNeverInlined]
    v8 = Call v4, -, v7
    v9 = PropGet v3 key="missing"
    v10 = Call v5, -, v9 [isNeverInlined]
    v11 = Call v4, -, v10
    v12 = CallDirect v2 function=f6
    v13 = Call v4, -, v12
    Return
}
function f1 name="first" kind=Arrow results=[{type=anything}] {
    v0 = Integer integer=1
    Return v0
}
function f2 name="second" kind=Arrow results=[{type=anything}] {
    v0 = Integer integer=2
    Return v0
}
function f3 name="callsEither" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = ToBoolean v0
    Branch v1 -> taken b1(), notTaken b2()
  b1():
    v2 = NewClosure function=f1
    v3 = NewObject -, v2 layout=L0 fields=[0]
    v4 = FinishObject v3
    Jump -> next b3(v4)
  b2():
    v5 = NewClosure function=f2
    v6 = NewObject -, v5 layout=L0 fields=[0]
    v7 = FinishObject v6
    Jump -> next b3(v7)
  b3(v8):
    v9 = PropGet v8 key="run"
    v10 = Call v9, v8 calleeKey="run"
    Return v10
}
function f4 name="Counter" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
    v1 = GetConstructPrototype v0 layout=L1
    v2 = NewObject v1 layout=L1
    v3 = Integer integer=7
    PropDefine v2, v3 key="x"
    v5 = NewClosure v2 function=f5
    PropDefine v2, v5 key="run"
    Return v2
}
function f5 name="run" kind=Arrow captures=[{type=L1}] results=[{type=anything}] {
    v1 = PropGet v0 key="x"
    Return v1
}
function f6 name="callsClosureOverOtherObject" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Construct v0
    v2 = Construct v0
    v3 = Integer integer=5
    PropSet v2, v3 key="x"
    v5 = PropGet v2 key="run"
    PropSet v1, v5 key="run"
    v7 = PropGet v1 key="run"
    v8 = Call v7, v1 calleeKey="run"
    Return v8
}
)IR"_s;

static constexpr ASCIILiteral answersOfClosuresOfSeveralKindsInOneSlot = "1\n2\n5\n"_s;

static constexpr ASCIILiteral narrowReferencesInFrames = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Point" fields=[{key="y" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Holder" fields=[{key="held" declared={type=anything} flags=[IsRequired]}, {key="count" declared={type=anything} flags=[IsRequired]}]
variable g0 name="kept" module=m0 declared={type=anything} initialization=Undefined
variable g1 name="alsoKept" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = CallDirect function=f1
    v3 = Call v1, -, v2
    v4 = CallDirect function=f2
    v5 = Call v1, -, v4
    v6 = CallDirect function=f4
    v7 = Call v1, -, v6
    Return
}
function f1 name="readsHeld" results=[{type=anything}] {
    v0 = Integer integer=7
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = FinishObject v1
    PutModuleVariable v2 variable=g0
    v4 = Integer integer=1
    v5 = NewObject -, v2, v4 layout=L1 fields=[0, 1]
    v6 = FinishObject v5
    v7 = PropGet v6 key="held"
    v8 = PropGet v7 key="y"
    Return v8
}
function f2 name="readsHeldInCallee" results=[{type=anything}] {
    v0 = Integer integer=8
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = FinishObject v1
    PutModuleVariable v2 variable=g0
    v4 = Integer integer=1
    v5 = NewObject -, v2, v4 layout=L1 fields=[0, 1]
    v6 = FinishObject v5
    v7 = NewClosure function=f3
    v8 = Call v7, -, v6 [isNeverInlined]
    Return v8
}
function f3 name="reads" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="held"
    v2 = PropGet v1 key="y"
    Return v2
}
function f4 name="storesHeld" results=[{type=anything}] {
    v0 = Integer integer=7
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = FinishObject v1
    PutModuleVariable v2 variable=g0
    v4 = Integer integer=9
    v5 = NewObject -, v4 layout=L0 fields=[0]
    v6 = FinishObject v5
    PutModuleVariable v6 variable=g1
    v8 = Integer integer=1
    v9 = NewObject -, v2, v8 layout=L1 fields=[0, 1]
    v10 = FinishObject v9
    PropSet v10, v6 key="held"
    v12 = Builtin builtin=GlobalThis
    v13 = PropGet v12 key="collect" [isFreeName]
    v14 = Call v13
    v15 = PropGet v10 key="held"
    v16 = PropGet v15 key="y"
    Return v16
}
)IR"_s;

static constexpr ASCIILiteral answersOfNarrowReferencesInFrames = "7\n8\n9\n"_s;

static constexpr ASCIILiteral defineCollectAndFlag = "globalThis.collect = () => fullGC(); globalThis.flag = true;"_s;

static void checkDeferredClosureProgram(ASCIILiteral program, ASCIILiteral answers, OptionWords shellOptions)
{
    checkOutputOfProgramInShell(program, answers, { "references=narrow"_s }, defineCollectAndFlag, shellOptions);
    checkOutputOfProgramInShell(program, answers, { "references=wide"_s }, defineCollectAndFlag, shellOptions);
}

static void testClosuresCapturingNothing()
{
    checkDeferredClosureProgram(closuresCapturingNothing, answersOfClosuresCapturingNothing, { });
}

static void testClosuresCapturingNothingUnderContinuousCollection()
{
    checkDeferredClosureProgram(closuresCapturingNothing, answersOfClosuresCapturingNothing, { "--collectContinuously=1"_s });
}

static void testClosuresCapturingTheirObject()
{
    checkDeferredClosureProgram(closuresCapturingTheirObject, answersOfClosuresCapturingTheirObject, { });
}

static void testClosuresCapturingTheirObjectUnderContinuousCollection()
{
    checkDeferredClosureProgram(closuresCapturingTheirObject, answersOfClosuresCapturingTheirObject, { "--collectContinuously=1"_s });
}

static void testClosuresOfSeveralKindsInOneSlot()
{
    checkDeferredClosureProgram(closuresOfSeveralKindsInOneSlot, answersOfClosuresOfSeveralKindsInOneSlot, { });
}

static void testNarrowReferencesInFrames()
{
    checkDeferredClosureProgram(narrowReferencesInFrames, answersOfNarrowReferencesInFrames, { });
    checkOutputOfProgramInShell(narrowReferencesInFrames, answersOfNarrowReferencesInFrames, { "references=narrow"_s }, defineCollectAndFlag, { "--collectContinuously=1"_s });
}

std::span<const NativeTest> deferredClosureProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "deferred-closure-programs/capturing-nothing"_s, testClosuresCapturingNothing },
        { "deferred-closure-programs/capturing-nothing-under-continuous-collection"_s, testClosuresCapturingNothingUnderContinuousCollection },
        { "deferred-closure-programs/capturing-their-object"_s, testClosuresCapturingTheirObject },
        { "deferred-closure-programs/capturing-their-object-under-continuous-collection"_s, testClosuresCapturingTheirObjectUnderContinuousCollection },
        { "deferred-closure-programs/several-kinds-in-one-slot"_s, testClosuresOfSeveralKindsInOneSlot },
        { "deferred-closure-programs/narrow-references-in-frames"_s, testNarrowReferencesInFrames },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
