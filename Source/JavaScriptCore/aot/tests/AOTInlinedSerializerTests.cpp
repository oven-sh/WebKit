/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral everyObjectHasAppendedFields = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Named" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = Integer integer=1
    v3 = CallDirect v2 function=f2
    v4 = CallDirect v2 function=f2
    v5 = CallDirect v2 function=f2
    PropSet v3, v2 key="b" mode=Strict
    PropSet v3, v2 key="c" mode=Strict
    PropSet v4, v2 key="c" mode=Strict
    PropSet v4, v2 key="b" mode=Strict
    PropSet v5, v2 key="c" mode=Strict
    v11 = CallDirect v3 function=f1
    v12 = Call v1, -, v11
    v13 = CallDirect v4 function=f1
    v14 = Call v1, -, v13
    v15 = CallDirect v5 function=f1
    v16 = Call v1, -, v15
    Return
}
function f1 name="describe" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="JSON" [isFreeName]
    v3 = PropGet v2 key="stringify"
    v4 = Call v3, v2, v0 calleeKey="stringify"
    v5 = PropGet v0 key="b"
    v6 = PropHas v0 key="b"
    v7 = String string=" "
    v8 = Add v4, v7
    v9 = Add v8, v5
    v10 = Add v9, v7
    v11 = Add v10, v6
    Return v11
}
function f2 name="make" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = NewObject -, v0 layout=L0 fields=[0]
    Return v1
}
)IR"_s;

static constexpr ASCIILiteral outputWithAppendedFields = R"({"a":1,"b":1,"c":1} 1 true
{"a":1,"c":1,"b":1} 1 true
{"a":1,"c":1} undefined false
)"_s;

// The same without the two stores to `second`: the copy of the serializer writes its text.
static constexpr ASCIILiteral oneObjectIsAsCreated = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Named" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = Integer integer=1
    v3 = CallDirect v2 function=f2
    v4 = CallDirect v2 function=f2
    v5 = CallDirect v2 function=f2
    PropSet v3, v2 key="b" mode=Strict
    PropSet v3, v2 key="c" mode=Strict
    PropSet v5, v2 key="c" mode=Strict
    v9 = CallDirect v3 function=f1
    v10 = Call v1, -, v9
    v11 = CallDirect v4 function=f1
    v12 = Call v1, -, v11
    v13 = CallDirect v5 function=f1
    v14 = Call v1, -, v13
    Return
}
function f1 name="describe" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="JSON" [isFreeName]
    v3 = PropGet v2 key="stringify"
    v4 = Call v3, v2, v0 calleeKey="stringify"
    v5 = PropGet v0 key="b"
    v6 = PropHas v0 key="b"
    v7 = String string=" "
    v8 = Add v4, v7
    v9 = Add v8, v5
    v10 = Add v9, v7
    v11 = Add v10, v6
    Return v11
}
function f2 name="make" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = NewObject -, v0 layout=L0 fields=[0]
    Return v1
}
)IR"_s;

static constexpr ASCIILiteral outputWithObjectAsCreated = R"({"a":1,"b":1,"c":1} 1 true
{"a":1} undefined false
{"a":1,"c":1} undefined false
)"_s;

static void checkBothPrograms(OptionWords optionWords)
{
    checkOutputOfProgramInShell(everyObjectHasAppendedFields, outputWithAppendedFields, optionWords);
    checkOutputOfProgramInShell(oneObjectIsAsCreated, outputWithObjectAsCreated, optionWords);
}

static void testEveryCallInlined()
{
    checkBothPrograms({ "heat=EverythingIsHot"_s });
    checkBothPrograms({ "heat=EverythingIsHot"_s, "validate"_s });
    checkBothPrograms({ "heat=EverythingIsHot"_s, "check=VerifyEachPass"_s });
    checkBothPrograms({ "heat=EverythingIsHot"_s, "references=narrow"_s });
}

static void testOtherSchedules()
{
    checkBothPrograms({ });
    checkBothPrograms({ "heat=EverythingIsCold"_s });
    checkBothPrograms({ "heat=EverythingIsHot"_s, "disable=pass:InlineCalls"_s });
    checkBothPrograms({ "heat=EverythingIsHot"_s, "disable=pass:SinkAllocations"_s });
    checkBothPrograms({ "heat=EverythingIsHot"_s, "disable=pass:ThreadJumps"_s });
}

std::span<const NativeTest> inlinedSerializerTests()
{
    static constexpr NativeTest tests[] = {
        { "inlined-serializer/every-call-inlined"_s, testEveryCallInlined },
        { "inlined-serializer/other-schedules"_s, testOtherSchedules },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
