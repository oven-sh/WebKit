/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTPrivateNameProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral receiversWithoutBrand = R"IR(layout L0 kind=Instance name="C" module=m0 constructor=f1 fields=[{key=#"#"@L0 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}]
layout L1 kind=Instance name="Other" module=m0 constructor=f8 fields=[{key=#"#"@L1 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}]
layout L2 module=m0
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=classtoken} flags=[IsImmutable]
variable g2 module=m0 declared={type=anything} flags=[IsImmutable]
variable g3 module=m0 declared={type=anything} flags=[IsImmutable]
variable g4 module=m0 declared={type=anything} flags=[IsImmutable]
variable g5 module=m0 declared={type=function} flags=[IsImmutable]
variable g6 module=m0 declared={type=classtoken} flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
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
    DefineMethod v3, v11 layout=L0 side=Static key="invoke"
    v13 = NewClosure function=f6
    DefineMethod v3, v13 layout=L0 side=Static index=1 key="read"
    v15 = NewClosure function=f7
    DefineMethod v3, v15 layout=L0 side=Static index=2 key="write"
    v17 = FinishClass v3 layout=L0
    v18 = Convert v17 declared={type=function flags=[IsExpected]}
    v19 = Convert v17 declared={type=function flags=[IsExpected]}
    v20 = NewClassToken layout=L1
    PutModuleVariable v20 variable=g6
    v22 = NewClosure function=f8
    v23 = NewClass v22 layout=L1
    PutModuleVariable v23 variable=g5
    v25 = NewClosure function=f9
    v26 = FinishClass v23 layout=L1
    v27 = Convert v26 declared={type=function flags=[IsExpected]}
    v28 = Convert v26 declared={type=function flags=[IsExpected]}
    v29 = Data data=d0
    v30 = Construct v28 calleeName="Other"
    v31 = Convert v30 declared={type=L1 flags=[IsExpected]}
    v32 = Builtin builtin=GlobalThis
    v33 = PropGet v32 key="Object" [isFreeName]
    v34 = Convert v33 declared={type=builtin(Object) flags=[IsExpected]}
    v35 = PropGet v34 key="create"
    v36 = Convert v35 declared={type=function flags=[IsExpected]}
    v37 = Convert v36 declared={type=builtin(ObjectCreate) flags=[IsExpected]}
    v38 = Construct v19 calleeName="C"
    v39 = Convert v38 declared={type=L0 flags=[IsExpected]}
    v40 = Call v37, v34, v39 calleeKey="create" calleeName="create"
    v41 = Integer integer=1
    v42 = Null
    v43 = Undefined
    v44 = Convert v43 declared={type=undefined flags=[IsExpected]}
    v45 = Construct v19 calleeName="C"
    v46 = Convert v45 declared={type=L0 flags=[IsExpected]}
    v47 = NewArray v29, v31, v19, v40, v41, v42, v44, v46 contents=anything
    v48 = Convert v47 declared={type=Array flags=[IsExpected]}
    v49 = Integer
    v50 = Convert v49 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v50)
  b2(v51):
    LoopHint
    v53 = PropGet v48 key="length"
    v54 = Convert v53 declared={type=number flags=[IsExpected]}
    v55 = Less v51, v54
    Branch v55 -> taken b3(| 0), notTaken b6(| 0)
  b3(v56):
    Jump -> next b4()
  b4():
    v57 = Builtin builtin=GlobalThis
    v58 = PropGet v57 key="print" [isFreeName]
    v59 = PropGet v19 key="invoke"
    v60 = Convert v59 declared={type=function flags=[IsExpected]}
    v61 = ElemGet v48, v56
    v62 = Call v60, v19, v61 calleeKey="invoke" calleeName="invoke"
    v63 = String string=" "
    v64 = Add v62, v63
    v65 = PropGet v19 key="read"
    v66 = Convert v65 declared={type=function flags=[IsExpected]}
    v67 = ElemGet v48, v56
    v68 = Call v66, v19, v67 calleeKey="read" calleeName="read"
    v69 = Add v64, v68
    v70 = String string=" "
    v71 = Add v69, v70
    v72 = PropGet v19 key="write"
    v73 = Convert v72 declared={type=function flags=[IsExpected]}
    v74 = ElemGet v48, v56
    v75 = Call v73, v19, v74 calleeKey="write" calleeName="write"
    v76 = Convert v75 declared={type=boolean|"stored" flags=[IsExpected]}
    v77 = Add v71, v76
    v78 = Call v58, -, v77 calleeName="print"
    Jump -> next b5()
  b5():
    v79 = Increment v56
    v80 = Convert v79 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v80)
  b6(v81):
    Jump -> next b7()
  b7():
    v82 = Undefined
    Return v82
}
function f1 name="C" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Convert v2 declared={type=L0 flags=[IsFromCompiler]} illTypedCause=FromLooseType
    v4 = CallDirect v3 function=f10
    Return v2
}
function f2 name="#m" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = String string="called"
    v1 = Convert v0 declared={type=string flags=[IsExpected]}
    Return v1
}
function f3 name="#g" module=m0 kind=Getter flags=[HasExplicitName] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = String string="got"
    v1 = Convert v0 declared={type=string flags=[IsExpected]}
    Return v1
}
function f4 name="#s" module=m0 kind=Setter flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = Undefined
    Return v1
}
function f5 name="invoke" module=m0 kind=Method flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    Jump -> next b2()
  b2() handler=b3():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0
    v3 = GetModuleVariable variable=g2
    v4 = Call v3, v0 calleeName="#m"
    Return v4
  b3() role=CatchHandler:
    v5 = CatchValue
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="TypeError" [isFreeName]
    v8 = Convert v7 declared={type=builtin(TypeError) flags=[IsExpected]}
    v9 = InstanceOf v5, v8
    Return v9
}
function f6 name="read" module=m0 kind=Method flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    Jump -> next b2()
  b2() handler=b3():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0
    v3 = GetModuleVariable variable=g3
    v4 = Call v3, v0
    Return v4
  b3() role=CatchHandler:
    v5 = CatchValue
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="TypeError" [isFreeName]
    v8 = Convert v7 declared={type=builtin(TypeError) flags=[IsExpected]}
    v9 = InstanceOf v5, v8
    Return v9
}
function f7 name="write" module=m0 kind=Method flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=boolean|"stored" flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    Jump -> next b2()
  b2() handler=b3():
    v1 = Integer integer=1
    v2 = GetModuleVariable variable=g1
    v3 = PrivateGet v0, v2 layout=L0
    v4 = GetModuleVariable variable=g4
    v5 = Call v4, v0, v1
    v6 = String string="stored"
    v7 = Convert v6 declared={type=boolean|"stored" flags=[IsExpected]}
    Return v7
  b3() role=CatchHandler:
    v8 = CatchValue
    v9 = Builtin builtin=GlobalThis
    v10 = PropGet v9 key="TypeError" [isFreeName]
    v11 = Convert v10 declared={type=builtin(TypeError) flags=[IsExpected]}
    v12 = InstanceOf v8, v11
    v13 = Convert v12 declared={type=boolean|"stored" flags=[IsExpected]}
    Return v13
}
function f8 name="Other" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L1
    v2 = NewObject v1 layout=L1
    v3 = Convert v2 declared={type=L1 flags=[IsFromCompiler]} illTypedCause=FromLooseType
    v4 = CallDirect v3 function=f11
    Return v2
}
function f9 name="#m" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = String string="other"
    v1 = Convert v0 declared={type=string flags=[IsExpected]}
    Return v1
}
function f10 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L0 flags=[IsFromCompiler]} results=[{type=anything}] {
  b0(v0: L0):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    PrivateDefine v0, v1, v1 layout=L0
    v3 = Undefined
    Return v3
}
function f11 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L1 flags=[IsFromCompiler]} results=[{type=anything}] {
  b0(v0: L1):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g6
    PrivateDefine v0, v1, v1 layout=L1
    v3 = Undefined
    Return v3
}
data d0 root=Object{layout=L2} module=m0
entry module=m0
)IR"_s;

static constexpr ASCIILiteral beforeSuperReturns = R"IR(layout L0 kind=Instance name="Base" module=m0 constructor=f1 fields=[{key="early" declared={type=anything}}]
layout L1 kind=Instance name="Derived" module=m0 base=L0 constructor=f2 fields=[{key=#"#"@L1 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}, {key="late" declared={type=string flags=[IsExpected]}}] supertypes=[L0] flags=[HasHeritage]
layout L2 kind=Instance name="Tested" module=m0 base=L0 constructor=f6 fields=[{key=#"#"@L2 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}] supertypes=[L0] flags=[HasHeritage]
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=function} flags=[IsImmutable]
variable g2 module=m0 declared={type=classtoken} flags=[IsImmutable]
variable g3 module=m0 declared={type=anything} flags=[IsImmutable]
variable g4 module=m0 declared={type=function} flags=[IsImmutable]
variable g5 module=m0 declared={type=classtoken} flags=[IsImmutable]
variable g6 module=m0 declared={type=anything} flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = NewClass v0 layout=L0
    PutModuleVariable v1 variable=g0
    v3 = FinishClass v1 layout=L0
    v4 = Convert v3 declared={type=function flags=[IsExpected]}
    v5 = Convert v3 declared={type=function flags=[IsExpected]}
    v6 = ResolveHeritage v5
    v7 = NewClassToken layout=L1
    PutModuleVariable v7 variable=g2
    v9 = NewClosure function=f2
    v10 = NewClass v9, v5, v6 layout=L1
    PutModuleVariable v10 variable=g1
    v12 = NewClosure function=f3
    PutModuleVariable v12 variable=g3
    v14 = NewClosure function=f4
    DefineMethod v10, v14 layout=L1 key="hook"
    v16 = NewClosure function=f5
    DefineMethod v10, v16 layout=L1 index=1 key="has"
    v18 = FinishClass v10 layout=L1
    v19 = Convert v18 declared={type=function flags=[IsExpected]}
    v20 = Convert v18 declared={type=function flags=[IsExpected]}
    v21 = ResolveHeritage v5
    v22 = NewClassToken layout=L2
    PutModuleVariable v22 variable=g5
    v24 = NewClosure function=f6
    v25 = NewClass v24, v5, v21 layout=L2
    PutModuleVariable v25 variable=g4
    v27 = NewClosure function=f7
    PutModuleVariable v27 variable=g6
    v29 = NewClosure function=f8
    DefineMethod v25, v29 layout=L2 key="hook"
    v31 = FinishClass v25 layout=L2
    v32 = Convert v31 declared={type=function flags=[IsExpected]}
    v33 = Convert v31 declared={type=function flags=[IsExpected]}
    v34 = Construct v20 calleeName="Derived"
    v35 = Convert v34 declared={type=L1 flags=[IsExpected]}
    v36 = Construct v33 calleeName="Tested"
    v37 = Convert v36 declared={type=L2 flags=[IsExpected]}
    v38 = Builtin builtin=GlobalThis
    v39 = PropGet v38 key="print" [isFreeName]
    v40 = PropGet v35 key="early"
    v41 = Call v39, -, v40 calleeName="print"
    v42 = Builtin builtin=GlobalThis
    v43 = PropGet v42 key="print" [isFreeName]
    v44 = PropGet v35 key="late"
    v45 = Convert v44 declared={type=string flags=[IsExpected]}
    v46 = Call v43, -, v45 calleeName="print"
    v47 = Builtin builtin=GlobalThis
    v48 = PropGet v47 key="print" [isFreeName]
    v49 = PropGet v35 key="hook"
    v50 = Convert v49 declared={type=function flags=[IsExpected]}
    v51 = Call v50, v35 calleeKey="hook" calleeName="hook"
    v52 = Convert v51 declared={type=boolean|string flags=[IsExpected]}
    v53 = Call v48, -, v52 calleeName="print"
    v54 = Builtin builtin=GlobalThis
    v55 = PropGet v54 key="print" [isFreeName]
    v56 = PropGet v37 key="early"
    v57 = Call v55, -, v56 calleeName="print"
    v58 = Builtin builtin=GlobalThis
    v59 = PropGet v58 key="print" [isFreeName]
    v60 = PropGet v37 key="hook"
    v61 = Convert v60 declared={type=function flags=[IsExpected]}
    v62 = Call v61, v37 calleeKey="hook" calleeName="hook"
    v63 = Convert v62 declared={type=boolean flags=[IsExpected]}
    v64 = Call v59, -, v63 calleeName="print"
    v65 = Undefined
    Return v65
}
function f1 name="Base" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Convert v2 declared={type=L0 flags=[IsExpected]}
    v4 = Convert v2 declared={type=L0 flags=[IsExpected]}
    v5 = PropGet v4 key="hook"
    v6 = Call v5, v4 calleeKey="hook" calleeName="hook"
    PropSet v3, v6 key="early"
    v8 = Undefined
    v9 = TypeTest v8 testedType=object
    Branch v9 -> taken b2(| 0), notTaken b3(| 0)
  b2(v10):
    Jump -> next b4(v10)
  b3(v11):
    Jump -> next b4(v2)
  b4(v12):
    Return v12
}
function f2 name="Derived" module=m0 kind=DerivedConstructor flags=[HasNewTarget, TakesCallee, HasExplicitName] thisType={type=anything} results=[{type=object}] {
  b0(v0: function, v1: anything):
    Jump -> next b1()
  b1():
    v2 = Absent
    v3 = GetSuperBase v0
    v4 = SuperConstruct v3, v1 layout=L1
    v5 = TypeTest v2 testedType=absent
    Branch v5 -> taken b2(| 0), notTaken b14(| 0)
  b2(v6):
    v7 = Convert v4 declared={type=L1 flags=[IsFromCompiler]} illTypedCause=FromLooseType
    v8 = CallDirect v7 function=f9
    v9 = TypeTest v4 testedType=absent
    Branch v9 -> taken b3(| 0), notTaken b4(| 0)
  b3(v10) frequency=Unlikely:
    v11 = NewRuntimeError runtimeError=UninitializedThis
    Throw v11
  b4(v12):
    v13 = Convert v12 declared={type=L1 flags=[IsExpected]}
    v14 = TypeTest v12 testedType=absent
    Branch v14 -> taken b5(| 0), notTaken b6(| 0)
  b5(v15) frequency=Unlikely:
    v16 = NewRuntimeError runtimeError=UninitializedThis
    Throw v16
  b6(v17):
    v18 = Convert v17 declared={type=L1 flags=[IsExpected]}
    v19 = GetModuleVariable variable=g2
    v20 = PrivateGet v18, v19 layout=L1
    v21 = GetModuleVariable variable=g3
    v22 = Call v21, v18 calleeName="#m"
    v23 = Convert v22 declared={type=string flags=[IsExpected]}
    PropSet v13, v23 key="late"
    v25 = Undefined
    v26 = TypeTest v25 testedType=object
    Branch v26 -> taken b7(| 0), notTaken b8(| 0)
  b7(v27):
    Jump -> next b12(v27)
  b8(v28):
    v29 = TypeTest v28 testedType=undefined
    Branch v29 -> taken b9(| 0), notTaken b13(| 0)
  b9(v30):
    v31 = TypeTest v17 testedType=absent
    Branch v31 -> taken b10(| 0), notTaken b11(| 0)
  b10(v32) frequency=Unlikely:
    v33 = NewRuntimeError runtimeError=UninitializedThis
    Throw v33
  b11(v34):
    Jump -> next b12(v34)
  b12(v35):
    Return v35
  b13(v36) frequency=Unlikely:
    v37 = NewRuntimeError runtimeError=DerivedConstructorResult name="Derived"
    Throw v37
  b14(v38) frequency=Unlikely:
    v39 = NewRuntimeError runtimeError=ThisAlreadyBound
    Throw v39
}
function f3 name="#m" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = String string="private"
    v1 = Convert v0 declared={type=string flags=[IsExpected]}
    Return v1
}
function f4 name="hook" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L1 flags=[IsExpected]} results=[{type=boolean|string flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    Jump -> next b2()
  b2() handler=b3():
    v1 = GetModuleVariable variable=g2
    v2 = PrivateGet v0, v1 layout=L1
    v3 = GetModuleVariable variable=g3
    v4 = Call v3, v0 calleeName="#m"
    v5 = Convert v4 declared={type=string flags=[IsExpected]}
    v6 = Convert v5 declared={type=boolean|string flags=[IsExpected]}
    Return v6
  b3() role=CatchHandler:
    v7 = CatchValue
    v8 = Builtin builtin=GlobalThis
    v9 = PropGet v8 key="TypeError" [isFreeName]
    v10 = Convert v9 declared={type=builtin(TypeError) flags=[IsExpected]}
    v11 = InstanceOf v7, v10
    v12 = Convert v11 declared={type=boolean|string flags=[IsExpected]}
    Return v12
}
function f5 name="has" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L1 flags=[IsExpected]} results=[{type=boolean flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g2
    v2 = PrivateHas v0, v1 layout=L1
    v3 = Convert v2 declared={type=boolean flags=[IsExpected]}
    Return v3
}
function f6 name="Tested" module=m0 kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee, HasExplicitName] thisType={type=anything} parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    Jump -> next b1()
  b1():
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L2
    v5 = Convert v4 declared={type=L2 flags=[IsFromCompiler]} illTypedCause=FromLooseType
    v6 = CallDirect v5 function=f10
    Return v4
}
function f7 name="#m" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = Undefined
    Return v0
}
function f8 name="hook" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L2 flags=[IsExpected]} results=[{type=boolean flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g5
    v2 = PrivateHas v0, v1 layout=L2
    v3 = Convert v2 declared={type=boolean flags=[IsExpected]}
    Return v3
}
function f9 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L1 flags=[IsFromCompiler]} results=[{type=anything}] {
  b0(v0: L1):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g2
    PrivateDefine v0, v1, v1 layout=L1
    v3 = Undefined
    Return v3
}
function f10 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L2 flags=[IsFromCompiler]} results=[{type=anything}] {
  b0(v0: L2):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g5
    PrivateDefine v0, v1, v1 layout=L2
    v3 = Undefined
    Return v3
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral returnOverride = R"IR(layout L0 kind=Instance name="Base" module=m0 constructor=f1
layout L1 kind=Instance name="Stamper" module=m0 base=L0 constructor=f2 fields=[{key=#"#"@L1 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}] supertypes=[L0] flags=[HasHeritage]
layout L2 kind=Instance name="Point" module=m0 constructor=f6 fields=[{key="tag" declared={type="point" flags=[IsExpected]}}]
layout L3 module=m0 fields=[{key="tag" declared={type=string flags=[IsExpected]} flags=[IsRequired]}]
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=function} flags=[IsImmutable]
variable g2 module=m0 declared={type=classtoken} flags=[IsImmutable]
variable g3 module=m0 declared={type=anything} flags=[IsImmutable]
variable g4 module=m0 declared={type=function} flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = NewClass v0 layout=L0
    PutModuleVariable v1 variable=g0
    v3 = FinishClass v1 layout=L0
    v4 = Convert v3 declared={type=function flags=[IsExpected]}
    v5 = Convert v3 declared={type=function flags=[IsExpected]}
    v6 = ResolveHeritage v5
    v7 = NewClassToken layout=L1
    PutModuleVariable v7 variable=g2
    v9 = NewClosure function=f2
    v10 = NewClass v9, v5, v6 layout=L1
    PutModuleVariable v10 variable=g1
    v12 = NewClosure function=f3
    PutModuleVariable v12 variable=g3
    v14 = NewClosure function=f4
    DefineMethod v10, v14 layout=L1 side=Static key="invoke"
    v16 = NewClosure function=f5
    DefineMethod v10, v16 layout=L1 side=Static index=1 key="has"
    v18 = FinishClass v10 layout=L1
    v19 = Convert v18 declared={type=function flags=[IsExpected]}
    v20 = Convert v18 declared={type=function flags=[IsExpected]}
    v21 = NewClosure function=f6
    v22 = NewClass v21 layout=L2
    PutModuleVariable v22 variable=g4
    v24 = FinishClass v22 layout=L2
    v25 = Convert v24 declared={type=function flags=[IsExpected]}
    v26 = Convert v24 declared={type=function flags=[IsExpected]}
    v27 = Data data=d0
    v28 = Convert v27 declared={type=L3 flags=[IsExpected]}
    v29 = Construct v26 calleeName="Point"
    v30 = Convert v29 declared={type=L2 flags=[IsExpected]}
    v31 = Builtin builtin=GlobalThis
    v32 = PropGet v31 key="print" [isFreeName]
    v33 = PropGet v20 key="invoke"
    v34 = Convert v33 declared={type=function flags=[IsExpected]}
    v35 = Call v34, v20, v28 calleeKey="invoke" calleeName="invoke"
    v36 = Call v32, -, v35 calleeName="print"
    v37 = Builtin builtin=GlobalThis
    v38 = PropGet v37 key="print" [isFreeName]
    v39 = PropGet v20 key="has"
    v40 = Convert v39 declared={type=function flags=[IsExpected]}
    v41 = Call v40, v20, v28 calleeKey="has" calleeName="has"
    v42 = Convert v41 declared={type=boolean flags=[IsExpected]}
    v43 = Call v38, -, v42 calleeName="print"
    v44 = Builtin builtin=GlobalThis
    v45 = PropGet v44 key="print" [isFreeName]
    v46 = Construct v20, -, v28 calleeName="Stamper"
    v47 = Convert v46 declared={type=L1 flags=[IsExpected]}
    v48 = StrictEq v47, v28
    v49 = Call v45, -, v48 calleeName="print"
    v50 = Builtin builtin=GlobalThis
    v51 = PropGet v50 key="print" [isFreeName]
    v52 = PropGet v20 key="invoke"
    v53 = Convert v52 declared={type=function flags=[IsExpected]}
    v54 = Call v53, v20, v28 calleeKey="invoke" calleeName="invoke"
    v55 = Call v51, -, v54 calleeName="print"
    v56 = Builtin builtin=GlobalThis
    v57 = PropGet v56 key="print" [isFreeName]
    v58 = PropGet v20 key="has"
    v59 = Convert v58 declared={type=function flags=[IsExpected]}
    v60 = Call v59, v20, v28 calleeKey="has" calleeName="has"
    v61 = Convert v60 declared={type=boolean flags=[IsExpected]}
    v62 = Call v57, -, v61 calleeName="print"
    Jump -> next b2()
  b2() handler=b4():
    v63 = Construct v20, -, v28 calleeName="Stamper"
    Jump -> next b3()
  b3():
    Jump -> next b5()
  b4() role=CatchHandler:
    v64 = CatchValue
    v65 = Builtin builtin=GlobalThis
    v66 = PropGet v65 key="print" [isFreeName]
    v67 = Builtin builtin=GlobalThis
    v68 = PropGet v67 key="TypeError" [isFreeName]
    v69 = Convert v68 declared={type=builtin(TypeError) flags=[IsExpected]}
    v70 = InstanceOf v64, v69
    v71 = Call v66, -, v70 calleeName="print"
    Jump -> next b5()
  b5():
    v72 = Construct v20, -, v30 calleeName="Stamper"
    v73 = Builtin builtin=GlobalThis
    v74 = PropGet v73 key="print" [isFreeName]
    v75 = PropGet v20 key="invoke"
    v76 = Convert v75 declared={type=function flags=[IsExpected]}
    v77 = Call v76, v20, v30 calleeKey="invoke" calleeName="invoke"
    v78 = Call v74, -, v77 calleeName="print"
    v79 = Builtin builtin=GlobalThis
    v80 = PropGet v79 key="print" [isFreeName]
    v81 = PropGet v20 key="invoke"
    v82 = Convert v81 declared={type=function flags=[IsExpected]}
    v83 = Construct v26 calleeName="Point"
    v84 = Convert v83 declared={type=L2 flags=[IsExpected]}
    v85 = Call v82, v20, v84 calleeKey="invoke" calleeName="invoke"
    v86 = Call v80, -, v85 calleeName="print"
    v87 = Undefined
    Return v87
}
function f1 name="Base" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=object}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = GetConstructPrototype v0 layout=L0
    v3 = NewObject v2 layout=L0
    v4 = TypeTest v1 testedType=object
    Branch v4 -> taken b2(| 0), notTaken b3(| 0)
  b2(v5):
    Jump -> next b4(v5)
  b3(v6):
    Jump -> next b4(v3)
  b4(v7):
    Return v7
}
function f2 name="Stamper" module=m0 kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee, HasExplicitName] thisType={type=anything} parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    Jump -> next b1()
  b1():
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L1
    v5 = Convert v4 declared={type=L1 flags=[IsFromCompiler]} illTypedCause=FromLooseType
    v6 = CallDirect v5 function=f7
    Return v4
}
function f3 name="#m" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L1 flags=[IsExpected]} results=[{type=string flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = String string="stamped "
    v2 = PropGet v0 key="tag"
    v3 = Add v1, v2
    v4 = Convert v3 declared={type=string flags=[IsExpected]}
    Return v4
}
function f4 name="invoke" module=m0 kind=Method flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    Jump -> next b2()
  b2() handler=b3():
    v1 = GetModuleVariable variable=g2
    v2 = PrivateGet v0, v1 layout=L1
    v3 = GetModuleVariable variable=g3
    v4 = Call v3, v0 calleeName="#m"
    Return v4
  b3() role=CatchHandler:
    v5 = CatchValue
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="TypeError" [isFreeName]
    v8 = Convert v7 declared={type=builtin(TypeError) flags=[IsExpected]}
    v9 = InstanceOf v5, v8
    Return v9
}
function f5 name="has" module=m0 kind=Method flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=boolean flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g2
    v2 = PrivateHas v0, v1 layout=L1
    v3 = Convert v2 declared={type=boolean flags=[IsExpected]}
    Return v3
}
function f6 name="Point" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L2
    v2 = NewObject v1 layout=L2
    v3 = Convert v2 declared={type=L2 flags=[IsExpected]}
    v4 = String string="point"
    v5 = Convert v4 declared={type=string flags=[IsExpected]}
    PropSet v3, v5 key="tag"
    v7 = Undefined
    v8 = TypeTest v7 testedType=object
    Branch v8 -> taken b2(| 0), notTaken b3(| 0)
  b2(v9):
    Jump -> next b4(v9)
  b3(v10):
    Jump -> next b4(v2)
  b4(v11):
    Return v11
}
function f7 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L1 flags=[IsFromCompiler]} results=[{type=anything}] {
  b0(v0: L1):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g2
    PrivateDefine v0, v1, v1 layout=L1
    v3 = Undefined
    Return v3
}
data d0 root=Object{layout=L3 members=[{key="tag" value=String{value="plain"}}]} module=m0
entry module=m0
)IR"_s;

static void testReceiversWithoutBrand()
{
    checkPrivateNameProgram(receiversWithoutBrand, "true true true\ntrue true true\ntrue true true\ntrue true true\ntrue true true\ntrue true true\ntrue true true\ncalled got stored\n"_s);
}

static void testBeforeSuperReturns()
{
    checkPrivateNameProgram(beforeSuperReturns, "true\nprivate\nprivate\nfalse\ntrue\n"_s);
}

static void testReturnOverride()
{
    checkPrivateNameProgram(returnOverride, "true\nfalse\ntrue\nstamped plain\ntrue\ntrue\nstamped point\ntrue\n"_s);
}

std::span<const NativeTest> privateBrandProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "private-brand-programs/receivers-without-brand"_s, testReceiversWithoutBrand },
        { "private-brand-programs/before-super-returns"_s, testBeforeSuperReturns },
        { "private-brand-programs/return-override"_s, testReturnOverride },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
