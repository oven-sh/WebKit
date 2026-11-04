/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

// class Failure extends Error { code, detail, info, describe(), get twice() }, class Deeper extends Failure { level, describe() }
static constexpr ASCIILiteral errorSubclasses = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Failure" constructor=f1 fields=[{key="code" declared={type=anything}}, {key="detail" declared={type=anything}}, {key="info" declared={type=anything}}] flags=[HasHeritage]
layout L1 kind=Instance name="Deeper" base=L0 constructor=f2 fields=[{key="level" declared={type=anything}}] flags=[HasHeritage]
layout L2 name="Info" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = PropGet v0 key="Error" [isFreeName]
    v3 = NewClosure function=f1
    v4 = ResolveHeritage v2
    v5 = NewClass v3, v2, v4 layout=L0
    v6 = NewClosure function=f3
    DefineMethod v5, v6 layout=L0 key="describe"
    v8 = NewClosure function=f4
    DefineMethod v5, v8 layout=L0 methodKind=Getter index=1 key="twice"
    v10 = FinishClass v5 layout=L0
    v11 = NewClosure function=f2
    v12 = ResolveHeritage v10
    v13 = NewClass v11, v10, v12 layout=L1
    v14 = NewClosure function=f5
    DefineMethod v13, v14 layout=L1 key="describe"
    v16 = FinishClass v13 layout=L1
    v17 = String string="boom"
    v18 = Construct v10, -, v17
    v19 = PropGet v18 key="code"
    v20 = PropGet v18 key="detail"
    v21 = PropGet v18 key="info"
    v22 = PropGet v21 key="a"
    v23 = Call v1, -, v19, v20, v22
    v24 = PropGet v18 key="describe"
    v25 = Call v24, v18 calleeKey="describe"
    v26 = PropGet v18 key="twice"
    v27 = Call v1, -, v25, v26
    v28 = InstanceOf v18, v10
    v29 = InstanceOf v18, v2
    v30 = InstanceOf v18, v16
    v31 = Call v1, -, v28, v29, v30
    v32 = Construct v16, -, v17
    v33 = PropGet v32 key="code"
    v34 = PropGet v32 key="level"
    v35 = PropGet v32 key="describe"
    v36 = Call v35, v32 calleeKey="describe"
    v37 = PropGet v32 key="twice"
    v38 = Call v1, -, v33, v34, v36, v37
    v39 = InstanceOf v32, v16
    v40 = InstanceOf v32, v10
    v41 = InstanceOf v32, v2
    v42 = Call v1, -, v39, v40, v41
    v43 = PropGet v0 key="Object" [isFreeName]
    v44 = PropGet v43 key="keys"
    v45 = Call v44, v43, v32 calleeKey="keys"
    v46 = PropGet v45 key="length"
    v47 = Integer integer=0
    v48 = ElemGet v45, v47
    v49 = Integer integer=3
    v50 = ElemGet v45, v49
    v51 = Call v1, -, v46, v48, v50
    v52 = NewArray contents=anything
    Jump -> next b1(v47)
  b1(v53):
    LoopHint
    v55 = Integer integer=2000
    v56 = Less v53, v55
    Branch v56 -> taken b2(), notTaken b3()
  b2():
    v57 = Construct v16, -, v17
    v58 = PropGet v52 key="push"
    v59 = Call v58, v52, v57 calleeKey="push"
    v60 = Integer integer=1
    v61 = Add v53, v60
    Jump -> next b1(v61)
  b3():
    v62 = PropGet v0 key="gc" [isFreeName]
    v63 = Call v62
    Jump -> next b4(v47, v47)
  b4(v64, v65):
    LoopHint
    v67 = Integer integer=2000
    v68 = Less v64, v67
    Branch v68 -> taken b5(), notTaken b6()
  b5():
    v69 = ElemGet v52, v64
    v70 = PropGet v69 key="code"
    v71 = PropGet v69 key="level"
    v72 = PropGet v69 key="info"
    v73 = PropGet v72 key="a"
    v74 = Add v65, v70
    v75 = Add v74, v71
    v76 = Add v75, v73
    v77 = Integer integer=1
    v78 = Add v64, v77
    Jump -> next b4(v78, v76)
  b6():
    v79 = Call v1, -, v65
    Return
}
function f1 name="Failure" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] parameters=[{type=anything}] results=[{type=anything}] {
    v3 = GetSuperBase v0 homeKind=Object
    v4 = SuperConstruct v3, v1, v2 layout=L0
    v5 = Integer integer=7
    PropSet v4, v5 key="code" mode=Strict
    v7 = String string="d"
    v8 = Add v7, v2
    PropSet v4, v8 key="detail" mode=Strict
    v10 = Integer integer=3
    v11 = NewObject -, v10 layout=L2 fields=[0]
    PropSet v4, v11 key="info" mode=Strict
    Return v4
}
function f2 name="Deeper" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] parameters=[{type=anything}] results=[{type=anything}] {
    v3 = GetSuperBase v0 homeKind=Object
    v4 = SuperConstruct v3, v1, v2 layout=L1
    v5 = Integer integer=2
    PropDefine v4, v5 key="level"
    v7 = Integer integer=9
    PropSet v4, v7 key="code" mode=Strict
    Return v4
}
function f3 name="describe" kind=Method flags=[HasThis] results=[{type=anything}] {
    v1 = PropGet v0 key="detail"
    v2 = String string=":"
    v3 = Add v1, v2
    v4 = PropGet v0 key="code"
    v5 = Add v3, v4
    Return v5
}
function f4 name="twice" kind=Getter flags=[HasThis] results=[{type=anything}] {
    v1 = PropGet v0 key="code"
    v2 = Integer integer=2
    v3 = Mul v1, v2
    Return v3
}
function f5 name="describe" kind=Method flags=[HasThis] results=[{type=anything}] {
    v1 = String string="deep "
    v2 = PropGet v0 key="detail"
    v3 = Add v1, v2
    v4 = PropGet v0 key="level"
    v5 = Add v3, v4
    Return v5
}
)IR"_s;

// class List extends Array { tag, size, label() }, class Registry extends Map { hits, name, lookup(key) }, class Bytes extends Uint8Array { cursor, next() }
static constexpr ASCIILiteral collectionSubclasses = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="List" constructor=f1 fields=[{key="tag" declared={type=anything}}, {key="size" declared={type=anything}}] flags=[HasHeritage]
layout L1 kind=Instance name="Registry" constructor=f2 fields=[{key="hits" declared={type=anything}}, {key="name" declared={type=anything}}] flags=[HasHeritage]
layout L2 kind=Instance name="Bytes" constructor=f3 fields=[{key="cursor" declared={type=anything}}] flags=[HasHeritage]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = PropGet v0 key="Array" [isFreeName]
    v3 = NewClosure function=f1
    v4 = ResolveHeritage v2
    v5 = NewClass v3, v2, v4 layout=L0
    v6 = NewClosure function=f4
    DefineMethod v5, v6 layout=L0 key="label"
    v8 = FinishClass v5 layout=L0
    v9 = PropGet v0 key="Map" [isFreeName]
    v10 = NewClosure function=f2
    v11 = ResolveHeritage v9
    v12 = NewClass v10, v9, v11 layout=L1
    v13 = NewClosure function=f5
    DefineMethod v12, v13 layout=L1 key="lookup"
    v15 = FinishClass v12 layout=L1
    v16 = PropGet v0 key="Uint8Array" [isFreeName]
    v17 = NewClosure function=f3
    v18 = ResolveHeritage v16
    v19 = NewClass v17, v16, v18 layout=L2
    v20 = NewClosure function=f6
    DefineMethod v19, v20 layout=L2 key="next"
    v22 = FinishClass v19 layout=L2
    v23 = Construct v8
    v24 = PropGet v23 key="length"
    v25 = PropGet v23 key="push"
    v26 = Integer integer=5
    v27 = Call v25, v23, v26 calleeKey="push"
    v28 = Call v25, v23, v26 calleeKey="push"
    v29 = PropGet v23 key="length"
    v30 = Integer integer=1
    v31 = ElemGet v23, v30
    v32 = PropGet v23 key="tag"
    v33 = PropGet v23 key="size"
    v34 = Call v1, -, v24, v29, v31, v32, v33
    v35 = PropGet v23 key="label"
    v36 = Call v35, v23 calleeKey="label"
    v37 = InstanceOf v23, v8
    v38 = InstanceOf v23, v2
    v39 = PropGet v2 key="isArray"
    v40 = Call v39, v2, v23 calleeKey="isArray"
    v41 = Call v1, -, v36, v37, v38, v40
    v42 = PropGet v0 key="Object" [isFreeName]
    v43 = PropGet v42 key="keys"
    v44 = Call v43, v42, v23 calleeKey="keys"
    v45 = PropGet v44 key="length"
    v46 = Integer integer=2
    v47 = ElemGet v44, v46
    v48 = Integer integer=3
    v49 = ElemGet v44, v48
    v50 = Call v1, -, v45, v47, v49
    v51 = String string="a"
    v52 = Construct v15, -, v51
    v53 = PropGet v52 key="set"
    v54 = String string="x"
    v55 = Call v53, v52, v54, v26 calleeKey="set"
    v56 = PropGet v52 key="lookup"
    v57 = Call v56, v52, v54 calleeKey="lookup"
    v58 = Call v56, v52, v51 calleeKey="lookup"
    v59 = PropGet v52 key="hits"
    v60 = PropGet v52 key="name"
    v61 = PropGet v52 key="size"
    v62 = InstanceOf v52, v15
    v63 = InstanceOf v52, v9
    v64 = Call v1, -, v57, v58, v59, v60, v61, v62, v63
    v65 = Integer integer=4
    v66 = Construct v22, -, v65
    v67 = Integer integer=0
    v68 = Integer integer=300
    ElemSet v66, v67, v68 mode=Strict
    v70 = PropGet v66 key="next"
    v71 = Call v70, v66 calleeKey="next"
    v72 = Call v70, v66 calleeKey="next"
    v73 = PropGet v66 key="cursor"
    v74 = PropGet v66 key="length"
    v75 = InstanceOf v66, v22
    v76 = InstanceOf v66, v16
    v77 = Call v1, -, v71, v72, v73, v74, v75, v76
    v78 = NewArray contents=anything
    Jump -> next b1(v67)
  b1(v79):
    LoopHint
    v81 = Integer integer=2000
    v82 = Less v79, v81
    Branch v82 -> taken b2(), notTaken b3()
  b2():
    v83 = Construct v8
    v84 = PropGet v78 key="push"
    v85 = Call v84, v78, v83 calleeKey="push"
    v86 = Construct v15, -, v51
    v87 = Call v84, v78, v86 calleeKey="push"
    v88 = Construct v22, -, v65
    v89 = Call v84, v78, v88 calleeKey="push"
    v90 = Add v79, v30
    Jump -> next b1(v90)
  b3():
    v91 = PropGet v0 key="gc" [isFreeName]
    v92 = Call v91
    v93 = Integer integer=300
    v94 = ElemGet v78, v93
    v95 = PropGet v94 key="tag"
    v96 = PropGet v94 key="size"
    v97 = Integer integer=301
    v98 = ElemGet v78, v97
    v99 = PropGet v98 key="name"
    v100 = PropGet v98 key="hits"
    v101 = Integer integer=302
    v102 = ElemGet v78, v101
    v103 = PropGet v102 key="cursor"
    v104 = Call v1, -, v95, v96, v99, v100, v103
    Return
}
function f1 name="List" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] results=[{type=anything}] {
    v2 = GetSuperBase v0 homeKind=Object
    v3 = SuperConstruct v2, v1 layout=L0
    v4 = String string="t"
    PropSet v3, v4 key="tag" mode=Strict
    v6 = Double number=1.5
    PropSet v3, v6 key="size" mode=Strict
    Return v3
}
function f2 name="Registry" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] parameters=[{type=anything}] results=[{type=anything}] {
    v3 = GetSuperBase v0 homeKind=Object
    v4 = SuperConstruct v3, v1 layout=L1
    v5 = Integer integer=0
    PropDefine v4, v5 key="hits"
    PropSet v4, v2 key="name" mode=Strict
    Return v4
}
function f3 name="Bytes" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] parameters=[{type=anything}] results=[{type=anything}] {
    v3 = GetSuperBase v0 homeKind=Object
    v4 = SuperConstruct v3, v1, v2 layout=L2
    v5 = Integer integer=0
    PropDefine v4, v5 key="cursor"
    Return v4
}
function f4 name="label" kind=Method flags=[HasThis] results=[{type=anything}] {
    v1 = PropGet v0 key="tag"
    v2 = PropGet v0 key="length"
    v3 = Add v1, v2
    Return v3
}
function f5 name="lookup" kind=Method flags=[HasThis] parameters=[{type=anything}] results=[{type=anything}] {
    v2 = PropGet v0 key="hits"
    v3 = Integer integer=1
    v4 = Add v2, v3
    PropSet v0, v4 key="hits" mode=Strict
    v6 = PropGet v0 key="get"
    v7 = Call v6, v0, v1 calleeKey="get"
    Return v7
}
function f6 name="next" kind=Method flags=[HasThis] results=[{type=anything}] {
    v1 = PropGet v0 key="cursor"
    v2 = Integer integer=1
    v3 = Add v1, v2
    PropSet v0, v3 key="cursor" mode=Strict
    v5 = ElemGet v0, v1
    Return v5
}
)IR"_s;

// class Failure extends Error { constructor(m) { super(m, { cause: 17 }) } get message() { return "from getter" } }, class Stack extends Array { get length() { return 42 } }
static constexpr ASCIILiteral gettersUnderOwnNames = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Failure" constructor=f1 flags=[HasHeritage]
layout L1 kind=Instance name="Stack" constructor=f2 flags=[HasHeritage]
layout L2 name="Options" fields=[{key="cause" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = PropGet v0 key="Error" [isFreeName]
    v3 = NewClosure function=f1
    v4 = ResolveHeritage v2
    v5 = NewClass v3, v2, v4 layout=L0
    v6 = NewClosure function=f3
    DefineMethod v5, v6 layout=L0 methodKind=Getter key="message"
    v8 = FinishClass v5 layout=L0
    v9 = PropGet v0 key="Array" [isFreeName]
    v10 = NewClosure function=f2
    v11 = ResolveHeritage v9
    v12 = NewClass v10, v9, v11 layout=L1
    v13 = NewClosure function=f4
    DefineMethod v12, v13 layout=L1 methodKind=Getter key="length"
    v15 = FinishClass v12 layout=L1
    v16 = String string="given"
    v17 = Construct v8, -, v16
    v18 = PropGet v17 key="message"
    v19 = PropGet v17 key="cause"
    v20 = PropGet v17 key="stack"
    v21 = TypeOf v20
    v22 = Call v1, -, v18, v19, v21
    v23 = Construct v15
    v24 = PropGet v23 key="length"
    v25 = Call v1, -, v24
    Return
}
function f1 name="Failure" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] parameters=[{type=anything}] results=[{type=anything}] {
    v3 = GetSuperBase v0 homeKind=Object
    v4 = Integer integer=17
    v5 = NewObject -, v4 layout=L2 fields=[0]
    v6 = SuperConstruct v3, v1, v2, v5 layout=L0
    Return v6
}
function f2 name="Stack" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee] results=[{type=anything}] {
    v2 = GetSuperBase v0 homeKind=Object
    v3 = SuperConstruct v2, v1 layout=L1
    Return v3
}
function f3 name="message" kind=Getter flags=[HasThis] results=[{type=anything}] {
    v1 = String string="from getter"
    Return v1
}
function f4 name="length" kind=Getter flags=[HasThis] results=[{type=anything}] {
    v1 = Integer integer=42
    Return v1
}
)IR"_s;

// The shell has none of the host's classes, whose members flow takes for constant as soon as a name is read on a value nobody knows.
static void checkUnderEveryOption(ASCIILiteral programText, ASCIILiteral expectedOutput)
{
    static constexpr ASCIILiteral forShell = "disable=flow:ConstantBuiltinLocations"_s;
    checkOutputOfProgramInShell(programText, expectedOutput, { forShell, "references=narrow"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { forShell, "references=wide"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { forShell, "references=narrow"_s, "validate"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { forShell, "references=wide"_s, "validate"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { forShell, "heat=EverythingIsHot"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { forShell, "disable=pass:ResolveAccesses"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { forShell, "disable=pass:ResolveCalls"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { forShell, "disable=pass:SinkAllocations"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { forShell, "disable=pass:AllocateInFrames"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { forShell, "disable=pass:RemoveUnreadFields"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { forShell, "disable=flow-results"_s });
}

static void testErrorSubclasses()
{
    checkUnderEveryOption(errorSubclasses, R"(7 dboom 3
dboom:7 14
true true false
9 2 deep dboom2 18
true true true
4 code level
28000
)"_s);
}

static void testCollectionSubclasses()
{
    checkUnderEveryOption(collectionSubclasses, R"(0 2 5 t 1.5
t2 true true true
4 tag size
5 undefined 2 a 1 true true
44 0 2 4 true true
t 1.5 a 0 0
)"_s);
}

static void testGettersUnderOwnNames()
{
    checkUnderEveryOption(gettersUnderOwnNames, R"(given 17 string
0
)"_s);
}

std::span<const NativeTest> builtinSubclassTests()
{
    static constexpr NativeTest tests[] = {
        { "builtin-subclasses/fields-and-methods-of-error-subclasses"_s, testErrorSubclasses },
        { "builtin-subclasses/fields-and-methods-of-collection-subclasses"_s, testCollectionSubclasses },
        { "builtin-subclasses/own-properties-come-before-getters-of-the-class"_s, testGettersUnderOwnNames },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
