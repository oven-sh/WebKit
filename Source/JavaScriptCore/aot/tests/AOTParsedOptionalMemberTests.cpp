/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

// type Kinds = { always: number; count?: number; name?: string; flag?: boolean; list?: number[]; orNull?: string | null }
// function parse(text) { return JSON.parse<Kinds>(text) }
// function storeTyped(o: Kinds) { o.name = "typed"; o.count = 3.5; o.flag = true }
// function storeBackwards(o: Kinds) { o.orNull = null; o.flag = false; o.name = "back" }
// For each text: show(parse(text)); the same after store(o), which is other code, after storeTyped(o) and after storeBackwards(o).
static constexpr ASCIILiteral recordsWithOptionalMembers = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
expectedresult x0 type=L1 layouts=[L0, L1] validation=[{shape=Object{layout=L1 members=[{key="always" node=1 field=0}, {key="count" node=1 field=1 isOptional=true}, {key="name" node=2 field=2 isOptional=true}, {key="flag" node=3 field=3 isOptional=true}, {key="list" node=5 field=5 isOptional=true}, {key="orNull" node=6 field=6 isOptional=true}]} type=L1}, {shape=Primitive{primitive=Number} type=number}, {shape=Primitive{} type=string}, {shape=Primitive{primitive=Boolean} type=boolean}, {shape=Object{layout=L0 members=[{key="id" node=1 field=0}, {key="note" node=2 field=1 isOptional=true}]} type=L0}, {shape=Array{element=1} type=Array<number>}, {shape=Union{arms=[7, 2]} type=null|string}, {shape=Null type=null}]
layout L0 name="Inner" fields=[{key="id" declared={type=number}}, {key="note" declared={type=undefined|string}}]
layout L1 name="Kinds" fields=[{key="always" declared={type=number}}, {key="count" declared={type=undefined|number}}, {key="name" declared={type=undefined|string}}, {key="flag" declared={type=undefined|boolean}}, {key="inner" declared={type=undefined|L0}}, {key="list" declared={type=undefined|Array<number>}}, {key="orNull" declared={type=undefined|null|string}}]
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="show" [isFreeName]
    v2 = PropGet v0 key="store" [isFreeName]
    v3 = String string="{\"always\":1}"
    v4 = CallDirect v3 function=f1
    v5 = Call v1, -, v4
    v6 = Call v2, -, v4
    v7 = Call v1, -, v4
    v8 = CallDirect v3 function=f1
    v9 = CallDirect v8 function=f2
    v10 = Call v1, -, v8
    v11 = CallDirect v3 function=f1
    v12 = CallDirect v11 function=f3
    v13 = Call v1, -, v11
    v14 = String string="{\"always\":1,\"count\":1.5,\"name\":\"n\",\"flag\":true,\"list\":[1,2],\"orNull\":null}"
    v15 = CallDirect v14 function=f1
    v16 = Call v1, -, v15
    v17 = Call v2, -, v15
    v18 = Call v1, -, v15
    v19 = CallDirect v14 function=f1
    v20 = CallDirect v19 function=f2
    v21 = Call v1, -, v19
    v22 = CallDirect v14 function=f1
    v23 = CallDirect v22 function=f3
    v24 = Call v1, -, v22
    v25 = String string="{\"orNull\":\"s\",\"list\":[],\"flag\":false,\"name\":\"\",\"count\":0,\"always\":2}"
    v26 = CallDirect v25 function=f1
    v27 = Call v1, -, v26
    v28 = Call v2, -, v26
    v29 = Call v1, -, v26
    v30 = CallDirect v25 function=f1
    v31 = CallDirect v30 function=f2
    v32 = Call v1, -, v30
    v33 = CallDirect v25 function=f1
    v34 = CallDirect v33 function=f3
    v35 = Call v1, -, v33
    v36 = String string="{\"name\":\"n\",\"always\":3}"
    v37 = CallDirect v36 function=f1
    v38 = Call v1, -, v37
    v39 = Call v2, -, v37
    v40 = Call v1, -, v37
    v41 = CallDirect v36 function=f1
    v42 = CallDirect v41 function=f2
    v43 = Call v1, -, v41
    v44 = CallDirect v36 function=f1
    v45 = CallDirect v44 function=f3
    v46 = Call v1, -, v44
    v47 = String string="{\"always\":4,\"flag\":true,\"count\":7}"
    v48 = CallDirect v47 function=f1
    v49 = Call v1, -, v48
    v50 = Call v2, -, v48
    v51 = Call v1, -, v48
    v52 = CallDirect v47 function=f1
    v53 = CallDirect v52 function=f2
    v54 = Call v1, -, v52
    v55 = CallDirect v47 function=f1
    v56 = CallDirect v55 function=f3
    v57 = Call v1, -, v55
    v58 = String string="{\"list\":[5],\"always\":5}"
    v59 = CallDirect v58 function=f1
    v60 = Call v1, -, v59
    v61 = Call v2, -, v59
    v62 = Call v1, -, v59
    v63 = CallDirect v58 function=f1
    v64 = CallDirect v63 function=f2
    v65 = Call v1, -, v63
    v66 = CallDirect v58 function=f1
    v67 = CallDirect v66 function=f3
    v68 = Call v1, -, v66
    Return
}
function f1 name="parse" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Builtin builtin=JSONParse
    v2 = Call v1, -, v0 expectedResult=x0
    Return v2
}
function f2 name="storeTyped" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Convert v0 declared={type=L1} illTypedCause=FromLooseType
    v2 = String string="typed"
    PropSet v1, v2 key="name"
    v4 = Double number=3.5
    PropSet v1, v4 key="count"
    v6 = Boolean boolean=true
    PropSet v1, v6 key="flag"
    Return
}
function f3 name="storeBackwards" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Convert v0 declared={type=L1} illTypedCause=FromLooseType
    v2 = Null
    PropSet v1, v2 key="orNull"
    v4 = Boolean boolean=false
    PropSet v1, v4 key="flag"
    v6 = String string="back"
    PropSet v1, v6 key="name"
    Return
}
)IR"_s;

// The same with function parse(text) { return JSON.parse(text) as Kinds }
static constexpr ASCIILiteral recordsWithOptionalMembersByCast = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Inner" fields=[{key="id" declared={type=number}}, {key="note" declared={type=undefined|string}}]
layout L1 name="Kinds" fields=[{key="always" declared={type=number}}, {key="count" declared={type=undefined|number}}, {key="name" declared={type=undefined|string}}, {key="flag" declared={type=undefined|boolean}}, {key="inner" declared={type=undefined|L0}}, {key="list" declared={type=undefined|Array<number>}}, {key="orNull" declared={type=undefined|null|string}}]
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="show" [isFreeName]
    v2 = PropGet v0 key="store" [isFreeName]
    v3 = String string="{\"always\":1}"
    v4 = CallDirect v3 function=f1
    v5 = Call v1, -, v4
    v6 = Call v2, -, v4
    v7 = Call v1, -, v4
    v8 = CallDirect v3 function=f1
    v9 = CallDirect v8 function=f2
    v10 = Call v1, -, v8
    v11 = CallDirect v3 function=f1
    v12 = CallDirect v11 function=f3
    v13 = Call v1, -, v11
    v14 = String string="{\"always\":1,\"count\":1.5,\"name\":\"n\",\"flag\":true,\"list\":[1,2],\"orNull\":null}"
    v15 = CallDirect v14 function=f1
    v16 = Call v1, -, v15
    v17 = Call v2, -, v15
    v18 = Call v1, -, v15
    v19 = CallDirect v14 function=f1
    v20 = CallDirect v19 function=f2
    v21 = Call v1, -, v19
    v22 = CallDirect v14 function=f1
    v23 = CallDirect v22 function=f3
    v24 = Call v1, -, v22
    v25 = String string="{\"orNull\":\"s\",\"list\":[],\"flag\":false,\"name\":\"\",\"count\":0,\"always\":2}"
    v26 = CallDirect v25 function=f1
    v27 = Call v1, -, v26
    v28 = Call v2, -, v26
    v29 = Call v1, -, v26
    v30 = CallDirect v25 function=f1
    v31 = CallDirect v30 function=f2
    v32 = Call v1, -, v30
    v33 = CallDirect v25 function=f1
    v34 = CallDirect v33 function=f3
    v35 = Call v1, -, v33
    v36 = String string="{\"name\":\"n\",\"always\":3}"
    v37 = CallDirect v36 function=f1
    v38 = Call v1, -, v37
    v39 = Call v2, -, v37
    v40 = Call v1, -, v37
    v41 = CallDirect v36 function=f1
    v42 = CallDirect v41 function=f2
    v43 = Call v1, -, v41
    v44 = CallDirect v36 function=f1
    v45 = CallDirect v44 function=f3
    v46 = Call v1, -, v44
    v47 = String string="{\"always\":4,\"flag\":true,\"count\":7}"
    v48 = CallDirect v47 function=f1
    v49 = Call v1, -, v48
    v50 = Call v2, -, v48
    v51 = Call v1, -, v48
    v52 = CallDirect v47 function=f1
    v53 = CallDirect v52 function=f2
    v54 = Call v1, -, v52
    v55 = CallDirect v47 function=f1
    v56 = CallDirect v55 function=f3
    v57 = Call v1, -, v55
    v58 = String string="{\"list\":[5],\"always\":5}"
    v59 = CallDirect v58 function=f1
    v60 = Call v1, -, v59
    v61 = Call v2, -, v59
    v62 = Call v1, -, v59
    v63 = CallDirect v58 function=f1
    v64 = CallDirect v63 function=f2
    v65 = Call v1, -, v63
    v66 = CallDirect v58 function=f1
    v67 = CallDirect v66 function=f3
    v68 = Call v1, -, v66
    Return
}
function f1 name="parse" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Builtin builtin=JSONParse
    v2 = Call v1, -, v0
    v3 = Convert v2 declared={type=undefined|null|L1} illTypedCause=FromLooseType
    Return v3
}
function f2 name="storeTyped" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Convert v0 declared={type=L1} illTypedCause=FromLooseType
    v2 = String string="typed"
    PropSet v1, v2 key="name"
    v4 = Double number=3.5
    PropSet v1, v4 key="count"
    v6 = Boolean boolean=true
    PropSet v1, v6 key="flag"
    Return
}
function f3 name="storeBackwards" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Convert v0 declared={type=L1} illTypedCause=FromLooseType
    v2 = Null
    PropSet v1, v2 key="orNull"
    v4 = Boolean boolean=false
    PropSet v1, v4 key="flag"
    v6 = String string="back"
    PropSet v1, v6 key="name"
    Return
}
)IR"_s;

// What the shell prints for the same statements with JSON.parse(text).
static constexpr ASCIILiteral outputOfRecordsWithOptionalMembers = R"(in always | own always | keys always | loop always | {"always":1} | {"always":1}
in always,name,list | own always,name,list | keys always,name,list | loop always,name,list | {"always":1,"name":"late","list":[9]} | {"always":1,"name":"late","list":[9]}
in always,count,name,flag | own always,count,name,flag | keys always,name,count,flag | loop always,name,count,flag | {"always":1,"name":"typed","count":3.5,"flag":true} | {"always":1,"name":"typed","count":3.5,"flag":true}
in always,name,flag,orNull | own always,name,flag,orNull | keys always,orNull,flag,name | loop always,orNull,flag,name | {"always":1,"orNull":null,"flag":false,"name":"back"} | {"always":1,"orNull":null,"flag":false,"name":"back"}
in always,count,name,flag,list,orNull | own always,count,name,flag,list,orNull | keys always,count,name,flag,list,orNull | loop always,count,name,flag,list,orNull | {"always":1,"count":1.5,"name":"n","flag":true,"list":[1,2],"orNull":null} | {"always":1,"count":1.5,"name":"n","flag":true,"list":[1,2],"orNull":null}
in always,count,name,flag,list,orNull | own always,count,name,flag,list,orNull | keys always,count,name,flag,list,orNull | loop always,count,name,flag,list,orNull | {"always":1,"count":1.5,"name":"late","flag":true,"list":[9],"orNull":null} | {"always":1,"count":1.5,"name":"late","flag":true,"list":[9],"orNull":null}
in always,count,name,flag,list,orNull | own always,count,name,flag,list,orNull | keys always,count,name,flag,list,orNull | loop always,count,name,flag,list,orNull | {"always":1,"count":3.5,"name":"typed","flag":true,"list":[1,2],"orNull":null} | {"always":1,"count":3.5,"name":"typed","flag":true,"list":[1,2],"orNull":null}
in always,count,name,flag,list,orNull | own always,count,name,flag,list,orNull | keys always,count,name,flag,list,orNull | loop always,count,name,flag,list,orNull | {"always":1,"count":1.5,"name":"back","flag":false,"list":[1,2],"orNull":null} | {"always":1,"count":1.5,"name":"back","flag":false,"list":[1,2],"orNull":null}
in always,count,name,flag,list,orNull | own always,count,name,flag,list,orNull | keys orNull,list,flag,name,count,always | loop orNull,list,flag,name,count,always | {"orNull":"s","list":[],"flag":false,"name":"","count":0,"always":2} | {"orNull":"s","list":[],"flag":false,"name":"","count":0,"always":2}
in always,count,name,flag,list,orNull | own always,count,name,flag,list,orNull | keys orNull,list,flag,name,count,always | loop orNull,list,flag,name,count,always | {"orNull":"s","list":[9],"flag":false,"name":"late","count":0,"always":2} | {"orNull":"s","list":[9],"flag":false,"name":"late","count":0,"always":2}
in always,count,name,flag,list,orNull | own always,count,name,flag,list,orNull | keys orNull,list,flag,name,count,always | loop orNull,list,flag,name,count,always | {"orNull":"s","list":[],"flag":true,"name":"typed","count":3.5,"always":2} | {"orNull":"s","list":[],"flag":true,"name":"typed","count":3.5,"always":2}
in always,count,name,flag,list,orNull | own always,count,name,flag,list,orNull | keys orNull,list,flag,name,count,always | loop orNull,list,flag,name,count,always | {"orNull":null,"list":[],"flag":false,"name":"back","count":0,"always":2} | {"orNull":null,"list":[],"flag":false,"name":"back","count":0,"always":2}
in always,name | own always,name | keys name,always | loop name,always | {"name":"n","always":3} | {"name":"n","always":3}
in always,name,list | own always,name,list | keys name,always,list | loop name,always,list | {"name":"late","always":3,"list":[9]} | {"name":"late","always":3,"list":[9]}
in always,count,name,flag | own always,count,name,flag | keys name,always,count,flag | loop name,always,count,flag | {"name":"typed","always":3,"count":3.5,"flag":true} | {"name":"typed","always":3,"count":3.5,"flag":true}
in always,name,flag,orNull | own always,name,flag,orNull | keys name,always,orNull,flag | loop name,always,orNull,flag | {"name":"back","always":3,"orNull":null,"flag":false} | {"name":"back","always":3,"orNull":null,"flag":false}
in always,count,flag | own always,count,flag | keys always,flag,count | loop always,flag,count | {"always":4,"flag":true,"count":7} | {"always":4,"flag":true,"count":7}
in always,count,name,flag,list | own always,count,name,flag,list | keys always,flag,count,name,list | loop always,flag,count,name,list | {"always":4,"flag":true,"count":7,"name":"late","list":[9]} | {"always":4,"flag":true,"count":7,"name":"late","list":[9]}
in always,count,name,flag | own always,count,name,flag | keys always,flag,count,name | loop always,flag,count,name | {"always":4,"flag":true,"count":3.5,"name":"typed"} | {"always":4,"flag":true,"count":3.5,"name":"typed"}
in always,count,name,flag,orNull | own always,count,name,flag,orNull | keys always,flag,count,orNull,name | loop always,flag,count,orNull,name | {"always":4,"flag":false,"count":7,"orNull":null,"name":"back"} | {"always":4,"flag":false,"count":7,"orNull":null,"name":"back"}
in always,list | own always,list | keys list,always | loop list,always | {"list":[5],"always":5} | {"list":[5],"always":5}
in always,name,list | own always,name,list | keys list,always,name | loop list,always,name | {"list":[9],"always":5,"name":"late"} | {"list":[9],"always":5,"name":"late"}
in always,count,name,flag,list | own always,count,name,flag,list | keys list,always,name,count,flag | loop list,always,name,count,flag | {"list":[5],"always":5,"name":"typed","count":3.5,"flag":true} | {"list":[5],"always":5,"name":"typed","count":3.5,"flag":true}
in always,name,flag,list,orNull | own always,name,flag,list,orNull | keys list,always,orNull,flag,name | loop list,always,orNull,flag,name | {"list":[5],"always":5,"orNull":null,"flag":false,"name":"back"} | {"list":[5],"always":5,"orNull":null,"flag":false,"name":"back"}
)"_s;

static constexpr ASCIILiteral showsWhatExists = R"js(
const names = ["always", "count", "name", "flag", "inner", "list", "orNull"];
globalThis.show = value => {
    let inLoop = [];
    for (let key in value)
        inLoop[inLoop.length] = key;
    let present = [], own = [];
    for (let name of names) {
        if (name in value)
            present[present.length] = name;
        if (Object.prototype.hasOwnProperty.call(value, name))
            own[own.length] = name;
    }
    print("in " + present + " | own " + own + " | keys " + Object.keys(value) + " | loop " + inLoop + " | " + JSON.stringify(value) + " | " + JSON.stringify({ ...value }));
};
globalThis.store = value => { value.name = "late"; value.list = [9]; };
)js"_s;

static void check(OptionWords optionWords)
{
    checkOutputOfProgramInShell(recordsWithOptionalMembers, outputOfRecordsWithOptionalMembers, optionWords, showsWhatExists);
    checkOutputOfProgramInShell(recordsWithOptionalMembersByCast, outputOfRecordsWithOptionalMembers, optionWords, showsWhatExists);
}

// A member that the text leaves out does not exist, and a later store creates it behind the other keys.
static void testEveryPass()
{
    check({ });
    check({ "validate"_s });
    check({ "references=narrow"_s, "validate"_s });
}

static void testHeat()
{
    check({ "heat=EverythingIsHot"_s });
    check({ "heat=EverythingIsCold"_s });
}

static void testWithoutPasses()
{
    check({ "disable=pass:InlineCalls"_s, "validate"_s });
    check({ "disable=pass:PlaceTypeTests"_s });
    check({ "disable=pass:ResolveAccesses"_s });
    check({ "disable=flow-results"_s });
}

std::span<const NativeTest> parsedOptionalMemberTests()
{
    static constexpr NativeTest tests[] = {
        { "parsed-optional-members/every-pass"_s, testEveryPass },
        { "parsed-optional-members/heat"_s, testHeat },
        { "parsed-optional-members/without-passes"_s, testWithoutPasses },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
