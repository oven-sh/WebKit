/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTPrivateNameProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral asyncMethods = R"IR(layout L0 kind=Instance name="C" module=m0 constructor=f1 fields=[{key=#"#"@L0 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}, {key="v" declared={type=anything}}]
layout L1 module=m0
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=classtoken} flags=[IsImmutable]
variable g2 module=m0 declared={type=anything} flags=[IsImmutable]
variable g3 module=m0 declared={type=anything} flags=[IsImmutable]
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
    DefineMethod v3, v9 layout=L0 key="later"
    v11 = NewClosure function=f5
    DefineMethod v3, v11 layout=L0 index=1 key="collect"
    v13 = NewClosure function=f6
    DefineMethod v3, v13 layout=L0 side=Static index=2 key="later"
    v15 = FinishClass v3 layout=L0
    v16 = Convert v15 declared={type=function flags=[IsExpected]}
    v17 = Convert v15 declared={type=function flags=[IsExpected]}
    v18 = Integer integer=10
    v19 = Construct v17, -, v18 calleeName="C"
    v20 = Convert v19 declared={type=L0 flags=[IsExpected]}
    v21 = PropGet v20 key="later"
    v22 = Convert v21 declared={type=function flags=[IsExpected]}
    v23 = Integer integer=5
    v24 = Call v22, v20, v23 calleeKey="later" calleeName="later"
    v25 = Convert v24 declared={type=Promise flags=[IsExpected]}
    v26 = PropGet v25 key="then"
    v27 = Convert v26 declared={type=function flags=[IsExpected]}
    v28 = NewClosure function=f7
    v29 = Call v27, v25, v28 calleeKey="then" calleeName="then"
    v30 = PropGet v20 key="collect"
    v31 = Convert v30 declared={type=function flags=[IsExpected]}
    v32 = Integer integer=3
    v33 = Call v31, v20, v32 calleeKey="collect" calleeName="collect"
    v34 = Convert v33 declared={type=Promise<string> flags=[IsExpected]}
    v35 = PropGet v34 key="then"
    v36 = Convert v35 declared={type=function flags=[IsExpected]}
    v37 = NewClosure function=f8
    v38 = Call v36, v34, v37 calleeKey="then" calleeName="then"
    Jump -> next b2()
  b2() handler=b4():
    v39 = PropGet v17 key="later"
    v40 = Convert v39 declared={type=function flags=[IsExpected]}
    v41 = Data data=d0
    v42 = Call v40, v17, v41 calleeKey="later" calleeName="later"
    Jump -> next b3()
  b3():
    Jump -> next b5()
  b4() role=CatchHandler:
    v43 = CatchValue
    v44 = Builtin builtin=GlobalThis
    v45 = PropGet v44 key="print" [isFreeName]
    v46 = Builtin builtin=GlobalThis
    v47 = PropGet v46 key="TypeError" [isFreeName]
    v48 = Convert v47 declared={type=builtin(TypeError) flags=[IsExpected]}
    v49 = InstanceOf v43, v48
    v50 = Call v45, -, v49 calleeName="print"
    Jump -> next b5()
  b5():
    v51 = Builtin builtin=GlobalThis
    v52 = PropGet v51 key="print" [isFreeName]
    v53 = String string="end of module"
    v54 = Call v52, -, v53 calleeName="print"
    v55 = Undefined
    Return v55
}
function f1 name="C" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=object}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = GetConstructPrototype v0 layout=L0
    v3 = NewObject v2 layout=L0
    v4 = Convert v3 declared={type=L0 flags=[IsFromCompiler]} illTypedCause=FromLooseType
    v5 = CallDirect v4 function=f9
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
function f2 name="#later" module=m0 kind=Method flags=[IsAsync, HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=Promise flags=[IsExpected]}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = Await v1
    v3 = PropGet v0 key="v"
    v4 = Add v3, v2
    Return v4
}
function f3 name="#stream" module=m0 kind=Method flags=[IsAsync, IsGenerator, HasThis, TakesCallee, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=AsyncGenerator flags=[IsExpected]}] {
  b0(v0: anything, v1: function, v2: anything):
    Jump -> next b1()
  b1():
    InitialYield
    v4 = Integer
    v5 = Convert v4 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v5)
  b2(v6):
    LoopHint
    v8 = Less v6, v2
    Branch v8 -> taken b3(| 0, 1), notTaken b9(| 0, 1)
  b3(v9, v10):
    Jump -> next b4()
  b4():
    v11 = PropGet v0 key="v"
    v12 = Add v11, v9
    v13 = Await v12
    v14 = Yield v13
    v15 = ResultAt v14
    v16 = ResultAt v14 index=1
    Switch v16 cases=[1, 2] -> case b5(), case b6(), otherwise b7()
  b5():
    Throw v15
  b6():
    Return v15
  b7():
    Jump -> next b8()
  b8():
    v17 = Increment v9
    v18 = Convert v17 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v18)
  b9(v19, v20):
    Jump -> next b10()
  b10():
    v21 = Undefined
    Return v21
}
function f4 name="later" module=m0 kind=Method flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=Promise flags=[IsExpected]}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = GetModuleVariable variable=g1
    v3 = PrivateGet v0, v2 layout=L0
    v4 = GetModuleVariable variable=g2
    v5 = Call v4, v0, v1 calleeName="#later"
    ReturnCall v5 casts=[{declared={type=Promise flags=[IsExpected]}}]
}
function f5 name="collect" module=m0 kind=Method flags=[IsAsync, HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=Promise<string> flags=[IsExpected]}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = String string=""
    v3 = Convert v2 declared={type=string flags=[IsExpected]}
    v4 = GetModuleVariable variable=g1
    v5 = PrivateGet v0, v4 layout=L0
    v6 = GetModuleVariable variable=g3
    v7 = Call v6, v0, v1 calleeName="#stream"
    v8 = Convert v7 declared={type=AsyncGenerator flags=[IsExpected]}
    v9 = IteratorOpen v8 iteratorKind=Async
    Jump -> next b2(v3)
  b2(v10):
    LoopHint
    v12 = IteratorResume v9 iteratorKind=Async
    v13 = ResultAt v12
    v14 = ResultAt v12 index=1
    Branch v13 -> taken b3(), notTaken b4()
  b3():
    v15 = Convert v10 declared={type=string flags=[IsExpected]}
    Return v15
  b4():
    Jump -> next b5()
  b5() handler=b7():
    v16 = String string=","
    v17 = Add v14, v16
    v18 = Add v10, v17
    v19 = Convert v18 declared={type=string flags=[IsExpected]}
    Jump -> next b6()
  b6() handler=b7():
    Jump -> next b2(v19)
  b7() role=IterationCleanup subject=v9:
    v20 = CatchException
    IteratorClose v9 iteratorKind=Async completion=Throw
    Rethrow v20
}
function f6 name="later" module=m0 kind=Method flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0
    v3 = GetModuleVariable variable=g2
    v4 = Integer integer=1
    v5 = Call v3, v0, v4 calleeName="#later"
    ReturnCall v5
}
function f7 module=m0 kind=Arrow length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = String string="later "
    v4 = Add v3, v0
    v5 = Call v2, -, v4 calleeName="print"
    ReturnCall v5
}
function f8 module=m0 kind=Arrow length=1 thisType={type=anything} parameters=[{type=string flags=[IsExpected]}] results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = String string="collected "
    v4 = Add v3, v0
    v5 = Call v2, -, v4 calleeName="print"
    ReturnCall v5
}
function f9 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L0 flags=[IsFromCompiler]} results=[{type=anything}] {
  b0(v0: L0):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    PrivateDefine v0, v1, v1 layout=L0
    v3 = Undefined
    Return v3
}
data d0 root=Object{layout=L1} module=m0
entry module=m0
)IR"_s;

static constexpr ASCIILiteral generatorMethods = R"IR(layout L0 kind=Instance name="C" module=m0 constructor=f1 fields=[{key=#"#"@L0 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}, {key="v" declared={type=anything}}]
layout L1 module=m0
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
    DefineMethod v3, v7 layout=L0 key="sum"
    v9 = NewClosure function=f4
    DefineMethod v3, v9 layout=L0 side=Static index=1 key="open"
    v11 = FinishClass v3 layout=L0
    v12 = Convert v11 declared={type=function flags=[IsExpected]}
    v13 = Convert v11 declared={type=function flags=[IsExpected]}
    v14 = Builtin builtin=GlobalThis
    v15 = PropGet v14 key="print" [isFreeName]
    v16 = Integer integer=10
    v17 = Construct v13, -, v16 calleeName="C"
    v18 = Convert v17 declared={type=L0 flags=[IsExpected]}
    v19 = PropGet v18 key="sum"
    v20 = Convert v19 declared={type=function flags=[IsExpected]}
    v21 = Integer integer=3
    v22 = Call v20, v18, v21 calleeKey="sum" calleeName="sum"
    v23 = Convert v22 declared={type=number flags=[IsExpected]}
    v24 = Call v15, -, v23 calleeName="print"
    v25 = Builtin builtin=GlobalThis
    v26 = PropGet v25 key="print" [isFreeName]
    v27 = PropGet v13 key="open"
    v28 = Convert v27 declared={type=function flags=[IsExpected]}
    v29 = Integer integer=1
    v30 = Construct v13, -, v29 calleeName="C"
    v31 = Convert v30 declared={type=L0 flags=[IsExpected]}
    v32 = Call v28, v13, v31 calleeKey="open" calleeName="open"
    v33 = Convert v32 declared={type=boolean|"opened" flags=[IsExpected]}
    v34 = Call v26, -, v33 calleeName="print"
    v35 = Builtin builtin=GlobalThis
    v36 = PropGet v35 key="print" [isFreeName]
    v37 = PropGet v13 key="open"
    v38 = Convert v37 declared={type=function flags=[IsExpected]}
    v39 = Data data=d0
    v40 = Call v38, v13, v39 calleeKey="open" calleeName="open"
    v41 = Convert v40 declared={type=boolean|"opened" flags=[IsExpected]}
    v42 = Call v36, -, v41 calleeName="print"
    v43 = Undefined
    Return v43
}
function f1 name="C" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=object}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = GetConstructPrototype v0 layout=L0
    v3 = NewObject v2 layout=L0
    v4 = Convert v3 declared={type=L0 flags=[IsFromCompiler]} illTypedCause=FromLooseType
    v5 = CallDirect v4 function=f5
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
function f2 name="#each" module=m0 kind=Method flags=[IsGenerator, HasThis, TakesCallee, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=Generator flags=[IsExpected]}] {
  b0(v0: anything, v1: function, v2: anything):
    Jump -> next b1()
  b1():
    InitialYield
    v4 = Integer
    v5 = Convert v4 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v5)
  b2(v6):
    LoopHint
    v8 = Less v6, v2
    Branch v8 -> taken b3(| 0, 1), notTaken b9(| 0, 1)
  b3(v9, v10):
    Jump -> next b4()
  b4():
    v11 = PropGet v0 key="v"
    v12 = Add v11, v9
    v13 = Yield v12
    v14 = ResultAt v13
    v15 = ResultAt v13 index=1
    Switch v15 cases=[1, 2] -> case b5(), case b6(), otherwise b7()
  b5():
    Throw v14
  b6():
    Return v14
  b7():
    Jump -> next b8()
  b8():
    v16 = Increment v9
    v17 = Convert v16 declared={type=number flags=[IsExpected]}
    Jump -> next b2(v17)
  b9(v18, v19):
    Jump -> next b10()
  b10():
    v20 = Undefined
    Return v20
}
function f3 name="sum" module=m0 kind=Method flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=number flags=[IsExpected]}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = Integer
    v3 = Convert v2 declared={type=number flags=[IsExpected]}
    v4 = GetModuleVariable variable=g1
    v5 = PrivateGet v0, v4 layout=L0
    v6 = GetModuleVariable variable=g2
    v7 = Call v6, v0, v1 calleeName="#each"
    v8 = Convert v7 declared={type=Generator flags=[IsExpected]}
    v9 = IteratorOpen v8
    Jump -> next b2(v3)
  b2(v10):
    LoopHint
    v12 = IteratorResume v9
    v13 = ResultAt v12
    v14 = ResultAt v12 index=1
    Branch v13 -> taken b3(), notTaken b4()
  b3():
    v15 = Convert v10 declared={type=number flags=[IsExpected]}
    Return v15
  b4():
    Jump -> next b5()
  b5() handler=b7():
    v16 = Add v10, v14
    v17 = Convert v16 declared={type=number flags=[IsExpected]}
    Jump -> next b6()
  b6() handler=b7():
    Jump -> next b2(v17)
  b7() role=IterationCleanup subject=v9:
    v18 = CatchException
    IteratorClose v9 completion=Throw
    Rethrow v18
}
function f4 name="open" module=m0 kind=Method flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=boolean|"opened" flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    Jump -> next b2()
  b2() handler=b3():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0
    v3 = GetModuleVariable variable=g2
    v4 = Integer integer=1
    v5 = Call v3, v0, v4 calleeName="#each"
    v6 = String string="opened"
    v7 = Convert v6 declared={type=boolean|"opened" flags=[IsExpected]}
    Return v7
  b3() role=CatchHandler:
    v8 = CatchValue
    v9 = Builtin builtin=GlobalThis
    v10 = PropGet v9 key="TypeError" [isFreeName]
    v11 = Convert v10 declared={type=builtin(TypeError) flags=[IsExpected]}
    v12 = InstanceOf v8, v11
    v13 = Convert v12 declared={type=boolean|"opened" flags=[IsExpected]}
    Return v13
}
function f5 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L0 flags=[IsFromCompiler]} results=[{type=anything}] {
  b0(v0: L0):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    PrivateDefine v0, v1, v1 layout=L0
    v3 = Undefined
    Return v3
}
data d0 root=Object{layout=L1} module=m0
entry module=m0
)IR"_s;

static constexpr ASCIILiteral besideFieldsAndStatics = R"IR(layout L0 kind=Instance name="C" module=m0 constructor=f1 fields=[{key=#"#"@L0 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}, {key=#"#x"@L0 declared={type=number flags=[IsExpected]} flags=[IsRequired, IsPrivate]}, {key=#"#y"@L0 declared={type=number flags=[IsExpected]} flags=[IsRequired, IsPrivate]}]
layout L1 kind=Instance name="D" module=m0 constructor=f6
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=classtoken} flags=[IsImmutable]
variable g2 module=m0 declared={type=anything} flags=[IsImmutable]
variable g3 module=m0 declared={type=anything} flags=[IsImmutable]
variable g4 module=m0 declared={type=anything} flags=[IsImmutable]
variable g5 name="D" module=m0 declared={type=function|absent} flags=[IsImmutable]
variable g6 module=m0 declared={type=function} flags=[IsImmutable]
variable g7 module=m0 declared={type=string|absent flags=[IsExpected]}
variable g8 module=m0 declared={type=anything} flags=[IsImmutable]
variable g9 module=m0 declared={type=anything} flags=[IsImmutable]
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
    DefineMethod v3, v11 layout=L0 key="all"
    v13 = FinishClass v3 layout=L0
    v14 = Convert v13 declared={type=function flags=[IsExpected]}
    v15 = Convert v13 declared={type=function flags=[IsExpected]}
    v16 = NewClosure function=f6
    v17 = NewClass v16 layout=L1
    PutModuleVariable v17 variable=g6
    v19 = NewClosure function=f7
    PutModuleVariable v19 variable=g8
    v21 = NewClosure function=f8
    PutModuleVariable v21 variable=g9
    v23 = NewClosure function=f9
    DefineMethod v17, v23 layout=L1 side=Static key="all"
    v25 = NewClosure function=f10
    DefineMethod v17, v25 layout=L1 side=Static index=1 key="onOther"
    v27 = FinishClass v17 layout=L1
    v28 = Convert v27 declared={type=function flags=[IsExpected]}
    PutModuleVariable v28 variable=g5
    v30 = Convert v27 declared={type=function flags=[IsExpected]}
    v31 = CallDirect v30 function=f11
    v32 = Convert v27 declared={type=function flags=[IsExpected]}
    v33 = Builtin builtin=GlobalThis
    v34 = PropGet v33 key="print" [isFreeName]
    v35 = Construct v15 calleeName="C"
    v36 = Convert v35 declared={type=L0 flags=[IsExpected]}
    v37 = PropGet v36 key="all"
    v38 = Convert v37 declared={type=function flags=[IsExpected]}
    v39 = Call v38, v36 calleeKey="all" calleeName="all"
    v40 = Convert v39 declared={type=string flags=[IsExpected]}
    v41 = Call v34, -, v40 calleeName="print"
    v42 = Builtin builtin=GlobalThis
    v43 = PropGet v42 key="print" [isFreeName]
    v44 = PropGet v32 key="all"
    v45 = Convert v44 declared={type=function flags=[IsExpected]}
    v46 = Call v45, v32 calleeKey="all" calleeName="all"
    v47 = Convert v46 declared={type=string flags=[IsExpected]}
    v48 = Call v43, -, v47 calleeName="print"
    v49 = Builtin builtin=GlobalThis
    v50 = PropGet v49 key="print" [isFreeName]
    v51 = PropGet v32 key="onOther"
    v52 = Convert v51 declared={type=function flags=[IsExpected]}
    v53 = Construct v15 calleeName="C"
    v54 = Convert v53 declared={type=L0 flags=[IsExpected]}
    v55 = Call v52, v32, v54 calleeKey="onOther" calleeName="onOther"
    v56 = Call v50, -, v55 calleeName="print"
    v57 = Undefined
    Return v57
}
function f1 name="C" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Convert v2 declared={type=L0 flags=[IsFromCompiler]} illTypedCause=FromLooseType
    v4 = CallDirect v3 function=f12
    v5 = Convert v2 declared={type=L0 flags=[IsExpected]}
    v6 = Convert v2 declared={type=L0 flags=[IsExpected]}
    v7 = GetModuleVariable variable=g1
    v8 = PrivateGet v6, v7 layout=L0
    v9 = GetModuleVariable variable=g2
    v10 = Call v9, v6 calleeName="#m"
    v11 = Convert v10 declared={type=number flags=[IsExpected]}
    v12 = GetModuleVariable variable=g1
    PrivateSet v5, v12, v11 layout=L0 field=2
    v14 = Undefined
    v15 = TypeTest v14 testedType=object
    Branch v15 -> taken b2(| 0), notTaken b3(| 0)
  b2(v16):
    Jump -> next b4(v16)
  b3(v17):
    Jump -> next b4(v2)
  b4(v18):
    Return v18
}
function f2 name="#m" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=number flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0 field=1
    v3 = Convert v2 declared={type=number flags=[IsExpected]}
    v4 = Integer integer=10
    v5 = Add v3, v4
    v6 = Convert v5 declared={type=number flags=[IsExpected]}
    Return v6
}
function f3 name="#g" module=m0 kind=Getter flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=number flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0 field=1
    v3 = Convert v2 declared={type=number flags=[IsExpected]}
    v4 = Integer integer=100
    v5 = Add v3, v4
    v6 = Convert v5 declared={type=number flags=[IsExpected]}
    Return v6
}
function f4 name="#g" module=m0 kind=Setter flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=number flags=[IsExpected]}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = Convert v1 declared={type=number flags=[IsExpected]}
    v3 = GetModuleVariable variable=g1
    PrivateSet v0, v3, v2 layout=L0 field=1
    v5 = Undefined
    Return v5
}
function f5 name="all" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=string flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = Integer integer=5
    v2 = GetModuleVariable variable=g1
    v3 = PrivateGet v0, v2 layout=L0
    v4 = GetModuleVariable variable=g4
    v5 = Call v4, v0, v1
    v6 = GetModuleVariable variable=g1
    v7 = PrivateGet v0, v6 layout=L0 field=1
    v8 = Convert v7 declared={type=number flags=[IsExpected]}
    v9 = String string=":"
    v10 = Add v8, v9
    v11 = GetModuleVariable variable=g1
    v12 = PrivateGet v0, v11 layout=L0 field=2
    v13 = Convert v12 declared={type=number flags=[IsExpected]}
    v14 = Add v10, v13
    v15 = String string=":"
    v16 = Add v14, v15
    v17 = GetModuleVariable variable=g1
    v18 = PrivateGet v0, v17 layout=L0
    v19 = GetModuleVariable variable=g2
    v20 = Call v19, v0 calleeName="#m"
    v21 = Convert v20 declared={type=number flags=[IsExpected]}
    v22 = Add v16, v21
    v23 = String string=":"
    v24 = Add v22, v23
    v25 = GetModuleVariable variable=g1
    v26 = PrivateGet v0, v25 layout=L0
    v27 = GetModuleVariable variable=g3
    v28 = Call v27, v0
    v29 = Convert v28 declared={type=number flags=[IsExpected]}
    v30 = Add v24, v29
    v31 = Convert v30 declared={type=string flags=[IsExpected]}
    Return v31
}
function f6 name="D" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L1
    v2 = NewObject v1 layout=L1
    Return v2
}
function f7 name="#m" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = String string="static #m"
    v1 = Convert v0 declared={type=string flags=[IsExpected]}
    Return v1
}
function f8 name="#g" module=m0 kind=Getter flags=[HasExplicitName] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = String string="static #g"
    v1 = Convert v0 declared={type=string flags=[IsExpected]}
    Return v1
}
function f9 name="all" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = GetModuleVariable variable=g5
    v1 = CheckInitialized v0 name="D"
    v2 = GetModuleVariable variable=g6
    v3 = StrictEq v1, v2
    Branch v3 -> taken b2(), notTaken b9()
  b2():
    v4 = GetModuleVariable variable=g8
    v5 = Call v4, v1 calleeName="#m"
    v6 = Convert v5 declared={type=string flags=[IsExpected]}
    v7 = String string=", "
    v8 = Add v6, v7
    v9 = GetModuleVariable variable=g5
    v10 = CheckInitialized v9 name="D"
    v11 = GetModuleVariable variable=g6
    v12 = StrictEq v10, v11
    Branch v12 -> taken b3(), notTaken b8()
  b3():
    v13 = GetModuleVariable variable=g7
    v14 = TypeTest v13 testedType=absent
    Branch v14 -> taken b4(| 0), notTaken b5(| 0)
  b4(v15) frequency=Unlikely:
    v16 = NewRuntimeError runtimeError=InvalidPrivateRead
    Throw v16
  b5(v17):
    v18 = Convert v17 declared={type=string flags=[IsExpected]}
    v19 = Add v8, v18
    v20 = String string=", "
    v21 = Add v19, v20
    v22 = GetModuleVariable variable=g5
    v23 = CheckInitialized v22 name="D"
    v24 = GetModuleVariable variable=g6
    v25 = StrictEq v23, v24
    Branch v25 -> taken b6(), notTaken b7()
  b6():
    v26 = GetModuleVariable variable=g9
    v27 = Call v26, v23
    v28 = Convert v27 declared={type=string flags=[IsExpected]}
    v29 = Add v21, v28
    v30 = Convert v29 declared={type=string flags=[IsExpected]}
    Return v30
  b7() frequency=Unlikely:
    v31 = NewRuntimeError runtimeError=InvalidPrivateBrand
    Throw v31
  b8() frequency=Unlikely:
    v32 = NewRuntimeError runtimeError=InvalidPrivateRead
    Throw v32
  b9() frequency=Unlikely:
    v33 = NewRuntimeError runtimeError=InvalidPrivateBrand
    Throw v33
}
function f10 name="onOther" module=m0 kind=Method flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    Jump -> next b2()
  b2() handler=b5():
    v1 = GetModuleVariable variable=g6
    v2 = StrictEq v0, v1
    Branch v2 -> taken b3(), notTaken b4()
  b3() handler=b5():
    v3 = GetModuleVariable variable=g8
    v4 = Call v3, v0 calleeName="#m"
    Return v4
  b4() frequency=Unlikely handler=b5():
    v5 = NewRuntimeError runtimeError=InvalidPrivateBrand
    Throw v5
  b5() role=CatchHandler:
    v6 = CatchValue
    v7 = Builtin builtin=GlobalThis
    v8 = PropGet v7 key="TypeError" [isFreeName]
    v9 = Convert v8 declared={type=builtin(TypeError) flags=[IsExpected]}
    v10 = InstanceOf v6, v9
    Return v10
}
function f11 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=function} results=[{type=anything}] {
  b0(v0: function):
    Jump -> next b1()
  b1():
    v1 = String string="static #x"
    v2 = Convert v1 declared={type=string flags=[IsExpected]}
    PutModuleVariable v2 variable=g7
    v4 = Undefined
    Return v4
}
function f12 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L0 flags=[IsFromCompiler]} results=[{type=anything}] {
  b0(v0: L0):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g1
    PrivateDefine v0, v1, v1 layout=L0
    v3 = Integer integer=1
    v4 = Convert v3 declared={type=number flags=[IsExpected]}
    v5 = GetModuleVariable variable=g1
    PrivateDefine v0, v5, v4 layout=L0 field=1
    v7 = Undefined
    v8 = GetModuleVariable variable=g1
    PrivateDefine v0, v8, v7 layout=L0 field=2
    v10 = Undefined
    Return v10
}
entry module=m0
)IR"_s;

static void testAsyncMethods()
{
    checkPrivateNameProgram(asyncMethods, "true\nend of module\nlater 15\ncollected 10,11,12,\n"_s);
}

static void testGeneratorMethods()
{
    checkPrivateNameProgram(generatorMethods, "33\nopened\ntrue\n"_s);
}

static void testBesideFieldsAndStatics()
{
    checkPrivateNameProgram(besideFieldsAndStatics, "5:11:15:105\nstatic #m, static #x, static #g\ntrue\n"_s);
}

std::span<const NativeTest> privateMemberKindProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "private-member-kind-programs/async-methods"_s, testAsyncMethods },
        { "private-member-kind-programs/generator-methods"_s, testGeneratorMethods },
        { "private-member-kind-programs/beside-fields-and-statics"_s, testBesideFieldsAndStatics },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
