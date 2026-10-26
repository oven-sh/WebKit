/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral recordsWithArrayMembers = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Row" fields=[{key="id" declared={type=number}}]
layout L1 name="Page" fields=[{key="items" declared={type=Array<L0>}}, {key="total" declared={type=number}}]
layout L2 name="Changed" fields=[{key="list" declared={type=Array<number>}}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = String string="{\"items\":[{\"id\":7},{\"id\":8}],\"total\":2}"
    v1 = CallDirect v0 function=f2
    v2 = String string="{\"items\":[],\"total\":0}"
    v3 = CallDirect v2 function=f2
    v4 = String string="{\"items\":[1,{\"id\":8},\"x\",null,[2],{\"other\":3}],\"total\":6}"
    v5 = CallDirect v4 function=f2
    v6 = String string="{ \"total\" : 1 , \"items\" : [ { \"id\" : 9 , \"more\" : [ 1 ] } ] , \"extra\" : [ { \"id\" : 1 } ] }"
    v7 = CallDirect v6 function=f2
    v8 = String string="{\"items\":{\"0\":{\"id\":5},\"length\":1},\"total\":1}"
    v9 = CallDirect v8 function=f2
    v10 = String string="{\"total\":1}"
    v11 = CallDirect v10 function=f2
    v12 = String string="{\"list\":[1,2]}"
    v13 = CallDirect v12 function=f3
    Return
}
function f1 name="parse" parameters=[{type=string}] results=[{type=anything}] {
    v1 = Builtin builtin=JSONParse
    v2 = Call v1, -, v0
    v3 = Convert v2 declared={type=undefined|null|L1} illTypedCause=FromLooseType
    Return v3
}
function f2 name="describe" parameters=[{type=string}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="show" [isFreeName]
    v3 = CallDirect v0 function=f1
    v4 = PropGet v3 key="total"
    v5 = Call v2, -, v4
    v6 = PropGet v3 key="items"
    v7 = Undefined
    v8 = StrictEq v6, v7
    Branch v8 -> taken b1(), notTaken b2()
  b1():
    v9 = Call v2, -, v3
    Return
  b2():
    v10 = PropGet v6 key="length"
    v11 = Call v2, -, v10
    v12 = Integer integer=1
    v13 = ElemGet v6, v12
    v14 = Call v2, -, v13
    v15 = Call v2, -, v3
    Return
}
function f3 name="change" parameters=[{type=string}] results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="show" [isFreeName]
    v3 = Builtin builtin=JSONParse
    v4 = Call v3, -, v0
    v5 = Convert v4 declared={type=undefined|null|L2} illTypedCause=FromLooseType
    v6 = PropGet v5 key="list"
    v7 = Integer integer=4
    v8 = Integer integer=6
    ElemSet v6, v7, v8
    PropSet v6, v8 key="named"
    v11 = PropGet v5 key="list"
    v12 = PropGet v11 key="length"
    v13 = Call v2, -, v12
    v14 = PropGet v11 key="named"
    v15 = Call v2, -, v14
    v16 = Integer integer=2
    v17 = ElemGet v11, v16
    v18 = Call v2, -, v17
    v19 = Call v2, -, v5
    Return
}
)IR"_s;

static constexpr ASCIILiteral outputOfRecordsWithArrayMembers = R"(2
2
{"id":8}
{"items":[{"id":7},{"id":8}],"total":2}
0
0
undefined
{"items":[],"total":0}
6
6
{"id":8}
{"items":[1,{"id":8},"x",null,[2],{"other":3}],"total":6}
1
1
undefined
{"total":1,"items":[{"id":9,"more":[1]}],"extra":[{"id":1}]}
1
1
undefined
{"items":{"0":{"id":5},"length":1},"total":1}
1
{"total":1}
5
6
undefined
{"list":[1,2,null,null,6]}
)"_s;

static constexpr ASCIILiteral showsValuesAsJSON = "globalThis.show = value => print(JSON.stringify(value));"_s;

// Validation stops the program at a value that is not of the type it is assumed to have.
static void testRecordsWithArrayMembers()
{
    checkOutputOfProgramInShell(recordsWithArrayMembers, outputOfRecordsWithArrayMembers, { }, showsValuesAsJSON);
    checkOutputOfProgramInShell(recordsWithArrayMembers, outputOfRecordsWithArrayMembers, { "validate"_s }, showsValuesAsJSON);
    checkOutputOfProgramInShell(recordsWithArrayMembers, outputOfRecordsWithArrayMembers, { "references=narrow"_s, "validate"_s }, showsValuesAsJSON);
    checkOutputOfProgramInShell(recordsWithArrayMembers, outputOfRecordsWithArrayMembers, { "disable=pass:InlineCalls"_s, "validate"_s }, showsValuesAsJSON);
    checkOutputOfProgramInShell(recordsWithArrayMembers, outputOfRecordsWithArrayMembers, { "disable=flow-results"_s }, showsValuesAsJSON);
}

std::span<const NativeTest> typedResultProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "typed-result-programs/records-with-array-members"_s, testRecordsWithArrayMembers },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
