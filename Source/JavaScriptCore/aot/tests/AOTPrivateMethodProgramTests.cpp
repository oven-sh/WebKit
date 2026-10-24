/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTPrivateNameProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral methodCalls = R"IR(layout L0 kind=Instance name="C" module=m0 constructor=f1 fields=[{key=#"#"@L0 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}, {key="arrowField" declared={type=function flags=[IsExpected]} flags=[IsRequired]}, {key="v" declared={type=anything}}]
layout L1 kind=Instance name="D" module=m0 base=L0 constructor=f7 supertypes=[L0] flags=[HasHeritage]
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=classtoken} flags=[IsImmutable]
variable g2 module=m0 declared={type=anything} flags=[IsImmutable]
variable g3 module=m0 declared={type=function} flags=[IsImmutable]
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
    DefineMethod v3, v7 layout=L0 key="onThis"
    v9 = NewClosure function=f4
    DefineMethod v3, v9 layout=L0 index=1 key="onOther"
    v11 = NewClosure function=f5
    DefineMethod v3, v11 layout=L0 side=Static index=2 key="onArgument"
    v13 = NewClosure function=f6
    DefineMethod v3, v13 layout=L0 index=3 key="inArrow"
    v15 = FinishClass v3 layout=L0
    v16 = Convert v15 declared={type=function flags=[IsExpected]}
    v17 = Convert v15 declared={type=function flags=[IsExpected]}
    v18 = ResolveHeritage v17
    v19 = NewClosure function=f7
    v20 = NewClass v19, v17, v18 layout=L1
    PutModuleVariable v20 variable=g3
    v22 = FinishClass v20 layout=L1
    v23 = Convert v22 declared={type=function flags=[IsExpected]}
    v24 = Convert v22 declared={type=function flags=[IsExpected]}
    v25 = Integer integer=10
    v26 = Construct v17, -, v25 calleeName="C"
    v27 = Convert v26 declared={type=L0 flags=[IsExpected]}
    v28 = Integer integer=20
    v29 = Construct v24, -, v28 calleeName="D"
    v30 = Convert v29 declared={type=L1 flags=[IsExpected]}
    v31 = Builtin builtin=GlobalThis
    v32 = PropGet v31 key="print" [isFreeName]
    v33 = PropGet v27 key="onThis"
    v34 = Convert v33 declared={type=function flags=[IsExpected]}
    v35 = Call v34, v27 calleeKey="onThis" calleeName="onThis"
    v36 = Call v32, -, v35 calleeName="print"
    v37 = Builtin builtin=GlobalThis
    v38 = PropGet v37 key="print" [isFreeName]
    v39 = PropGet v27 key="onOther"
    v40 = Convert v39 declared={type=function flags=[IsExpected]}
    v41 = Call v40, v27, v30 calleeKey="onOther" calleeName="onOther"
    v42 = Call v38, -, v41 calleeName="print"
    v43 = Builtin builtin=GlobalThis
    v44 = PropGet v43 key="print" [isFreeName]
    v45 = PropGet v17 key="onArgument"
    v46 = Convert v45 declared={type=function flags=[IsExpected]}
    v47 = Call v46, v17, v27 calleeKey="onArgument" calleeName="onArgument"
    v48 = Call v44, -, v47 calleeName="print"
    v49 = Builtin builtin=GlobalThis
    v50 = PropGet v49 key="print" [isFreeName]
    v51 = PropGet v27 key="inArrow"
    v52 = Convert v51 declared={type=function flags=[IsExpected]}
    v53 = Call v52, v27 calleeKey="inArrow" calleeName="inArrow"
    v54 = Call v50, -, v53 calleeName="print"
    v55 = Builtin builtin=GlobalThis
    v56 = PropGet v55 key="print" [isFreeName]
    v57 = PropGet v27 key="arrowField"
    v58 = Convert v57 declared={type=function flags=[IsExpected]}
    v59 = Call v58, v27 calleeKey="arrowField" calleeName="arrowField"
    v60 = Call v56, -, v59 calleeName="print"
    v61 = Builtin builtin=GlobalThis
    v62 = PropGet v61 key="print" [isFreeName]
    v63 = PropGet v30 key="onThis"
    v64 = Convert v63 declared={type=function flags=[IsExpected]}
    v65 = Call v64, v30 calleeKey="onThis" calleeName="onThis"
    v66 = Call v62, -, v65 calleeName="print"
    v67 = Builtin builtin=GlobalThis
    v68 = PropGet v67 key="print" [isFreeName]
    v69 = PropGet v30 key="arrowField"
    v70 = Convert v69 declared={type=function flags=[IsExpected]}
    v71 = Call v70, v30 calleeKey="arrowField" calleeName="arrowField"
    v72 = Call v68, -, v71 calleeName="print"
    v73 = Undefined
    Return v73
}
function f1 name="C" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=object}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = GetConstructPrototype v0 layout=L0
    v3 = NewObject v2 layout=L0
    v4 = Convert v3 declared={type=L0 flags=[IsFromCompiler]} illTypedCause=FromLooseType
    v5 = CallDirect v4 function=f8
    v6 = Convert v3 declared={type=L0 flags=[IsExpected]}
    PropSet v6, v1 key="v"
    v8 = Undefined
    v9 = TypeTest v8 testedType=object
    Branch v9 -> taken b2(| 0), notTaken b3(| 0)
  b2(v10):
    Jump -> next b4(v10)
  b3(v11):
    Jump -> next b4(v3)
  b4(v12):
    Return v12
}
function f2 name="#m" module=m0 kind=Method flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = PropGet v0 key="v"
    v3 = Add v2, v1
    Return v3
}
function f3 name="onThis" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0
    v3 = GetModuleVariable variable=g2
    v4 = Integer integer=1
    v5 = Call v3, v0, v4 calleeName="#m"
    ReturnCall v5
}
function f4 name="onOther" module=m0 kind=Method flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0
    v3 = GetModuleVariable variable=g2
    v4 = Integer integer=2
    v5 = Call v3, v0, v4 calleeName="#m"
    ReturnCall v5
}
function f5 name="onArgument" module=m0 kind=Method flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0
    v3 = GetModuleVariable variable=g2
    v4 = Integer integer=3
    v5 = Call v3, v0, v4 calleeName="#m"
    ReturnCall v5
}
function f6 name="inArrow" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = NewClosure v0 function=f9
    v2 = Call v1
    ReturnCall v2
}
function f7 name="D" module=m0 kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee, HasExplicitName] thisType={type=anything} parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    Jump -> next b1()
  b1():
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L1
    Return v4
}
function f8 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L0 flags=[IsFromCompiler]} results=[{type=anything}] {
  b0(v0: L0):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    PrivateDefine v0, v1, v1 layout=L0
    v3 = NewClosure v0 function=f10
    v4 = Convert v3 declared={type=function flags=[IsExpected]}
    PropDefine v0, v4 key="arrowField"
    v6 = Undefined
    Return v6
}
function f9 module=m0 kind=Arrow captures=[{type=L0 flags=[IsExpected]}] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = Convert v0 declared={type=L0 flags=[IsExpected]}
    v2 = GetModuleVariable variable=g1
    v3 = PrivateGet v1, v2 layout=L0
    v4 = GetModuleVariable variable=g2
    v5 = Integer integer=4
    v6 = Call v4, v1, v5 calleeName="#m"
    ReturnCall v6
}
function f10 name="arrowField" module=m0 kind=Arrow captures=[{type=L0 flags=[IsFromCompiler]}] thisType={type=anything} results=[{type=anything}] {
  b0(v0: L0):
    Jump -> next b1()
  b1():
    v1 = Convert v0 declared={type=L0 flags=[IsExpected]}
    v2 = GetModuleVariable variable=g1
    v3 = PrivateGet v1, v2 layout=L0
    v4 = GetModuleVariable variable=g2
    v5 = Integer integer=5
    v6 = Call v4, v1, v5 calleeName="#m"
    ReturnCall v6
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral methodReadAsValue = R"IR(layout L0 kind=Instance name="C" module=m0 constructor=f1 fields=[{key=#"#"@L0 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}, {key="v" declared={type=anything}}]
layout L1 module=m0 fields=[{key="v" declared={type=string flags=[IsExpected]} flags=[IsRequired]}, {key="f" declared={type=function flags=[IsExpected]} flags=[IsRequired]}]
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=classtoken} flags=[IsImmutable]
variable g2 module=m0 declared={type=anything} flags=[IsImmutable]
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
    DefineMethod v3, v7 layout=L0 key="bare"
    v9 = NewClosure function=f4
    DefineMethod v3, v9 layout=L0 index=1 key="held"
    v11 = NewClosure function=f5
    DefineMethod v3, v11 layout=L0 index=2 key="same"
    v13 = NewClosure function=f6
    DefineMethod v3, v13 layout=L0 index=3 key="get"
    v15 = FinishClass v3 layout=L0
    v16 = Convert v15 declared={type=function flags=[IsExpected]}
    v17 = Convert v15 declared={type=function flags=[IsExpected]}
    v18 = Integer integer=5
    v19 = Construct v17, -, v18 calleeName="C"
    v20 = Convert v19 declared={type=L0 flags=[IsExpected]}
    v21 = PropGet v20 key="get"
    v22 = Convert v21 declared={type=function flags=[IsExpected]}
    v23 = Call v22, v20 calleeKey="get" calleeName="get"
    v24 = Convert v23 declared={type=function flags=[IsExpected]}
    v25 = Builtin builtin=GlobalThis
    v26 = PropGet v25 key="print" [isFreeName]
    v27 = PropGet v20 key="bare"
    v28 = Convert v27 declared={type=function flags=[IsExpected]}
    v29 = Call v28, v20 calleeKey="bare" calleeName="bare"
    v30 = Call v26, -, v29 calleeName="print"
    v31 = Builtin builtin=GlobalThis
    v32 = PropGet v31 key="print" [isFreeName]
    v33 = PropGet v20 key="held"
    v34 = Convert v33 declared={type=function flags=[IsExpected]}
    v35 = Call v34, v20 calleeKey="held" calleeName="held"
    v36 = Call v32, -, v35 calleeName="print"
    v37 = Builtin builtin=GlobalThis
    v38 = PropGet v37 key="print" [isFreeName]
    v39 = PropGet v20 key="same"
    v40 = Convert v39 declared={type=function flags=[IsExpected]}
    v41 = Integer integer=6
    v42 = Construct v17, -, v41 calleeName="C"
    v43 = Convert v42 declared={type=L0 flags=[IsExpected]}
    v44 = Call v40, v20, v43 calleeKey="same" calleeName="same"
    v45 = Convert v44 declared={type=boolean flags=[IsExpected]}
    v46 = Call v38, -, v45 calleeName="print"
    v47 = Builtin builtin=GlobalThis
    v48 = PropGet v47 key="print" [isFreeName]
    v49 = TypeOf v24
    v50 = Call v48, -, v49 calleeName="print"
    v51 = Builtin builtin=GlobalThis
    v52 = PropGet v51 key="print" [isFreeName]
    v53 = PropGet v24 key="name"
    v54 = Convert v53 declared={type=string flags=[IsExpected]}
    v55 = Call v52, -, v54 calleeName="print"
    v56 = Builtin builtin=GlobalThis
    v57 = PropGet v56 key="print" [isFreeName]
    v58 = Call v24 calleeName="f"
    v59 = Call v57, -, v58 calleeName="print"
    v60 = Undefined
    Return v60
}
function f1 name="C" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=object}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = GetConstructPrototype v0 layout=L0
    v3 = NewObject v2 layout=L0
    v4 = Convert v3 declared={type=L0 flags=[IsFromCompiler]} illTypedCause=FromLooseType
    v5 = CallDirect v4 function=f7
    v6 = Convert v3 declared={type=L0 flags=[IsExpected]}
    PropSet v6, v1 key="v"
    v8 = Undefined
    v9 = TypeTest v8 testedType=object
    Branch v9 -> taken b2(| 0), notTaken b3(| 0)
  b2(v10):
    Jump -> next b4(v10)
  b3(v11):
    Jump -> next b4(v3)
  b4(v12):
    Return v12
}
function f2 name="#m" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = TypeTest v0 testedType=undefined
    Branch v1 -> taken b2(| 0), notTaken b4(| 0)
  b2(v2):
    Jump -> next b3()
  b3():
    v3 = String string="no receiver"
    Jump -> next b6(v3)
  b4(v4):
    Jump -> next b5()
  b5():
    v5 = PropGet v0 key="v"
    Jump -> next b6(v5)
  b6(v6):
    Return v6
}
function f3 name="bare" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0
    v3 = GetModuleVariable variable=g2
    v4 = Convert v3 declared={type=function flags=[IsExpected]}
    v5 = Call v4 calleeName="f"
    ReturnCall v5
}
function f4 name="held" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = String string="holder"
    v2 = Convert v1 declared={type=string flags=[IsExpected]}
    v3 = GetModuleVariable variable=g1
    v4 = PrivateGet v0, v3 layout=L0
    v5 = GetModuleVariable variable=g2
    v6 = Convert v5 declared={type=function flags=[IsExpected]}
    v7 = NewObject -, v2, v6 layout=L1 fields=[0, 1]
    v8 = FinishObject v7
    v9 = Convert v8 declared={type=L1 flags=[IsExpected]}
    v10 = PropGet v9 key="f"
    v11 = Convert v10 declared={type=function flags=[IsExpected]}
    v12 = Call v11, v9 calleeKey="f" calleeName="f"
    ReturnCall v12
}
function f5 name="same" module=m0 kind=Method flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=boolean flags=[IsExpected]}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = GetModuleVariable variable=g1
    v3 = PrivateGet v0, v2 layout=L0
    v4 = GetModuleVariable variable=g2
    v5 = Convert v4 declared={type=function flags=[IsExpected]}
    v6 = GetModuleVariable variable=g1
    v7 = PrivateGet v1, v6 layout=L0
    v8 = GetModuleVariable variable=g2
    v9 = StrictEq v5, v8
    v10 = Convert v9 declared={type=boolean flags=[IsExpected]}
    Return v10
}
function f6 name="get" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=function flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0
    v3 = GetModuleVariable variable=g2
    v4 = Convert v3 declared={type=function flags=[IsExpected]}
    Return v4
}
function f7 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L0 flags=[IsFromCompiler]} results=[{type=anything}] {
  b0(v0: L0):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    PrivateDefine v0, v1, v1 layout=L0
    v3 = Undefined
    Return v3
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral methodsInAHotLoop = R"IR(layout L0 kind=Instance name="Counter" module=m0 constructor=f1 fields=[{key=#"#"@L0 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}, {key="total" declared={type=number flags=[IsExpected]} flags=[IsRequired]}]
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=classtoken} flags=[IsImmutable]
variable g2 module=m0 declared={type=anything} flags=[IsImmutable]
variable g3 module=m0 declared={type=anything} flags=[IsImmutable]
variable g4 module=m0 declared={type=anything} flags=[IsImmutable]
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
    DefineMethod v3, v11 layout=L0 key="run"
    v13 = FinishClass v3 layout=L0
    v14 = Convert v13 declared={type=function flags=[IsExpected]}
    v15 = Convert v13 declared={type=function flags=[IsExpected]}
    v16 = Builtin builtin=GlobalThis
    v17 = PropGet v16 key="print" [isFreeName]
    v18 = Construct v15 calleeName="Counter"
    v19 = Convert v18 declared={type=L0 flags=[IsExpected]}
    v20 = PropGet v19 key="run"
    v21 = Convert v20 declared={type=function flags=[IsExpected]}
    v22 = Integer integer=100000
    v23 = Call v21, v19, v22 calleeKey="run" calleeName="run"
    v24 = Convert v23 declared={type=number flags=[IsExpected]}
    v25 = Call v17, -, v24 calleeName="print"
    v26 = Undefined
    Return v26
}
function f1 name="Counter" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Convert v2 declared={type=L0 flags=[IsFromCompiler]} illTypedCause=FromLooseType
    v4 = CallDirect v3 function=f6
    Return v2
}
function f2 name="#step" module=m0 kind=Method flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=number flags=[IsExpected]}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = PropGet v0 key="total"
    v3 = Add v2, v1
    v4 = Convert v3 declared={type=number flags=[IsExpected]}
    PropSet v0, v4 key="total"
    v6 = PropGet v0 key="total"
    v7 = Convert v6 declared={type=number flags=[IsExpected]}
    Return v7
}
function f3 name="#half" module=m0 kind=Getter flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=number flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = PropGet v0 key="total"
    v2 = Convert v1 declared={type=number flags=[IsExpected]}
    v3 = Integer integer=1
    v4 = RShift v2, v3
    v5 = Convert v4 declared={type=number flags=[IsExpected]}
    Return v5
}
function f4 name="#reset" module=m0 kind=Setter flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = Convert v1 declared={type=number flags=[IsExpected]}
    PropSet v0, v2 key="total"
    v4 = Undefined
    Return v4
}
function f5 name="run" module=m0 kind=Method flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=number flags=[IsExpected]}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = Integer
    v3 = Convert v2 declared={type=number flags=[IsExpected]}
    v4 = Integer
    v5 = Convert v4 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v3, v5)
  b2(v6, v7):
    LoopHint
    v9 = Less v7, v1
    Branch v9 -> taken b3(| 0, 1), notTaken b8(| 0, 1)
  b3(v10, v11):
    Jump -> next b4()
  b4():
    v12 = GetModuleVariable variable=g1
    v13 = PrivateGet v0, v12 layout=L0
    v14 = GetModuleVariable variable=g2
    v15 = Integer integer=3
    v16 = BitAnd v10, v15
    v17 = Call v14, v0, v16 calleeName="#step"
    v18 = Convert v17 declared={type=number flags=[IsExpected]}
    v19 = Integer integer=1023
    v20 = BitAnd v10, v19
    v21 = Integer integer=1023
    v22 = StrictEq v20, v21
    Branch v22 -> taken b5(), notTaken b6()
  b5():
    v23 = GetModuleVariable variable=g1
    v24 = PrivateGet v0, v23 layout=L0
    v25 = GetModuleVariable variable=g3
    v26 = Call v25, v0
    v27 = Convert v26 declared={type=number flags=[IsExpected]}
    v28 = GetModuleVariable variable=g1
    v29 = PrivateGet v0, v28 layout=L0
    v30 = GetModuleVariable variable=g4
    v31 = Call v30, v0, v27
    Jump -> next b6()
  b6():
    Jump -> next b7()
  b7():
    v32 = Increment v10
    v33 = Convert v32 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v18, v33)
  b8(v34, v35):
    Jump -> next b9()
  b9():
    v36 = Convert v6 declared={type=number flags=[IsExpected]}
    Return v36
}
function f6 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L0 flags=[IsFromCompiler]} results=[{type=anything}] {
  b0(v0: L0):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    PrivateDefine v0, v1, v1 layout=L0
    v3 = Integer
    v4 = Convert v3 declared={type=number flags=[IsExpected]}
    PropDefine v0, v4 key="total"
    v6 = Undefined
    Return v6
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral subclassMadeByAFunction = R"IR(layout L0 kind=Instance name="K" module=m0 constructor=f2 fields=[{key=#"#"@L0 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}, {key=#"#p"@L0 declared={type=number flags=[IsExpected]} flags=[IsRequired, IsPrivate]}, {key="a" declared={type=number flags=[IsExpected]} flags=[IsRequired]}]
layout L1 kind=Instance module=m0 base=L0 constructor=f6 fields=[{key="b" declared={type=number flags=[IsExpected]} flags=[IsRequired]}] supertypes=[L0] flags=[HasHeritage]
variable g0 name="K" module=m0 declared={type=function|absent} flags=[IsImmutable]
variable g1 module=m0 declared={type=function} flags=[IsImmutable]
variable g2 module=m0 declared={type=classtoken} flags=[IsImmutable]
variable g3 module=m0 declared={type=anything} flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = Convert v0 declared={type=function flags=[IsExpected]}
    v2 = NewClassToken layout=L0
    PutModuleVariable v2 variable=g2
    v4 = NewClosure function=f2
    v5 = NewClass v4 layout=L0
    PutModuleVariable v5 variable=g1
    v7 = NewClosure function=f3
    PutModuleVariable v7 variable=g3
    v9 = NewClosure function=f4
    DefineMethod v5, v9 layout=L0 key="read"
    v11 = NewClosure function=f5
    DefineMethod v5, v11 layout=L0 side=Static index=1 key="has"
    v13 = FinishClass v5 layout=L0
    v14 = Convert v13 declared={type=function flags=[IsExpected]}
    v15 = Convert v13 declared={type=function flags=[IsExpected]}
    PutModuleVariable v15 variable=g0
    v17 = Convert v1 declared={type=function flags=[IsExpected]}
    v18 = Call v17 calleeName="make"
    v19 = Convert v18 declared={type=function flags=[IsExpected]}
    v20 = Call v17 calleeName="make"
    v21 = Convert v20 declared={type=function flags=[IsExpected]}
    v22 = Builtin builtin=GlobalThis
    v23 = PropGet v22 key="print" [isFreeName]
    v24 = Construct v19 calleeName="First"
    v25 = Convert v24 declared={type=L1 flags=[IsExpected]}
    v26 = PropGet v25 key="get"
    v27 = Convert v26 declared={type=function flags=[IsExpected]}
    v28 = Call v27, v25 calleeKey="get" calleeName="get"
    v29 = Convert v28 declared={type=number flags=[IsExpected]}
    v30 = Call v23, -, v29 calleeName="print"
    v31 = Builtin builtin=GlobalThis
    v32 = PropGet v31 key="print" [isFreeName]
    v33 = Construct v19 calleeName="First"
    v34 = Convert v33 declared={type=L1 flags=[IsExpected]}
    v35 = PropGet v34 key="read"
    v36 = Convert v35 declared={type=function flags=[IsExpected]}
    v37 = Call v36, v34 calleeKey="read" calleeName="read"
    v38 = Convert v37 declared={type=number flags=[IsExpected]}
    v39 = Call v32, -, v38 calleeName="print"
    v40 = Builtin builtin=GlobalThis
    v41 = PropGet v40 key="print" [isFreeName]
    v42 = Construct v21 calleeName="Second"
    v43 = Convert v42 declared={type=L1 flags=[IsExpected]}
    v44 = PropGet v43 key="read"
    v45 = Convert v44 declared={type=function flags=[IsExpected]}
    v46 = Call v45, v43 calleeKey="read" calleeName="read"
    v47 = Convert v46 declared={type=number flags=[IsExpected]}
    v48 = Call v41, -, v47 calleeName="print"
    v49 = Builtin builtin=GlobalThis
    v50 = PropGet v49 key="print" [isFreeName]
    v51 = PropGet v15 key="has"
    v52 = Convert v51 declared={type=function flags=[IsExpected]}
    v53 = Construct v21 calleeName="Second"
    v54 = Convert v53 declared={type=L1 flags=[IsExpected]}
    v55 = Call v52, v15, v54 calleeKey="has" calleeName="has"
    v56 = Convert v55 declared={type=boolean flags=[IsExpected]}
    v57 = Call v50, -, v56 calleeName="print"
    v58 = Builtin builtin=GlobalThis
    v59 = PropGet v58 key="print" [isFreeName]
    v60 = Builtin builtin=GlobalThis
    v61 = PropGet v60 key="Object" [isFreeName]
    v62 = Convert v61 declared={type=builtin(Object) flags=[IsExpected]}
    v63 = PropGet v62 key="keys"
    v64 = Convert v63 declared={type=function flags=[IsExpected]}
    v65 = Convert v64 declared={type=builtin(ObjectKeys) flags=[IsExpected]}
    v66 = Construct v21 calleeName="Second"
    v67 = Convert v66 declared={type=L1 flags=[IsExpected]}
    v68 = Call v65, v62, v67 calleeKey="keys" calleeName="keys"
    v69 = Convert v68 declared={type=Array<string> flags=[IsExpected]}
    v70 = PropGet v69 key="length"
    v71 = Convert v70 declared={type=number flags=[IsExpected]}
    v72 = Call v59, -, v71 calleeName="print"
    v73 = Undefined
    Return v73
}
function f1 name="make" module=m0 flags=[HasExplicitName] thisType={type=anything} results=[{type=function flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = GetModuleVariable variable=g0
    v1 = CheckInitialized v0 name="K"
    v2 = ResolveHeritage v1
    v3 = NewClosure function=f6
    v4 = NewClass v3, v1, v2 layout=L1
    v5 = NewClosure function=f7
    DefineMethod v4, v5 layout=L1 key="get"
    v7 = FinishClass v4 layout=L1
    v8 = Convert v7 declared={type=function flags=[IsExpected]}
    Return v8
}
function f2 name="K" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Convert v2 declared={type=L0 flags=[IsFromCompiler]} illTypedCause=FromLooseType
    v4 = CallDirect v3 function=f8
    Return v2
}
function f3 name="#m" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=number flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g2
    v2 = PrivateGet v0, v1 layout=L0 field=1
    v3 = Convert v2 declared={type=number flags=[IsExpected]}
    v4 = PropGet v0 key="a"
    v5 = Convert v4 declared={type=number flags=[IsExpected]}
    v6 = Add v3, v5
    v7 = Convert v6 declared={type=number flags=[IsExpected]}
    Return v7
}
function f4 name="read" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=number flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g2
    v2 = PrivateGet v0, v1 layout=L0
    v3 = GetModuleVariable variable=g3
    v4 = Call v3, v0 calleeName="#m"
    ReturnCall v4 casts=[{declared={type=number flags=[IsExpected]}}]
}
function f5 name="has" module=m0 kind=Method flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=boolean flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g2
    v2 = PrivateHas v0, v1 layout=L0
    v3 = Convert v2 declared={type=boolean flags=[IsExpected]}
    Return v3
}
function f6 module=m0 kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee] thisType={type=anything} parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    Jump -> next b1()
  b1():
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L1
    v5 = Convert v4 declared={type=L1 flags=[IsFromCompiler]} illTypedCause=FromLooseType
    v6 = CallDirect v5 function=f9
    Return v4
}
function f7 name="get" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L1 flags=[IsExpected]} results=[{type=number flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = PropGet v0 key="b"
    v2 = Convert v1 declared={type=number flags=[IsExpected]}
    Return v2
}
function f8 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L0 flags=[IsFromCompiler]} results=[{type=anything}] {
  b0(v0: L0):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g2
    PrivateDefine v0, v1, v1 layout=L0
    v3 = Integer integer=7
    v4 = Convert v3 declared={type=number flags=[IsExpected]}
    v5 = GetModuleVariable variable=g2
    PrivateDefine v0, v5, v4 layout=L0 field=1
    v7 = Integer integer=1
    v8 = Convert v7 declared={type=number flags=[IsExpected]}
    PropDefine v0, v8 key="a"
    v10 = Undefined
    Return v10
}
function f9 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L1 flags=[IsFromCompiler]} results=[{type=anything}] {
  b0(v0: L1):
    Jump -> next b1()
  b1():
    v1 = Integer integer=2
    v2 = Convert v1 declared={type=number flags=[IsExpected]}
    PropDefine v0, v2 key="b"
    v4 = Undefined
    Return v4
}
entry module=m0
)IR"_s;

static void testMethodCalls()
{
    checkPrivateNameProgram(methodCalls, "11\n22\n13\n14\n15\n21\n25\n"_s);
}

static void testMethodReadAsValue()
{
    checkPrivateNameProgram(methodReadAsValue, "no receiver\nholder\ntrue\nfunction\n#m\nno receiver\n"_s);
}

static void testMethodsInAHotLoop()
{
    checkPrivateNameProgram(methodsInAHotLoop, "2543\n"_s);
}

static void testSubclassMadeByAFunction()
{
    checkPrivateNameProgram(subclassMadeByAFunction, "2\n8\n8\ntrue\n2\n"_s);
}

std::span<const NativeTest> privateMethodProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "private-method-programs/method-calls"_s, testMethodCalls },
        { "private-method-programs/method-read-as-value"_s, testMethodReadAsValue },
        { "private-method-programs/methods-in-a-hot-loop"_s, testMethodsInAHotLoop },
        { "private-method-programs/subclass-made-by-a-function"_s, testSubclassMadeByAFunction },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
