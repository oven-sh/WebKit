/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTPrivateNameProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral accessors = R"IR(layout L0 kind=Instance name="C" module=m0 constructor=f1 fields=[{key=#"#"@L0 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}, {key="v" declared={type=number flags=[IsExpected]} flags=[IsRequired]}]
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=classtoken} flags=[IsImmutable]
variable g2 module=m0 declared={type=anything} flags=[IsImmutable]
variable g3 module=m0 declared={type=anything} flags=[IsImmutable]
variable g4 module=m0 declared={type=anything} flags=[IsImmutable]
variable g5 module=m0 declared={type=anything} flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v0 = NewClassToken layout=L0
    PutModuleVariable v0 variable=g1
    v2 = NewClosure function=f1
    v3 = NewClass v2 layout=L0
    PutModuleVariable v3 variable=g0
    v5 = NewClosure function=f2
    PutModuleVariable v5 variable=g2
    v7 = NewClosure function=f3
    PutModuleVariable v7 variable=g3
    v9 = NewClosure function=f4
    PutModuleVariable v9 variable=g4
    v11 = NewClosure function=f5
    PutModuleVariable v11 variable=g5
    v13 = NewClosure function=f6
    DefineMethod v3, v13 layout=L0 key="read"
    v15 = NewClosure function=f7
    DefineMethod v3, v15 layout=L0 index=1 key="write"
    v17 = NewClosure function=f8
    DefineMethod v3, v17 layout=L0 index=2 key="increment"
    v19 = NewClosure function=f9
    DefineMethod v3, v19 layout=L0 index=3 key="add"
    v21 = NewClosure function=f10
    DefineMethod v3, v21 layout=L0 side=Static index=4 key="read"
    v23 = FinishClass v3 layout=L0
    v24 = Convert v23 declared={type=function flags=[IsExpected]}
    v25 = Convert v23 declared={type=function flags=[IsExpected]}
    v26 = Builtin builtin=GlobalThis
    v27 = PropGet v26 key="print" [isFreeName]
    v28 = Construct v25 calleeName="C"
    v29 = Convert v28 declared={type=L0 flags=[IsExpected]}
    v30 = PropGet v29 key="read"
    v31 = Convert v30 declared={type=function flags=[IsExpected]}
    v32 = Call v31, v29 calleeKey="read" calleeName="read"
    v33 = Convert v32 declared={type=number flags=[IsExpected]}
    v34 = Call v27, -, v33 calleeName="print"
    v35 = Builtin builtin=GlobalThis
    v36 = PropGet v35 key="print" [isFreeName]
    v37 = Construct v25 calleeName="C"
    v38 = Convert v37 declared={type=L0 flags=[IsExpected]}
    v39 = PropGet v38 key="write"
    v40 = Convert v39 declared={type=function flags=[IsExpected]}
    v41 = Integer integer=4
    v42 = Call v40, v38, v41 calleeKey="write" calleeName="write"
    v43 = Convert v42 declared={type=number flags=[IsExpected]}
    v44 = Call v36, -, v43 calleeName="print"
    v45 = Builtin builtin=GlobalThis
    v46 = PropGet v45 key="print" [isFreeName]
    v47 = Construct v25 calleeName="C"
    v48 = Convert v47 declared={type=L0 flags=[IsExpected]}
    v49 = PropGet v48 key="increment"
    v50 = Convert v49 declared={type=function flags=[IsExpected]}
    v51 = Call v50, v48 calleeKey="increment" calleeName="increment"
    v52 = Convert v51 declared={type=string flags=[IsExpected]}
    v53 = Call v46, -, v52 calleeName="print"
    v54 = Builtin builtin=GlobalThis
    v55 = PropGet v54 key="print" [isFreeName]
    v56 = Construct v25 calleeName="C"
    v57 = Convert v56 declared={type=L0 flags=[IsExpected]}
    v58 = PropGet v57 key="add"
    v59 = Convert v58 declared={type=function flags=[IsExpected]}
    v60 = Call v59, v57 calleeKey="add" calleeName="add"
    v61 = Convert v60 declared={type=string flags=[IsExpected]}
    v62 = Call v55, -, v61 calleeName="print"
    v63 = Builtin builtin=GlobalThis
    v64 = PropGet v63 key="print" [isFreeName]
    v65 = PropGet v25 key="read"
    v66 = Convert v65 declared={type=function flags=[IsExpected]}
    v67 = Construct v25 calleeName="C"
    v68 = Convert v67 declared={type=L0 flags=[IsExpected]}
    v69 = Call v66, v25, v68 calleeKey="read" calleeName="read"
    v70 = Call v64, -, v69 calleeName="print"
    v71 = Undefined
    Return v71
}
function f1 name="C" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Convert v2 declared={type=L0 flags=[IsFromCompiler]}
    v4 = CallDirect v3 function=f11
    Return v2
}
function f2 name="#g" module=m0 kind=Getter flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=number flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = PropGet v0 key="v"
    v2 = Convert v1 declared={type=number flags=[IsExpected]}
    v3 = Integer integer=100
    v4 = Add v2, v3
    v5 = Convert v4 declared={type=number flags=[IsExpected]}
    Return v5
}
function f3 name="#s" module=m0 kind=Setter flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = Integer integer=2
    v3 = Mul v1, v2
    v4 = Convert v3 declared={type=number flags=[IsExpected]}
    PropSet v0, v4 key="v"
    v6 = Undefined
    Return v6
}
function f4 name="#p" module=m0 kind=Getter flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=number flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = PropGet v0 key="v"
    v2 = Convert v1 declared={type=number flags=[IsExpected]}
    Return v2
}
function f5 name="#p" module=m0 kind=Setter flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=number flags=[IsExpected]}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = Convert v1 declared={type=number flags=[IsExpected]}
    PropSet v0, v2 key="v"
    v4 = Undefined
    Return v4
}
function f6 name="read" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=number flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0
    v3 = GetModuleVariable variable=g2
    v4 = Call v3, v0
    v5 = Convert v4 declared={type=number flags=[IsExpected]}
    Return v5
}
function f7 name="write" module=m0 kind=Method flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=number flags=[IsExpected]}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = GetModuleVariable variable=g1
    v3 = PrivateGet v0, v2 layout=L0
    v4 = GetModuleVariable variable=g3
    v5 = Call v4, v0, v1
    v6 = PropGet v0 key="v"
    v7 = Convert v6 declared={type=number flags=[IsExpected]}
    Return v7
}
function f8 name="increment" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=string flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0
    v3 = GetModuleVariable variable=g4
    v4 = Call v3, v0
    v5 = ToNumeric v4
    v6 = Increment v5
    v7 = GetModuleVariable variable=g1
    v8 = PrivateGet v0, v7 layout=L0
    v9 = GetModuleVariable variable=g5
    v10 = Call v9, v0, v6
    v11 = Convert v5 declared={type=number flags=[IsExpected]}
    v12 = String string=":"
    v13 = Add v11, v12
    v14 = PropGet v0 key="v"
    v15 = Convert v14 declared={type=number flags=[IsExpected]}
    v16 = Add v13, v15
    v17 = Convert v16 declared={type=string flags=[IsExpected]}
    Return v17
}
function f9 name="add" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=string flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0
    v3 = GetModuleVariable variable=g4
    v4 = Call v3, v0
    v5 = Integer integer=10
    v6 = Add v4, v5
    v7 = GetModuleVariable variable=g1
    v8 = PrivateGet v0, v7 layout=L0
    v9 = GetModuleVariable variable=g5
    v10 = Call v9, v0, v6
    v11 = String string=":"
    v12 = Add v6, v11
    v13 = PropGet v0 key="v"
    v14 = Convert v13 declared={type=number flags=[IsExpected]}
    v15 = Add v12, v14
    v16 = Convert v15 declared={type=string flags=[IsExpected]}
    Return v16
}
function f10 name="read" module=m0 kind=Method flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0
    v3 = GetModuleVariable variable=g2
    v4 = Call v3, v0
    Return v4
}
function f11 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L0 flags=[IsFromCompiler]} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    PrivateDefine v0, v1, v1 layout=L0
    v3 = Integer integer=1
    v4 = Convert v3 declared={type=number flags=[IsExpected]}
    PropDefine v0, v4 key="v"
    v6 = Undefined
    Return v6
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral storesThatThrow = R"IR(layout L0 kind=Instance name="C" module=m0 constructor=f1 fields=[{key=#"#"@L0 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}]
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=classtoken} flags=[IsImmutable]
variable g2 module=m0 declared={type=anything} flags=[IsImmutable]
variable g3 module=m0 declared={type=anything} flags=[IsImmutable]
variable g4 module=m0 declared={type=anything} flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v0 = NewClassToken layout=L0
    PutModuleVariable v0 variable=g1
    v2 = NewClosure function=f1
    v3 = NewClass v2 layout=L0
    PutModuleVariable v3 variable=g0
    v5 = NewClosure function=f2
    PutModuleVariable v5 variable=g2
    v7 = NewClosure function=f3
    PutModuleVariable v7 variable=g3
    v9 = NewClosure function=f4
    PutModuleVariable v9 variable=g4
    v11 = NewClosure function=f5
    DefineMethod v3, v11 layout=L0 key="run"
    v13 = FinishClass v3 layout=L0
    v14 = Convert v13 declared={type=function flags=[IsExpected]}
    v15 = Convert v13 declared={type=function flags=[IsExpected]}
    v16 = Construct v15 calleeName="C"
    v17 = Convert v16 declared={type=L0 flags=[IsExpected]}
    v18 = Integer
    v19 = Convert v18 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v19)
  b2(v20):
    LoopHint
    v22 = Integer integer=5
    v23 = Less v20, v22
    Branch v23 -> taken b3(| 0), notTaken b6(| 0)
  b3(v24):
    Jump -> next b4()
  b4():
    v25 = Builtin builtin=GlobalThis
    v26 = PropGet v25 key="print" [isFreeName]
    v27 = PropGet v17 key="run"
    v28 = Convert v27 declared={type=function flags=[IsExpected]}
    v29 = Call v28, v17, v24 calleeKey="run" calleeName="run"
    v30 = Call v26, -, v29 calleeName="print"
    Jump -> next b5()
  b5():
    v31 = Increment v24
    v32 = Convert v31 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v32)
  b6(v33):
    Jump -> next b7()
  b7():
    v34 = Undefined
    Return v34
}
function f1 name="C" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Convert v2 declared={type=L0 flags=[IsFromCompiler]}
    v4 = CallDirect v3 function=f6
    Return v2
}
function f2 name="#m" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=number flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v0 = Integer integer=1
    v1 = Convert v0 declared={type=number flags=[IsExpected]}
    Return v1
}
function f3 name="#g" module=m0 kind=Getter flags=[HasExplicitName] thisType={type=anything} results=[{type=number flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v0 = Integer integer=2
    v1 = Convert v0 declared={type=number flags=[IsExpected]}
    Return v1
}
function f4 name="#s" module=m0 kind=Setter flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v1 = Undefined
    Return v1
}
function f5 name="run" module=m0 kind=Method flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    Jump -> next b2()
  b2() handler=b22():
    v2 = Integer
    v3 = StrictEq v1, v2
    Branch v3 -> taken b3(| 0), notTaken b5(| 0)
  b3(v4) handler=b22():
    Jump -> next b4()
  b4() handler=b22():
    v5 = Integer integer=1
    v6 = GetModuleVariable variable=g1
    v7 = PrivateGet v0, v6 layout=L0
    v8 = NewRuntimeError runtimeError=PrivateMethodWrite
    Throw v8
  b5(v9) handler=b22():
    Jump -> next b6()
  b6() handler=b22():
    v10 = Integer integer=1
    v11 = StrictEq v9, v10
    Branch v11 -> taken b7(| 0), notTaken b9(| 0)
  b7(v12) handler=b22():
    Jump -> next b8()
  b8() handler=b22():
    v13 = Integer integer=1
    v14 = GetModuleVariable variable=g1
    v15 = PrivateGet v0, v14 layout=L0
    v16 = NewRuntimeError runtimeError=PrivateAccessorWithoutSetter
    Throw v16
  b9(v17) handler=b22():
    Jump -> next b10()
  b10() handler=b22():
    v18 = Integer integer=2
    v19 = StrictEq v17, v18
    Branch v19 -> taken b11(| 0), notTaken b13(| 0)
  b11(v20) handler=b22():
    Jump -> next b12()
  b12() handler=b22():
    v21 = GetModuleVariable variable=g1
    v22 = PrivateGet v0, v21 layout=L0
    v23 = NewRuntimeError runtimeError=PrivateAccessorWithoutGetter
    Throw v23
  b13(v24) handler=b22():
    Jump -> next b14()
  b14() handler=b22():
    v25 = Integer integer=3
    v26 = StrictEq v24, v25
    Branch v26 -> taken b15(| 0), notTaken b17(| 0)
  b15(v27) handler=b22():
    Jump -> next b16()
  b16() handler=b22():
    v28 = GetModuleVariable variable=g1
    v29 = PrivateGet v0, v28 layout=L0
    v30 = GetModuleVariable variable=g3
    v31 = Call v30, v0
    v32 = Increment v31
    v33 = GetModuleVariable variable=g1
    v34 = PrivateGet v0, v33 layout=L0
    v35 = NewRuntimeError runtimeError=PrivateAccessorWithoutSetter
    Throw v35
  b17(v36) handler=b22():
    Jump -> next b18()
  b18() handler=b22():
    Jump -> next b19()
  b19() handler=b22():
    Jump -> next b20()
  b20() handler=b22():
    Jump -> next b21()
  b21() handler=b22():
    v37 = GetModuleVariable variable=g1
    v38 = PrivateGet v0, v37 layout=L0
    v39 = GetModuleVariable variable=g2
    v40 = Call v39, v0 calleeName="#m"
    v41 = Convert v40 declared={type=number flags=[IsExpected]}
    v42 = GetModuleVariable variable=g1
    v43 = PrivateGet v0, v42 layout=L0
    v44 = GetModuleVariable variable=g3
    v45 = Call v44, v0
    v46 = Convert v45 declared={type=number flags=[IsExpected]}
    v47 = Add v41, v46
    Return v47
  b22() role=CatchHandler:
    v48 = CatchValue
    v49 = Builtin builtin=GlobalThis
    v50 = PropGet v49 key="TypeError" [isFreeName]
    v51 = Convert v50 declared={type=builtin(TypeError) flags=[IsExpected]}
    v52 = InstanceOf v48, v51
    Return v52
}
function f6 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L0 flags=[IsFromCompiler]} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    PrivateDefine v0, v1, v1 layout=L0
    v3 = Undefined
    Return v3
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral inOperator = R"IR(layout L0 kind=Instance name="C" module=m0 constructor=f1 fields=[{key=#"#"@L0 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}]
layout L1 kind=Instance name="D" module=m0 base=L0 constructor=f6 supertypes=[L0] flags=[HasHeritage]
layout L2 kind=Instance name="Other" module=m0 constructor=f7 fields=[{key=#"#"@L2 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}]
layout L3 module=m0
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=classtoken} flags=[IsImmutable]
variable g2 module=m0 declared={type=anything} flags=[IsImmutable]
variable g3 module=m0 declared={type=anything} flags=[IsImmutable]
variable g4 module=m0 declared={type=function} flags=[IsImmutable]
variable g5 module=m0 declared={type=function} flags=[IsImmutable]
variable g6 module=m0 declared={type=classtoken} flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v0 = NewClassToken layout=L0
    PutModuleVariable v0 variable=g1
    v2 = NewClosure function=f1
    v3 = NewClass v2 layout=L0
    PutModuleVariable v3 variable=g0
    v5 = NewClosure function=f2
    PutModuleVariable v5 variable=g2
    v7 = NewClosure function=f3
    PutModuleVariable v7 variable=g3
    v9 = NewClosure function=f4
    DefineMethod v3, v9 layout=L0 side=Static key="has"
    v11 = NewClosure function=f5
    DefineMethod v3, v11 layout=L0 side=Static index=1 key="hasGetter"
    v13 = FinishClass v3 layout=L0
    v14 = Convert v13 declared={type=function flags=[IsExpected]}
    v15 = Convert v13 declared={type=function flags=[IsExpected]}
    v16 = ResolveHeritage v15
    v17 = NewClosure function=f6
    v18 = NewClass v17, v15, v16 layout=L1
    PutModuleVariable v18 variable=g4
    v20 = FinishClass v18 layout=L1
    v21 = Convert v20 declared={type=function flags=[IsExpected]}
    v22 = Convert v20 declared={type=function flags=[IsExpected]}
    v23 = NewClassToken layout=L2
    PutModuleVariable v23 variable=g6
    v25 = NewClosure function=f7
    v26 = NewClass v25 layout=L2
    PutModuleVariable v26 variable=g5
    v28 = NewClosure function=f8
    v29 = FinishClass v26 layout=L2
    v30 = Convert v29 declared={type=function flags=[IsExpected]}
    v31 = Convert v29 declared={type=function flags=[IsExpected]}
    v32 = Builtin builtin=GlobalThis
    v33 = PropGet v32 key="print" [isFreeName]
    v34 = PropGet v15 key="has"
    v35 = Convert v34 declared={type=function flags=[IsExpected]}
    v36 = Construct v15 calleeName="C"
    v37 = Convert v36 declared={type=L0 flags=[IsExpected]}
    v38 = Call v35, v15, v37 calleeKey="has" calleeName="has"
    v39 = Convert v38 declared={type=boolean flags=[IsExpected]}
    v40 = Call v33, -, v39 calleeName="print"
    v41 = Builtin builtin=GlobalThis
    v42 = PropGet v41 key="print" [isFreeName]
    v43 = PropGet v15 key="hasGetter"
    v44 = Convert v43 declared={type=function flags=[IsExpected]}
    v45 = Construct v15 calleeName="C"
    v46 = Convert v45 declared={type=L0 flags=[IsExpected]}
    v47 = Call v44, v15, v46 calleeKey="hasGetter" calleeName="hasGetter"
    v48 = Convert v47 declared={type=boolean flags=[IsExpected]}
    v49 = Call v42, -, v48 calleeName="print"
    v50 = Builtin builtin=GlobalThis
    v51 = PropGet v50 key="print" [isFreeName]
    v52 = PropGet v15 key="has"
    v53 = Convert v52 declared={type=function flags=[IsExpected]}
    v54 = Construct v22 calleeName="D"
    v55 = Convert v54 declared={type=L1 flags=[IsExpected]}
    v56 = Call v53, v15, v55 calleeKey="has" calleeName="has"
    v57 = Convert v56 declared={type=boolean flags=[IsExpected]}
    v58 = Call v51, -, v57 calleeName="print"
    v59 = Builtin builtin=GlobalThis
    v60 = PropGet v59 key="print" [isFreeName]
    v61 = PropGet v15 key="has"
    v62 = Convert v61 declared={type=function flags=[IsExpected]}
    v63 = Data data=d0
    v64 = Call v62, v15, v63 calleeKey="has" calleeName="has"
    v65 = Convert v64 declared={type=boolean flags=[IsExpected]}
    v66 = Call v60, -, v65 calleeName="print"
    v67 = Builtin builtin=GlobalThis
    v68 = PropGet v67 key="print" [isFreeName]
    v69 = PropGet v15 key="has"
    v70 = Convert v69 declared={type=function flags=[IsExpected]}
    v71 = Construct v31 calleeName="Other"
    v72 = Convert v71 declared={type=L2 flags=[IsExpected]}
    v73 = Call v70, v15, v72 calleeKey="has" calleeName="has"
    v74 = Convert v73 declared={type=boolean flags=[IsExpected]}
    v75 = Call v68, -, v74 calleeName="print"
    v76 = Builtin builtin=GlobalThis
    v77 = PropGet v76 key="print" [isFreeName]
    v78 = PropGet v15 key="has"
    v79 = Convert v78 declared={type=function flags=[IsExpected]}
    v80 = Call v79, v15, v15 calleeKey="has" calleeName="has"
    v81 = Convert v80 declared={type=boolean flags=[IsExpected]}
    v82 = Call v77, -, v81 calleeName="print"
    Jump -> next b2()
  b2() handler=b4():
    v83 = PropGet v15 key="has"
    v84 = Convert v83 declared={type=function flags=[IsExpected]}
    v85 = Integer integer=1
    v86 = Call v84, v15, v85 calleeKey="has" calleeName="has"
    Jump -> next b3()
  b3():
    Jump -> next b5()
  b4() role=CatchHandler:
    v87 = CatchValue
    v88 = Builtin builtin=GlobalThis
    v89 = PropGet v88 key="print" [isFreeName]
    v90 = Builtin builtin=GlobalThis
    v91 = PropGet v90 key="TypeError" [isFreeName]
    v92 = Convert v91 declared={type=builtin(TypeError) flags=[IsExpected]}
    v93 = InstanceOf v87, v92
    v94 = Call v89, -, v93 calleeName="print"
    Jump -> next b5()
  b5():
    v95 = Undefined
    Return v95
}
function f1 name="C" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Convert v2 declared={type=L0 flags=[IsFromCompiler]}
    v4 = CallDirect v3 function=f9
    Return v2
}
function f2 name="#m" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v0 = Undefined
    Return v0
}
function f3 name="#g" module=m0 kind=Getter flags=[HasExplicitName] thisType={type=anything} results=[{type=number flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v0 = Integer integer=1
    v1 = Convert v0 declared={type=number flags=[IsExpected]}
    Return v1
}
function f4 name="has" module=m0 kind=Method flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=boolean flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateHas v0, v1 layout=L0
    v3 = Convert v2 declared={type=boolean flags=[IsExpected]}
    Return v3
}
function f5 name="hasGetter" module=m0 kind=Method flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=boolean flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateHas v0, v1 layout=L0
    v3 = Convert v2 declared={type=boolean flags=[IsExpected]}
    Return v3
}
function f6 name="D" module=m0 kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee, HasExplicitName] thisType={type=anything} parameters=[{type=Array}] results=[{type=object}] {
    Jump -> next b1()
  b1():
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L1
    Return v4
}
function f7 name="Other" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L2
    v2 = NewObject v1 layout=L2
    v3 = Convert v2 declared={type=L2 flags=[IsFromCompiler]}
    v4 = CallDirect v3 function=f10
    Return v2
}
function f8 name="#m" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v0 = Undefined
    Return v0
}
function f9 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L0 flags=[IsFromCompiler]} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    PrivateDefine v0, v1, v1 layout=L0
    v3 = Undefined
    Return v3
}
function f10 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L2 flags=[IsFromCompiler]} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g6
    PrivateDefine v0, v1, v1 layout=L2
    v3 = Undefined
    Return v3
}
data d0 root=Object{layout=L3} module=m0
entry module=m0
)IR"_s;

static constexpr ASCIILiteral classEvaluatedTwice = R"IR(layout L0 kind=Instance module=m0 constructor=f2 fields=[{key=#"#"@L0 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = Convert v0 declared={type=function flags=[IsExpected]}
    v2 = Convert v1 declared={type=function flags=[IsExpected]}
    v3 = String string="a"
    v4 = Call v2, -, v3 calleeName="make"
    v5 = Convert v4 declared={type=function flags=[IsExpected]}
    v6 = String string="b"
    v7 = Call v2, -, v6 calleeName="make"
    v8 = Convert v7 declared={type=function flags=[IsExpected]}
    v9 = Construct v5 calleeName="A"
    v10 = Convert v9 declared={type=L0 flags=[IsExpected]}
    v11 = Construct v8 calleeName="B"
    v12 = Convert v11 declared={type=L0 flags=[IsExpected]}
    v13 = Builtin builtin=GlobalThis
    v14 = PropGet v13 key="print" [isFreeName]
    v15 = PropGet v5 key="invoke"
    v16 = Convert v15 declared={type=function flags=[IsExpected]}
    v17 = Call v16, v5, v10 calleeKey="invoke" calleeName="invoke"
    v18 = Call v14, -, v17 calleeName="print"
    v19 = Builtin builtin=GlobalThis
    v20 = PropGet v19 key="print" [isFreeName]
    v21 = PropGet v8 key="invoke"
    v22 = Convert v21 declared={type=function flags=[IsExpected]}
    v23 = Call v22, v8, v12 calleeKey="invoke" calleeName="invoke"
    v24 = Call v20, -, v23 calleeName="print"
    v25 = Builtin builtin=GlobalThis
    v26 = PropGet v25 key="print" [isFreeName]
    v27 = PropGet v5 key="invoke"
    v28 = Convert v27 declared={type=function flags=[IsExpected]}
    v29 = Call v28, v5, v12 calleeKey="invoke" calleeName="invoke"
    v30 = Call v26, -, v29 calleeName="print"
    v31 = Builtin builtin=GlobalThis
    v32 = PropGet v31 key="print" [isFreeName]
    v33 = PropGet v8 key="invoke"
    v34 = Convert v33 declared={type=function flags=[IsExpected]}
    v35 = Call v34, v8, v10 calleeKey="invoke" calleeName="invoke"
    v36 = Call v32, -, v35 calleeName="print"
    v37 = Builtin builtin=GlobalThis
    v38 = PropGet v37 key="print" [isFreeName]
    v39 = PropGet v5 key="read"
    v40 = Convert v39 declared={type=function flags=[IsExpected]}
    v41 = Call v40, v5, v10 calleeKey="read" calleeName="read"
    v42 = Call v38, -, v41 calleeName="print"
    v43 = Builtin builtin=GlobalThis
    v44 = PropGet v43 key="print" [isFreeName]
    v45 = PropGet v5 key="read"
    v46 = Convert v45 declared={type=function flags=[IsExpected]}
    v47 = Call v46, v5, v12 calleeKey="read" calleeName="read"
    v48 = Call v44, -, v47 calleeName="print"
    v49 = Builtin builtin=GlobalThis
    v50 = PropGet v49 key="print" [isFreeName]
    v51 = PropGet v5 key="has"
    v52 = Convert v51 declared={type=function flags=[IsExpected]}
    v53 = Call v52, v5, v10 calleeKey="has" calleeName="has"
    v54 = Convert v53 declared={type=boolean flags=[IsExpected]}
    v55 = Call v50, -, v54 calleeName="print"
    v56 = Builtin builtin=GlobalThis
    v57 = PropGet v56 key="print" [isFreeName]
    v58 = PropGet v5 key="has"
    v59 = Convert v58 declared={type=function flags=[IsExpected]}
    v60 = Call v59, v5, v12 calleeKey="has" calleeName="has"
    v61 = Convert v60 declared={type=boolean flags=[IsExpected]}
    v62 = Call v57, -, v61 calleeName="print"
    v63 = Builtin builtin=GlobalThis
    v64 = PropGet v63 key="print" [isFreeName]
    v65 = PropGet v8 key="has"
    v66 = Convert v65 declared={type=function flags=[IsExpected]}
    v67 = Call v66, v8, v10 calleeKey="has" calleeName="has"
    v68 = Convert v67 declared={type=boolean flags=[IsExpected]}
    v69 = Call v64, -, v68 calleeName="print"
    v70 = Builtin builtin=GlobalThis
    v71 = PropGet v70 key="print" [isFreeName]
    v72 = PropGet v8 key="has"
    v73 = Convert v72 declared={type=function flags=[IsExpected]}
    v74 = Call v73, v8, v12 calleeKey="has" calleeName="has"
    v75 = Convert v74 declared={type=boolean flags=[IsExpected]}
    v76 = Call v71, -, v75 calleeName="print"
    v77 = Undefined
    Return v77
}
function f1 name="make" module=m0 flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=function flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v1 = NewClassToken layout=L0
    v2 = NewClosure v1 function=f2
    v3 = NewClass v2 layout=L0
    v4 = NewClosure v0 function=f3
    v5 = NewClosure v0 function=f4
    v6 = NewClosure v1, v4 function=f5
    DefineMethod v3, v6 layout=L0 side=Static key="invoke"
    v8 = NewClosure v1, v5 function=f6
    DefineMethod v3, v8 layout=L0 side=Static index=1 key="read"
    v10 = NewClosure v1, v4 function=f7
    DefineMethod v3, v10 layout=L0 side=Static index=2 key="has"
    v12 = FinishClass v3 layout=L0
    v13 = Convert v12 declared={type=function flags=[IsExpected]}
    Return v13
}
function f2 module=m0 kind=BaseConstructor flags=[HasNewTarget] captures=[{type=classtoken}] thisType={type=anything} results=[{type=object}] {
    Jump -> next b1()
  b1():
    v2 = GetConstructPrototype v1 layout=L0
    v3 = NewObject v2 layout=L0
    v4 = Convert v3 declared={type=L0 flags=[IsFromCompiler]}
    v5 = CallDirect v0, v4 function=f8
    Return v3
}
function f3 name="#m" module=m0 kind=Method flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    Return v0
}
function f4 name="#g" module=m0 kind=Getter flags=[HasExplicitName] captures=[{type=anything}] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v1 = String string="get "
    v2 = Add v1, v0
    v3 = Convert v2 declared={type=string flags=[IsExpected]}
    Return v3
}
function f5 name="invoke" module=m0 kind=Method flags=[HasExplicitName] length=1 captures=[{type=classtoken}, {type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1():
    Jump -> next b2()
  b2() handler=b3():
    v3 = PrivateGet v2, v0 layout=L0
    v4 = Call v1, v2 calleeName="#m"
    Return v4
  b3() role=CatchHandler:
    v5 = CatchValue
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="TypeError" [isFreeName]
    v8 = Convert v7 declared={type=builtin(TypeError) flags=[IsExpected]}
    v9 = InstanceOf v5, v8
    Return v9
}
function f6 name="read" module=m0 kind=Method flags=[HasExplicitName] length=1 captures=[{type=classtoken}, {type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    Jump -> next b1()
  b1():
    Jump -> next b2()
  b2() handler=b3():
    v3 = PrivateGet v2, v0 layout=L0
    v4 = Call v1, v2
    Return v4
  b3() role=CatchHandler:
    v5 = CatchValue
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="TypeError" [isFreeName]
    v8 = Convert v7 declared={type=builtin(TypeError) flags=[IsExpected]}
    v9 = InstanceOf v5, v8
    Return v9
}
function f7 name="has" module=m0 kind=Method flags=[HasExplicitName] length=1 captures=[{type=classtoken}, {type=anything}] thisType={type=anything} parameters=[{type=anything}] results=[{type=boolean flags=[IsExpected]}] {
    Jump -> next b1()
  b1():
    v3 = PrivateHas v2, v0 layout=L0
    v4 = Convert v3 declared={type=boolean flags=[IsExpected]}
    Return v4
}
function f8 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] captures=[{type=classtoken}] thisType={type=L0 flags=[IsFromCompiler]} results=[{type=anything}] {
    Jump -> next b1()
  b1():
    PrivateDefine v1, v0, v0 layout=L0
    v3 = Undefined
    Return v3
}
entry module=m0
)IR"_s;

static void testAccessors()
{
    checkPrivateNameProgram(accessors, "101\n8\n1:2\n11:11\n101\n"_s);
}

static void testStoresThatThrow()
{
    checkPrivateNameProgram(storesThatThrow, "true\ntrue\ntrue\ntrue\n3\n"_s);
}

static void testInOperator()
{
    checkPrivateNameProgram(inOperator, "true\ntrue\ntrue\nfalse\nfalse\nfalse\ntrue\n"_s);
}

static void testClassEvaluatedTwice()
{
    checkPrivateNameProgram(classEvaluatedTwice, "a\nb\ntrue\ntrue\nget a\ntrue\ntrue\nfalse\nfalse\ntrue\n"_s);
}

std::span<const NativeTest> privateAccessorProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "private-accessor-programs/accessors"_s, testAccessors },
        { "private-accessor-programs/stores-that-throw"_s, testStoresThatThrow },
        { "private-accessor-programs/in-operator"_s, testInOperator },
        { "private-accessor-programs/class-evaluated-twice"_s, testClassEvaluatedTwice },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
