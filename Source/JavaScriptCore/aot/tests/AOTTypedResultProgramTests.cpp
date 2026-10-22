/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

// type Row = { id: number }; type Page = { items: Row[]; total: number }; type Changed = { list: number[] }
// function parse(text) { return JSON.parse(text) as Page }
// function describe(text) {
//     const page = parse(text); show(page.total);
//     if (page.items === undefined) return show(page);
//     show(page.items.length); show(page.items[1]); show(page)
// }
// function change(text) {
//     const value = JSON.parse(text) as Changed; value.list[4] = 6; value.list.named = 6;
//     show(value.list.length); show(value.list.named); show(value.list[2]); show(value)
// }
static constexpr ASCIILiteral recordsWithArrayMembers = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Row" fields=[{key="id" declared={type=number}}]
layout L1 name="Page" fields=[{key="items" declared={type=Array<L0>}}, {key="total" declared={type=number}}]
layout L2 name="Changed" fields=[{key="list" declared={type=Array<number>}}]
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
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
  b0(v0: string):
    v1 = Builtin builtin=JSONParse
    v2 = Call v1, -, v0
    v3 = Convert v2 declared={type=undefined|null|L1} illTypedCause=FromLooseType
    Return v3
}
function f2 name="describe" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
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
  b0(v0: string):
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

// type Row = { id: number }; type Page = { items: Row[]; total: number }
// function describe(page) { show(page.items.length); show(page.items[1]); show(page) }
// const given = make(), validated = validate<Page>(given); describe(validated); show(validated.items === given.items);
// describe(JSON.parse<Page>(..)); describe(JSON.parse<Page>(..))
// The helpers that take over where compiled code gives up return undefined in the shell, so the program only gets through if it never does.
static constexpr ASCIILiteral validatedRecordsWithArrayMembers = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
expectedresult x0 type=L1 layouts=[L0, L1] validation=[{shape=Object{layout=L1 members=[{key="items" node=1 field=0}, {key="total" node=3 field=1}]} type=L1}, {shape=Array{element=2} type=Array<L0>}, {shape=Object{layout=L0 members=[{key="id" node=3 field=0}]} type=L0}, {shape=Primitive{primitive=Number} type=number}]
layout L0 name="Row" fields=[{key="id" declared={type=number}}]
layout L1 name="Page" fields=[{key="items" declared={type=Array<L0>}}, {key="total" declared={type=number}}]
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="show" [isFreeName]
    v2 = PropGet v0 key="make" [isFreeName]
    v3 = Call v2
    v4 = CallDirect v3 function=f1
    v5 = CallDirect v4 function=f3
    v6 = PropGet v4 key="items"
    v7 = PropGet v3 key="items"
    v8 = StrictEq v6, v7
    v9 = Call v1, -, v8
    v10 = String string="{\"items\":[{\"id\":7},{\"id\":8},{\"id\":9}],\"total\":3}"
    v11 = CallDirect v10 function=f2
    v12 = CallDirect v11 function=f3
    v13 = String string="{ \"total\" : 0 , \"items\" : [ ] }"
    v14 = CallDirect v13 function=f2
    v15 = CallDirect v14 function=f3
    Return
}
function f1 name="validates" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=BunBundleValidate
    v2 = Call v1, -, v0 expectedResult=x0
    Return v2
}
function f2 name="parses" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Builtin builtin=JSONParse
    v2 = Call v1, -, v0 expectedResult=x0
    Return v2
}
function f3 name="describe" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="show" [isFreeName]
    v3 = PropGet v0 key="items"
    v4 = PropGet v3 key="length"
    v5 = Call v2, -, v4
    v6 = Integer integer=1
    v7 = ElemGet v3, v6
    v8 = Call v2, -, v7
    v9 = Call v2, -, v0
    Return
}
)IR"_s;

static constexpr ASCIILiteral outputOfValidatedRecordsWithArrayMembers = R"(2
{"id":2}
{"items":[{"id":1},{"id":2}],"total":2}
false
3
{"id":8}
{"items":[{"id":7},{"id":8},{"id":9}],"total":3}
0
undefined
{"total":0,"items":[]}
)"_s;

static constexpr ASCIILiteral showsValuesAndMakesPage = "globalThis.show = value => print(JSON.stringify(value)); globalThis.make = () => ({ items: [{ id: 1 }, { id: 2 }], total: 2 });"_s;

static void testValidatedRecordsWithArrayMembers()
{
    checkOutputOfProgramInShell(validatedRecordsWithArrayMembers, outputOfValidatedRecordsWithArrayMembers, { }, showsValuesAndMakesPage);
    checkOutputOfProgramInShell(validatedRecordsWithArrayMembers, outputOfValidatedRecordsWithArrayMembers, { "validate"_s }, showsValuesAndMakesPage);
    checkOutputOfProgramInShell(validatedRecordsWithArrayMembers, outputOfValidatedRecordsWithArrayMembers, { "references=narrow"_s, "validate"_s }, showsValuesAndMakesPage);
    checkOutputOfProgramInShell(validatedRecordsWithArrayMembers, outputOfValidatedRecordsWithArrayMembers, { "disable=pass:InlineCalls"_s, "validate"_s }, showsValuesAndMakesPage);
    checkOutputOfProgramInShell(validatedRecordsWithArrayMembers, outputOfValidatedRecordsWithArrayMembers, { "disable=flow-results"_s }, showsValuesAndMakesPage);
}

std::span<const NativeTest> typedResultProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "typed-result-programs/records-with-array-members"_s, testRecordsWithArrayMembers },
        { "typed-result-programs/validated-records-with-array-members"_s, testValidatedRecordsWithArrayMembers },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
