/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral instanceMethodsNamedLikeObjectMembers = R"IR(layout L0 kind=Instance name="Plain" module=m0 position=146 constructor=f2
layout L1 kind=Instance module=m0 position=28 constructor=f3
layout L2 module=m0 position=455
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = Convert v0 declared={type=function flags=[IsExpected]}
    v2 = NewClosure function=f2
    v3 = NewClass v2 layout=L0
    PutModuleVariable v3 variable=g0
    v5 = FinishClass v3 layout=L0
    v6 = Convert v5 declared={type=function flags=[IsExpected]}
    v7 = Convert v5 declared={type=function flags=[IsExpected]}
    v8 = Convert v1 declared={type=function flags=[IsExpected]}
    v9 = String string="a"
    v10 = Call v8, -, v9 calleeName="make"
    v11 = Convert v10 declared={type=function flags=[IsExpected]}
    v12 = Construct v11
    v13 = Convert v12 declared={type=L1 flags=[IsExpected]}
    v14 = String string="bb"
    v15 = Call v8, -, v14 calleeName="make"
    v16 = Convert v15 declared={type=function flags=[IsExpected]}
    v17 = Construct v16
    v18 = Convert v17 declared={type=L1 flags=[IsExpected]}
    v19 = Builtin builtin=GlobalThis
    v20 = PropGet v19 key="print" [isFreeName]
    v21 = PropGet v13 key="toString"
    v22 = Convert v21 declared={type=function flags=[IsExpected]}
    v23 = Call v22, v13 calleeKey="toString" calleeName="toString"
    v24 = PropGet v18 key="toString"
    v25 = Convert v24 declared={type=function flags=[IsExpected]}
    v26 = Call v25, v18 calleeKey="toString" calleeName="toString"
    v27 = PropGet v13 key="valueOf"
    v28 = Convert v27 declared={type=function flags=[IsExpected]}
    v29 = Call v28, v13 calleeKey="valueOf" calleeName="valueOf"
    v30 = PropGet v18 key="valueOf"
    v31 = Convert v30 declared={type=function flags=[IsExpected]}
    v32 = Call v31, v18 calleeKey="valueOf" calleeName="valueOf"
    v33 = PropGet v13 key="hasOwnProperty"
    v34 = Convert v33 declared={type=function flags=[IsExpected]}
    v35 = String string="k"
    v36 = Call v34, v13, v35 calleeKey="hasOwnProperty" calleeName="hasOwnProperty"
    v37 = PropGet v18 key="hasOwnProperty"
    v38 = Convert v37 declared={type=function flags=[IsExpected]}
    v39 = String string="k"
    v40 = Call v38, v18, v39 calleeKey="hasOwnProperty" calleeName="hasOwnProperty"
    v41 = Call v20, -, v23, v26, v29, v32, v36, v40 calleeName="print"
    v42 = Builtin builtin=GlobalThis
    v43 = PropGet v42 key="print" [isFreeName]
    v44 = ToString v13
    v45 = ToString v18
    v46 = Integer integer=1
    v47 = Add v13, v46
    v48 = Integer integer=1
    v49 = Add v18, v48
    v50 = Call v43, -, v44, v45, v47, v49 calleeName="print"
    v51 = Builtin builtin=GlobalThis
    v52 = PropGet v51 key="print" [isFreeName]
    v53 = PropGet v13 key="toString"
    v54 = Convert v53 declared={type=function flags=[IsExpected]}
    v55 = Builtin builtin=GlobalThis
    v56 = PropGet v55 key="Object" [isFreeName]
    v57 = Convert v56 declared={type=builtin(Object) flags=[IsExpected]}
    v58 = PropGet v57 key="prototype"
    v59 = Convert v58 declared={type=builtin(ObjectPrototype) flags=[IsExpected]}
    v60 = PropGet v59 key="toString"
    v61 = Convert v60 declared={type=builtin(ObjectPrototypeToString) flags=[IsExpected]}
    v62 = StrictEq v54, v61
    v63 = PropGet v13 key="toString"
    v64 = Convert v63 declared={type=function flags=[IsExpected]}
    v65 = PropGet v18 key="toString"
    v66 = Convert v65 declared={type=function flags=[IsExpected]}
    v67 = StrictEq v64, v66
    v68 = Call v52, -, v62, v67 calleeName="print"
    v69 = Construct v7 calleeName="Plain"
    v70 = Convert v69 declared={type=L0 flags=[IsExpected]}
    v71 = Data data=d0
    v72 = NewArray v13, v70, v18, v71 contents=anything
    v73 = Convert v72 declared={type=Array<L0|L1> flags=[IsExpected]}
    v74 = Integer
    v75 = Convert v74 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v75)
  b2(v76):
    LoopHint
    v78 = Integer integer=4
    v79 = Less v76, v78
    Branch v79 -> taken b3(| 0), notTaken b6(| 0)
  b3(v80):
    Jump -> next b4()
  b4():
    v81 = Builtin builtin=GlobalThis
    v82 = PropGet v81 key="print" [isFreeName]
    v83 = ElemGet v73, v80
    v84 = Convert v83 declared={type=L0|L1 flags=[IsExpected]}
    v85 = PropGet v84 key="toString"
    v86 = Convert v85 declared={type=function flags=[IsExpected]}
    v87 = Call v86, v84 calleeKey="toString" calleeName="toString"
    v88 = ElemGet v73, v80
    v89 = Convert v88 declared={type=L0|L1 flags=[IsExpected]}
    v90 = PropGet v89 key="hasOwnProperty"
    v91 = Convert v90 declared={type=function flags=[IsExpected]}
    v92 = String string="k"
    v93 = Call v91, v89, v92 calleeKey="hasOwnProperty" calleeName="hasOwnProperty"
    v94 = Call v82, -, v87, v93 calleeName="print"
    Jump -> next b5()
  b5():
    v95 = Increment v80
    v96 = Convert v95 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v96)
  b6(v97):
    Jump -> next b7()
  b7():
    v98 = Undefined
    Return v98
}
function f1 name="make" module=m0 flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=function flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v1 = NewClosure function=f3
    v2 = NewClass v1 layout=L1
    v3 = NewClosure v0 function=f4
    DefineMethod v2, v3 layout=L1 key="toString"
    v5 = NewClosure v0 function=f5
    DefineMethod v2, v5 layout=L1 index=1 key="valueOf"
    v7 = NewClosure v0 function=f6
    DefineMethod v2, v7 layout=L1 index=2 key="hasOwnProperty"
    v9 = FinishClass v2 layout=L1
    v10 = Convert v9 declared={type=function flags=[IsExpected]}
    Return v10
}
function f2 name="Plain" module=m0 position=146 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f3 module=m0 position=28 kind=BaseConstructor flags=[HasNewTarget] thisType={type=anything} results=[{type=object}] {
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L1
    v2 = NewObject v1 layout=L1
    Return v2
}
function f4 name="toString" module=m0 position=36 kind=Method flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    Return v0
}
function f5 name="valueOf" module=m0 position=62 kind=Method flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v1 = PropGet v0 key="length"
    v2 = Integer integer=40
    v3 = Add v1, v2
    Return v3
}
function f6 name="hasOwnProperty" module=m0 position=99 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v2 = Add v0, v1
    Return v2
}
data d0 root=Object{layout=L2} module=m0
entry module=m0
function f7 name="call" role=Library flags=[HasRestParameter, HasThis] length=1 enclosingRuntimeFunction=FunctionPrototypeCall thisType={type=anything} parameters=[{type=anything}, {type=Array<anything>}] results=[{type=anything}] {
    v3 = CallList v0, v1, v2
    Return v3
}
function f8 name="apply" role=Library flags=[HasThis] length=2 enclosingRuntimeFunction=FunctionPrototypeApply thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = NewArray contents=anything
    AppendSpread v3, v2
    v5 = CallList v0, v1, v3
    Return v5
}
runtimefunction id=FunctionPrototypeApply function=f8
runtimefunction id=FunctionPrototypeCall function=f7
)IR"_s;

static constexpr ASCIILiteral membersOfLiteralsAndFunctions = R"IR(layout L0 module=m0 position=31 fields=[{key="call" declared={type=function flags=[IsExpected]} flags=[IsRequired, IsMethod]}, {key="toString" declared={type=function flags=[IsExpected]} flags=[IsRequired, IsMethod]}, {key="hasOwnProperty" declared={type=function flags=[IsExpected]} flags=[IsRequired, IsMethod]}]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = Convert v0 declared={type=function flags=[IsExpected]}
    v2 = NewClosure function=f2
    v3 = Convert v2 declared={type=function flags=[IsExpected]}
    v4 = Convert v1 declared={type=function flags=[IsExpected]}
    v5 = String string="a"
    v6 = Call v4, -, v5 calleeName="literal"
    v7 = Convert v6 declared={type=L0 flags=[IsExpected]}
    v8 = String string="b"
    v9 = Call v4, -, v8 calleeName="literal"
    v10 = Convert v9 declared={type=L0 flags=[IsExpected]}
    v11 = Builtin builtin=GlobalThis
    v12 = PropGet v11 key="print" [isFreeName]
    v13 = PropGet v7 key="call"
    v14 = Convert v13 declared={type=function flags=[IsExpected]}
    v15 = Integer integer=1
    v16 = Call v14, v7, v15 calleeKey="call" calleeName="call"
    v17 = PropGet v10 key="call"
    v18 = Convert v17 declared={type=function flags=[IsExpected]}
    v19 = Integer integer=2
    v20 = Call v18, v10, v19 calleeKey="call" calleeName="call"
    v21 = ToString v7
    v22 = ToString v10
    v23 = PropGet v7 key="hasOwnProperty"
    v24 = Convert v23 declared={type=function flags=[IsExpected]}
    v25 = String string="k"
    v26 = Call v24, v7, v25 calleeKey="hasOwnProperty" calleeName="hasOwnProperty"
    v27 = PropGet v10 key="hasOwnProperty"
    v28 = Convert v27 declared={type=function flags=[IsExpected]}
    v29 = String string="k"
    v30 = Call v28, v10, v29 calleeKey="hasOwnProperty" calleeName="hasOwnProperty"
    v31 = Call v12, -, v16, v20, v21, v22, v26, v30 calleeName="print"
    v32 = Convert v3 declared={type=function flags=[IsExpected]}
    v33 = String string="f"
    v34 = Call v32, -, v33 calleeName="withOwnCall"
    v35 = Convert v34 declared={type=function flags=[IsExpected]}
    v36 = String string="g"
    v37 = Call v32, -, v36 calleeName="withOwnCall"
    v38 = Convert v37 declared={type=function flags=[IsExpected]}
    v39 = Builtin builtin=GlobalThis
    v40 = PropGet v39 key="print" [isFreeName]
    v41 = PropGet v35 key="call"
    v42 = Convert v41 declared={type=function flags=[IsExpected]}
    v43 = Integer integer=1
    v44 = Integer integer=2
    v45 = Call v42, v35, v43, v44 calleeKey="call" calleeName="call"
    v46 = Convert v45 declared={type=string flags=[IsExpected]}
    v47 = PropGet v38 key="call"
    v48 = Convert v47 declared={type=function flags=[IsExpected]}
    v49 = Integer integer=1
    v50 = Integer integer=2
    v51 = Call v48, v38, v49, v50 calleeKey="call" calleeName="call"
    v52 = Convert v51 declared={type=string flags=[IsExpected]}
    v53 = Integer integer=3
    v54 = Call v35, -, v53 calleeName="F"
    v55 = Integer integer=4
    v56 = Call v38, -, v55 calleeName="G"
    v57 = Call v40, -, v46, v52, v54, v56 calleeName="print"
    v58 = PropDelete v35 key="call"
    v59 = Builtin builtin=GlobalThis
    v60 = PropGet v59 key="print" [isFreeName]
    v61 = PropGet v35 key="call"
    v62 = Convert v61 declared={type=function flags=[IsExpected]}
    v63 = Integer integer=1
    v64 = Integer integer=2
    v65 = Call v62, v35, v63, v64 calleeKey="call" calleeName="call"
    v66 = Convert v65 declared={type=string flags=[IsExpected]}
    v67 = PropGet v38 key="call"
    v68 = Convert v67 declared={type=function flags=[IsExpected]}
    v69 = Integer integer=1
    v70 = Integer integer=2
    v71 = Call v68, v38, v69, v70 calleeKey="call" calleeName="call"
    v72 = Convert v71 declared={type=string flags=[IsExpected]}
    v73 = Call v60, -, v66, v72 calleeName="print"
    v74 = Undefined
    Return v74
}
function f1 name="literal" module=m0 flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=L0 flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v1 = NewObject layout=L0
    v2 = NewClosure v0 function=f3
    v3 = Convert v2 declared={type=function flags=[IsExpected]}
    PropDefine v1, v3 key="call"
    v5 = NewClosure v0 function=f4
    v6 = Convert v5 declared={type=function flags=[IsExpected]}
    PropDefine v1, v6 key="toString"
    v8 = NewClosure v0 function=f5
    v9 = Convert v8 declared={type=function flags=[IsExpected]}
    PropDefine v1, v9 key="hasOwnProperty"
    v11 = FinishObject v1
    v12 = Convert v11 declared={type=L0 flags=[IsExpected]}
    Return v12
}
function f2 name="withOwnCall" module=m0 position=129 flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=function flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v1 = NewClosure v0 function=f6
    v2 = Convert v1 declared={type=function flags=[IsExpected]}
    v3 = Convert v2 declared={type=function flags=[IsExpected]}
    v4 = NewClosure v0 function=f7
    v5 = Convert v4 declared={type=function flags=[IsExpected]}
    PropSet v3, v5 key="call"
    Return v3
}
function f3 name="call" module=m0 position=33 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v2 = Add v0, v1
    Return v2
}
function f4 name="toString" module=m0 position=61 kind=Method flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    Return v0
}
function f5 name="hasOwnProperty" module=m0 position=88 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v2 = Add v0, v1
    Return v2
}
function f6 name="f" module=m0 position=157 flags=[HasThis, HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v3 = Add v0, v1
    v4 = Add v3, v2
    Return v4
}
function f7 module=m0 position=206 length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=string flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v2 = String string="own"
    v3 = Add v2, v0
    v4 = Add v3, v1
    v5 = Convert v4 declared={type=string flags=[IsExpected]}
    Return v5
}
entry module=m0
function f8 name="call" role=Library flags=[HasRestParameter, HasThis] length=1 enclosingRuntimeFunction=FunctionPrototypeCall thisType={type=anything} parameters=[{type=anything}, {type=Array<anything>}] results=[{type=anything}] {
    v3 = CallList v0, v1, v2
    Return v3
}
function f9 name="apply" role=Library flags=[HasThis] length=2 enclosingRuntimeFunction=FunctionPrototypeApply thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = NewArray contents=anything
    AppendSpread v3, v2
    v5 = CallList v0, v1, v3
    Return v5
}
runtimefunction id=FunctionPrototypeApply function=f9
runtimefunction id=FunctionPrototypeCall function=f8
)IR"_s;

static constexpr ASCIILiteral membersOfPrototypeGivenToObjectCreate = R"IR(layout L0 module=m0 position=42 fields=[{key="toString" declared={type=function flags=[IsExpected]} flags=[IsRequired, IsMethod]}, {key="hasOwnProperty" declared={type=function flags=[IsExpected]} flags=[IsRequired, IsMethod]}]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = Convert v0 declared={type=function flags=[IsExpected]}
    v2 = Convert v1 declared={type=function flags=[IsExpected]}
    v3 = String string="a"
    v4 = Call v2, -, v3 calleeName="make"
    v5 = String string="b"
    v6 = Call v2, -, v5 calleeName="make"
    v7 = Builtin builtin=GlobalThis
    v8 = PropGet v7 key="print" [isFreeName]
    v9 = PropGet v4 key="toString"
    v10 = Call v9, v4 calleeKey="toString" calleeName="toString"
    v11 = PropGet v6 key="toString"
    v12 = Call v11, v6 calleeKey="toString" calleeName="toString"
    v13 = ToString v4
    v14 = PropGet v4 key="hasOwnProperty"
    v15 = String string="x"
    v16 = Call v14, v4, v15 calleeKey="hasOwnProperty" calleeName="hasOwnProperty"
    v17 = PropGet v6 key="hasOwnProperty"
    v18 = String string="y"
    v19 = Call v17, v6, v18 calleeKey="hasOwnProperty" calleeName="hasOwnProperty"
    v20 = Call v8, -, v10, v12, v13, v16, v19 calleeName="print"
    v21 = Undefined
    Return v21
}
function f1 name="make" module=m0 flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="Object" [isFreeName]
    v3 = Convert v2 declared={type=builtin(Object) flags=[IsExpected]}
    v4 = PropGet v3 key="create"
    v5 = Convert v4 declared={type=function flags=[IsExpected]}
    v6 = Convert v5 declared={type=builtin(ObjectCreate) flags=[IsExpected]}
    v7 = NewObject layout=L0
    v8 = NewClosure v0 function=f2
    v9 = Convert v8 declared={type=function flags=[IsExpected]}
    PropDefine v7, v9 key="toString"
    v11 = NewClosure v0 function=f3
    v12 = Convert v11 declared={type=function flags=[IsExpected]}
    PropDefine v7, v12 key="hasOwnProperty"
    v14 = FinishObject v7
    v15 = Call v6, v3, v14 calleeKey="create" calleeName="create"
    ReturnCall v15
}
function f2 name="toString" module=m0 position=44 kind=Method flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    Return v0
}
function f3 name="hasOwnProperty" module=m0 position=71 kind=Method flags=[HasExplicitName] length=1 captures=[{type=anything}] thisType={type=anything} parameters=[{type=number|string|symbol flags=[IsExpected]}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = Add v0, v1
    Return v2
}
entry module=m0
function f4 name="call" role=Library flags=[HasRestParameter, HasThis] length=1 enclosingRuntimeFunction=FunctionPrototypeCall thisType={type=anything} parameters=[{type=anything}, {type=Array<anything>}] results=[{type=anything}] {
    v3 = CallList v0, v1, v2
    Return v3
}
function f5 name="apply" role=Library flags=[HasThis] length=2 enclosingRuntimeFunction=FunctionPrototypeApply thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = NewArray contents=anything
    AppendSpread v3, v2
    v5 = CallList v0, v1, v3
    Return v5
}
runtimefunction id=FunctionPrototypeApply function=f5
runtimefunction id=FunctionPrototypeCall function=f4
)IR"_s;

static void checkUnderEveryOption(ASCIILiteral programText, ASCIILiteral expectedOutput)
{
    static constexpr ASCIILiteral optionWords[] = {
        "validate"_s,
        "check=VerifyEachPass"_s,
        "disable=pass:InlineCalls"_s,
        "disable=pass:ResolveCalls"_s,
        "heat=EverythingIsHot"_s,
        "references=narrow"_s,
    };
    checkOutputOfProgramInShell(programText, expectedOutput, { });
    for (ASCIILiteral word : optionWords)
        checkOutputOfProgramInShell(programText, expectedOutput, { word });
}

static void testInstanceMethodsNamedLikeObjectMembers()
{
    checkUnderEveryOption(instanceMethodsNamedLikeObjectMembers, "a bb 41 42 ak bbk\na bb 42 43\nfalse false\na ak\n[object Object] false\nbb bbk\n[object Object] false\n"_s);
}

static void testMembersOfLiteralsAndFunctions()
{
    checkUnderEveryOption(membersOfLiteralsAndFunctions, "a1 b2 a b ak bk\nownf1 owng1 fundefined3 gundefined4\nf12 owng1\n"_s);
}

static void testMembersOfPrototypeGivenToObjectCreate()
{
    checkUnderEveryOption(membersOfPrototypeGivenToObjectCreate, "a b a ax by\n"_s);
}

std::span<const NativeTest> membersNamedLikeBuiltinsTests()
{
    static constexpr NativeTest tests[] = {
        { "members-named-like-builtins/instance-methods-named-like-object-members"_s, testInstanceMethodsNamedLikeObjectMembers },
        { "members-named-like-builtins/members-of-literals-and-functions"_s, testMembersOfLiteralsAndFunctions },
        { "members-named-like-builtins/members-of-prototype-given-to-object-create"_s, testMembersOfPrototypeGivenToObjectCreate },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
