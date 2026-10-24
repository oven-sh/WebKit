/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTCompiledStructure.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral functionExpression = R"IR(layout L0 kind=Instance constructor=f1 fields=[{key="px" declared={type=1 flags=[IsExpected]}}] flags=[IsConstructorFunction]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = PropGet v0 key="prototype"
    v2 = NewClosure function=f2
    PropSet v1, v2 key="sum"
    v4 = Builtin builtin=GlobalThis
    v5 = PropGet v4 key="print" [isFreeName]
    v6 = Construct v0
    v7 = PropGet v6 key="sum"
    v8 = Call v7, v6
    v9 = Call v5, -, v8
    v10 = Construct v0
    v11 = InstanceOf v10, v0
    v12 = Call v5, -, v11
    v13 = PropGet v4 key="Object" [isFreeName]
    v14 = PropGet v13 key="getPrototypeOf"
    v15 = Construct v0
    v16 = Call v14, v13, v15
    v17 = PropGet v0 key="prototype"
    v18 = StrictEq v16, v17
    v19 = Call v5, -, v18
    v20 = Construct v0
    v21 = PropGet v20 key="constructor"
    v22 = StrictEq v21, v0
    v23 = Call v5, -, v22
    v24 = PropGet v0 key="prototype"
    v25 = PropGet v24 key="constructor"
    v26 = StrictEq v25, v0
    v27 = Call v5, -, v26
    v28 = Undefined
    Return v28
}
function f1 name="Point" flags=[HasThis] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    PropSet v0, v1 key="px"
    v3 = Undefined
    Return v3
}
function f2 flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="px"
    Return v1
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral bodiesWithoutThis = R"IR(layout L0 kind=Instance name="Declared" constructor=f1 flags=[IsConstructorFunction]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = PropGet v0 key="prototype"
    v2 = NewClosure function=f2
    PropSet v1, v2 key="m"
    v4 = PropGet v0 key="prototype"
    v5 = Integer integer=3
    PropSet v4, v5 key="x"
    v7 = Builtin builtin=GlobalThis
    v8 = PropGet v7 key="print" [isFreeName]
    v9 = Construct v0
    v10 = PropGet v9 key="m"
    v11 = Call v10, v9
    v12 = Construct v0
    v13 = PropGet v12 key="x"
    v14 = PropGet v7 key="Object" [isFreeName]
    v15 = PropGet v14 key="getPrototypeOf"
    v16 = Construct v0
    v17 = Call v15, v14, v16
    v18 = PropGet v0 key="prototype"
    v19 = StrictEq v17, v18
    v20 = Call v8, -, v11, v13, v19
    v21 = NewClosure function=f3
    v22 = PropGet v21 key="prototype"
    v23 = NewClosure function=f4
    PropSet v22, v23 key="m"
    v25 = PropGet v21 key="prototype"
    v26 = Integer integer=4
    PropSet v25, v26 key="x"
    v28 = Construct v21
    v29 = PropGet v28 key="m"
    v30 = Call v29, v28
    v31 = Construct v21
    v32 = PropGet v31 key="x"
    v33 = PropGet v14 key="getPrototypeOf"
    v34 = Construct v21
    v35 = Call v33, v14, v34
    v36 = PropGet v21 key="prototype"
    v37 = StrictEq v35, v36
    v38 = Call v8, -, v30, v32, v37
    v39 = PropGet v0 key="prototype"
    v40 = NewClosure function=f5
    PropSet v39, v40 key="valueOf"
    v42 = PropGet v21 key="prototype"
    v43 = NewClosure function=f6
    PropSet v42, v43 key="toString"
    v45 = Construct v0
    v46 = Integer integer=1
    v47 = Add v45, v46
    v48 = String string=""
    v49 = Construct v21
    v50 = Add v48, v49
    v51 = Call v8, -, v47, v50
    v52 = Undefined
    Return v52
}
function f1 name="Declared" flags=[HasExplicitName] thisType={type=anything} results=[{type=anything}] {
    v0 = Undefined
    Return v0
}
function f2 thisType={type=anything} results=[{type=number flags=[IsExpected]}] {
    v0 = Integer integer=7
    Return v0
}
function f3 name="Expression" thisType={type=anything} results=[{type=anything}] {
    v0 = Undefined
    Return v0
}
function f4 thisType={type=anything} results=[{type=number flags=[IsExpected]}] {
    v0 = Integer integer=8
    Return v0
}
function f5 thisType={type=anything} results=[{type=number flags=[IsExpected]}] {
    v0 = Integer integer=5
    Return v0
}
function f6 thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v0 = String string="text"
    Return v0
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral classBelowAFunctionExpression = R"IR(layout L0 kind=Instance constructor=f1 fields=[{key="px" declared={type=1 flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L1 kind=Instance name="Sub" constructor=f3 fields=[{key="py" declared={type=2 flags=[IsExpected]}}] flags=[HasHeritage]
variable g0 declared={type=function} flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = PropGet v0 key="prototype"
    v2 = NewClosure function=f2
    PropSet v1, v2 key="sum"
    v4 = ResolveHeritage v0
    v5 = NewClosure function=f3
    v6 = NewClass v5, v0, v4 layout=L1
    PutModuleVariable v6 variable=g0
    v8 = NewClosure function=f4
    DefineMethod v6, v8 layout=L1 key="both"
    v10 = FinishClass v6 layout=L1
    v11 = Builtin builtin=GlobalThis
    v12 = PropGet v11 key="print" [isFreeName]
    v13 = PropGet v11 key="Object" [isFreeName]
    v14 = PropGet v13 key="getPrototypeOf"
    v15 = Call v14, v13, v10
    v16 = StrictEq v15, v0
    v17 = Call v12, -, v16
    v18 = PropGet v13 key="getPrototypeOf"
    v19 = PropGet v10 key="prototype"
    v20 = Call v18, v13, v19
    v21 = PropGet v0 key="prototype"
    v22 = StrictEq v20, v21
    v23 = Call v12, -, v22
    v24 = Construct v10
    v25 = PropGet v24 key="sum"
    v26 = Call v25, v24
    v27 = Construct v10
    v28 = PropGet v27 key="both"
    v29 = Call v28, v27
    v30 = Call v12, -, v26, v29
    v31 = Construct v10
    v32 = InstanceOf v31, v0
    v33 = Construct v10
    v34 = InstanceOf v33, v10
    v35 = Construct v0
    v36 = InstanceOf v35, v10
    v37 = Call v12, -, v32, v34, v36
    v38 = Undefined
    Return v38
}
function f1 name="Point" flags=[HasThis] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    PropSet v0, v1 key="px"
    v3 = Undefined
    Return v3
}
function f2 flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="px"
    Return v1
}
function f3 name="Sub" kind=DerivedConstructor flags=[HasNewTarget, TakesCallee, HasExplicitName] thisType={type=anything} results=[{type=object}] {
    v2 = Absent
    v3 = GetSuperBase v0
    v4 = SuperConstruct v3, v1 layout=L1
    v5 = TypeTest v2 testedType=absent
    Branch v5 -> taken b1(| 0), notTaken b11(| 0)
  b1(v6):
    v7 = TypeTest v4 testedType=absent
    Branch v7 -> taken b2(| 0), notTaken b3(| 0)
  b2(v8) frequency=Unlikely:
    v9 = NewRuntimeError runtimeError=UninitializedThis
    Throw v9
  b3(v10):
    v11 = Integer integer=2
    PropSet v10, v11 key="py"
    v13 = Undefined
    v14 = TypeTest v13 testedType=object
    Branch v14 -> taken b4(| 0), notTaken b5(| 0)
  b4(v15):
    Jump -> next b9(v15)
  b5(v16):
    v17 = TypeTest v16 testedType=undefined
    Branch v17 -> taken b6(| 0), notTaken b10(| 0)
  b6(v18):
    v19 = TypeTest v10 testedType=absent
    Branch v19 -> taken b7(| 0), notTaken b8(| 0)
  b7(v20) frequency=Unlikely:
    v21 = NewRuntimeError runtimeError=UninitializedThis
    Throw v21
  b8(v22):
    Jump -> next b9(v22)
  b9(v23):
    Return v23
  b10(v24) frequency=Unlikely:
    v25 = NewRuntimeError runtimeError=DerivedConstructorResult name="Sub"
    Throw v25
  b11(v26) frequency=Unlikely:
    v27 = NewRuntimeError runtimeError=ThisAlreadyBound
    Throw v27
}
function f4 name="both" kind=Method flags=[HasThis, HasExplicitName] thisType={type=L1 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="sum"
    v2 = Call v1, v0
    v3 = PropGet v0 key="py"
    v4 = Add v2, v3
    Return v4
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral declaredInAFunctionThatIsCalledAtOnce = R"IR(layout L0 kind=Instance name="Point" constructor=f2 fields=[{key="px" declared={type=anything}}] flags=[IsConstructorFunction]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = Call v0
    v2 = Builtin builtin=GlobalThis
    v3 = PropGet v2 key="print" [isFreeName]
    v4 = Integer integer=1
    v5 = Construct v1, -, v4
    v6 = PropGet v5 key="sum"
    v7 = Call v6, v5
    v8 = PropGet v1 key="make"
    v9 = Integer integer=2
    v10 = Call v8, v1, v9
    v11 = PropGet v10 key="sum"
    v12 = Call v11, v10
    v13 = Integer integer=3
    v14 = Construct v1, -, v13
    v15 = PropGet v14 key="clone"
    v16 = Call v15, v14
    v17 = PropGet v16 key="sum"
    v18 = Call v17, v16
    v19 = Call v3, -, v7, v12, v18
    v20 = Integer integer=3
    v21 = Construct v1, -, v20
    v22 = InstanceOf v21, v1
    v23 = PropGet v1 key="make"
    v24 = Integer integer=4
    v25 = Call v23, v1, v24
    v26 = InstanceOf v25, v1
    v27 = Integer integer=5
    v28 = Construct v1, -, v27
    v29 = PropGet v28 key="clone"
    v30 = Call v29, v28
    v31 = InstanceOf v30, v1
    v32 = Call v3, -, v22, v26, v31
    v33 = PropGet v2 key="Object" [isFreeName]
    v34 = PropGet v33 key="getPrototypeOf"
    v35 = PropGet v1 key="make"
    v36 = Integer integer=6
    v37 = Call v35, v1, v36
    v38 = Call v34, v33, v37
    v39 = PropGet v1 key="prototype"
    v40 = StrictEq v38, v39
    v41 = Integer integer=7
    v42 = Construct v1, -, v41
    v43 = PropGet v42 key="constructor"
    v44 = StrictEq v43, v1
    v45 = Call v3, -, v40, v44
    v46 = Undefined
    Return v46
}
function f1 thisType={type=anything} results=[{type=function flags=[IsExpected]}] {
    v0 = NewClosure function=f2
    v1 = PropGet v0 key="prototype"
    v2 = NewClosure function=f3
    PropSet v1, v2 key="sum"
    v4 = PropGet v0 key="prototype"
    v5 = NewClosure v0 function=f4
    PropSet v4, v5 key="clone"
    v7 = NewClosure v0 function=f5
    PropSet v0, v7 key="make"
    Return v0
}
function f2 name="Point" flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    PropSet v0, v1 key="px"
    v3 = Undefined
    Return v3
}
function f3 flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="px"
    v2 = Integer integer=1
    v3 = Add v1, v2
    Return v3
}
function f4 flags=[HasThis] captures=[{type=function}] thisType={type=anything} results=[{type=anything}] {
    v2 = PropGet v1 key="px"
    v3 = Construct v0, -, v2
    Return v3
}
function f5 length=1 captures=[{type=function}] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    v2 = Construct v0, -, v1
    Return v2
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral functionThatConstructsItself = R"IR(layout L0 kind=Instance name="Both" constructor=f1 fields=[{key="v" declared={type=anything}}] flags=[IsConstructorFunction]
layout L1 kind=Environment fields=[{key=@"binding.0" declared={type=function} flags=[IsImmutable]}]
layout L2 kind=Instance name="Wrapped" constructor=f4 fields=[{key="v" declared={type=anything}}] flags=[IsConstructorFunction]
variable g0 name="Both" declared={type=function} initialization=Function function=f1 flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = GetModuleVariable variable=g0
    v1 = PropGet v0 key="prototype"
    v2 = NewClosure function=f2
    PropSet v1, v2 key="get"
    v4 = Builtin builtin=GlobalThis
    v5 = PropGet v4 key="print" [isFreeName]
    v6 = GetModuleVariable variable=g0
    v7 = Integer integer=1
    v8 = Call v6, -, v7
    v9 = PropGet v8 key="get"
    v10 = Call v9, v8
    v11 = GetModuleVariable variable=g0
    v12 = Integer integer=2
    v13 = Construct v11, -, v12
    v14 = PropGet v13 key="get"
    v15 = Call v14, v13
    v16 = GetModuleVariable variable=g0
    v17 = Integer integer=3
    v18 = Call v16, -, v17
    v19 = GetModuleVariable variable=g0
    v20 = InstanceOf v18, v19
    v21 = GetModuleVariable variable=g0
    v22 = Integer integer=4
    v23 = Call v21, -, v22
    v24 = PropGet v23 key="constructor"
    v25 = GetModuleVariable variable=g0
    v26 = StrictEq v24, v25
    v27 = Call v5, -, v10, v15, v20, v26
    v28 = NewClosure function=f3
    v29 = Call v28
    v30 = Integer integer=1
    v31 = Call v29, -, v30
    v32 = PropGet v31 key="get"
    v33 = Call v32, v31
    v34 = Integer integer=2
    v35 = Construct v29, -, v34
    v36 = PropGet v35 key="get"
    v37 = Call v36, v35
    v38 = Integer integer=3
    v39 = Call v29, -, v38
    v40 = InstanceOf v39, v29
    v41 = Integer integer=4
    v42 = Call v29, -, v41
    v43 = PropGet v42 key="constructor"
    v44 = StrictEq v43, v29
    v45 = Call v5, -, v33, v37, v40, v44
    v46 = Undefined
    Return v46
}
function f1 name="Both" flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = GetModuleVariable variable=g0
    v3 = InstanceOf v0, v2
    v4 = ToBoolean v3
    Branch v4 -> taken b1(| 0), notTaken b3(| 0)
  b1(v5):
    Jump -> next b2()
  b2():
    PropSet v0, v1 key="v"
    v7 = Undefined
    Return v7
  b3(v8):
    Jump -> next b4()
  b4():
    v9 = GetModuleVariable variable=g0
    v10 = Construct v9, -, v1
    Return v10
}
function f2 flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="v"
    Return v1
}
function f3 thisType={type=anything} results=[{type=function flags=[IsExpected]}] {
    v0 = NewObject layout=L1
    v1 = NewClosure v0 function=f4
    PutEnvironmentField v0, v1 layout=L1
    v3 = PropGet v1 key="prototype"
    v4 = NewClosure function=f5
    PropSet v3, v4 key="get"
    Return v1
}
function f4 name="Wrapped" flags=[HasThis, HasExplicitName] length=1 captures=[{type=env L1}] thisType={type=L2 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: env L1, v1: anything, v2: anything):
    v3 = GetEnvironmentField v0 layout=L1
    v4 = InstanceOf v1, v3
    v5 = ToBoolean v4
    Branch v5 -> taken b1(| 0), notTaken b3(| 0)
  b1(v6):
    Jump -> next b2()
  b2():
    PropSet v1, v2 key="v"
    v8 = Undefined
    Return v8
  b3(v9):
    Jump -> next b4()
  b4():
    v10 = GetEnvironmentField v0 layout=L1
    v11 = Construct v10, -, v2
    Return v11
}
function f5 flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="v"
    Return v1
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral declaredInAFunctionCalledOnceAndTwice = R"IR(layout L0 kind=Instance name="Inner" constructor=f3 fields=[{key="v" declared={type=1 flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L1 kind=Instance name="Inner" constructor=f5 fields=[{key="v" declared={type=2 flags=[IsExpected]}}] flags=[IsConstructorFunction]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = NewClosure function=f2
    v2 = Call v0
    v3 = Call v1
    v4 = Call v1
    v5 = Builtin builtin=GlobalThis
    v6 = PropGet v5 key="print" [isFreeName]
    v7 = Construct v2
    v8 = PropGet v7 key="get"
    v9 = Call v8, v7
    v10 = Construct v3
    v11 = PropGet v10 key="get"
    v12 = Call v11, v10
    v13 = Construct v4
    v14 = PropGet v13 key="get"
    v15 = Call v14, v13
    v16 = Call v6, -, v9, v12, v15
    v17 = Construct v2
    v18 = InstanceOf v17, v2
    v19 = Construct v3
    v20 = InstanceOf v19, v3
    v21 = Construct v3
    v22 = InstanceOf v21, v4
    v23 = StrictEq v3, v4
    v24 = Call v6, -, v18, v20, v22, v23
    v25 = Undefined
    Return v25
}
function f1 name="once" flags=[HasExplicitName] thisType={type=anything} results=[{type=function flags=[IsExpected]}] {
    v0 = NewClosure function=f3
    v1 = PropGet v0 key="prototype"
    v2 = NewClosure function=f4
    PropSet v1, v2 key="get"
    Return v0
}
function f2 name="twice" flags=[HasExplicitName] thisType={type=anything} results=[{type=function flags=[IsExpected]}] {
    v0 = NewClosure function=f5
    v1 = PropGet v0 key="prototype"
    v2 = NewClosure function=f6
    PropSet v1, v2 key="get"
    Return v0
}
function f3 name="Inner" flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    PropSet v0, v1 key="v"
    v3 = Undefined
    Return v3
}
function f4 flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="v"
    Return v1
}
function f5 name="Inner" flags=[HasThis, HasExplicitName] thisType={type=L1 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=2
    PropSet v0, v1 key="v"
    v3 = Undefined
    Return v3
}
function f6 flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="v"
    Return v1
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral factoryWithACapture = R"IR(layout L0 kind=Instance name="P" constructor=f4 fields=[{key="tag" declared={type=anything}}] flags=[IsConstructorFunction]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = String string="a"
    v2 = Call v0, -, v1
    v3 = String string="b"
    v4 = Call v0, -, v3
    v5 = PropGet v2 key="prototype"
    v6 = NewClosure function=f2
    PropSet v5, v6 key="who"
    v8 = PropGet v4 key="prototype"
    v9 = NewClosure function=f3
    PropSet v8, v9 key="who"
    v11 = Builtin builtin=GlobalThis
    v12 = PropGet v11 key="print" [isFreeName]
    v13 = Construct v2
    v14 = PropGet v13 key="who"
    v15 = Call v14, v13
    v16 = Construct v4
    v17 = PropGet v16 key="who"
    v18 = Call v17, v16
    v19 = Call v12, -, v15, v18
    v20 = Construct v2
    v21 = InstanceOf v20, v2
    v22 = Construct v2
    v23 = InstanceOf v22, v4
    v24 = Construct v4
    v25 = InstanceOf v24, v2
    v26 = Construct v4
    v27 = InstanceOf v26, v4
    v28 = Call v12, -, v21, v23, v25, v27
    v29 = PropGet v2 key="prototype"
    v30 = PropGet v4 key="prototype"
    v31 = StrictEq v29, v30
    v32 = StrictEq v2, v4
    v33 = Call v12, -, v31, v32
    v34 = PropGet v11 key="Object" [isFreeName]
    v35 = PropGet v34 key="getPrototypeOf"
    v36 = Construct v2
    v37 = Call v35, v34, v36
    v38 = PropGet v2 key="prototype"
    v39 = StrictEq v37, v38
    v40 = PropGet v34 key="getPrototypeOf"
    v41 = Construct v4
    v42 = Call v40, v34, v41
    v43 = PropGet v4 key="prototype"
    v44 = StrictEq v42, v43
    v45 = Call v12, -, v39, v44
    v46 = Undefined
    Return v46
}
function f1 name="make" flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=function flags=[IsExpected]}] {
    v1 = NewClosure v0 function=f4
    Return v1
}
function f2 flags=[HasThis] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v1 = String string="A"
    v2 = PropGet v0 key="tag"
    v3 = Add v1, v2
    Return v3
}
function f3 flags=[HasThis] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v1 = String string="B"
    v2 = PropGet v0 key="tag"
    v3 = Add v1, v2
    Return v3
}
function f4 name="P" flags=[HasThis, HasExplicitName] captures=[{type=anything}] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    PropSet v1, v0 key="tag"
    v3 = Undefined
    Return v3
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral factoryWithoutCaptures = R"IR(layout L0 kind=Instance name="P" constructor=f4 fields=[{key="tag" declared={type=1 flags=[IsExpected]}}] flags=[IsConstructorFunction]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = Call v0
    v2 = Call v0
    v3 = PropGet v1 key="prototype"
    v4 = NewClosure function=f2
    PropSet v3, v4 key="who"
    v6 = PropGet v2 key="prototype"
    v7 = NewClosure function=f3
    PropSet v6, v7 key="who"
    v9 = Builtin builtin=GlobalThis
    v10 = PropGet v9 key="print" [isFreeName]
    v11 = Construct v1
    v12 = PropGet v11 key="who"
    v13 = Call v12, v11
    v14 = Construct v2
    v15 = PropGet v14 key="who"
    v16 = Call v15, v14
    v17 = Call v10, -, v13, v16
    v18 = Construct v1
    v19 = InstanceOf v18, v1
    v20 = Construct v1
    v21 = InstanceOf v20, v2
    v22 = Construct v2
    v23 = InstanceOf v22, v1
    v24 = Construct v2
    v25 = InstanceOf v24, v2
    v26 = Call v10, -, v19, v21, v23, v25
    v27 = PropGet v1 key="prototype"
    v28 = PropGet v2 key="prototype"
    v29 = StrictEq v27, v28
    v30 = StrictEq v1, v2
    v31 = Call v10, -, v29, v30
    v32 = PropGet v9 key="Object" [isFreeName]
    v33 = PropGet v32 key="getPrototypeOf"
    v34 = Construct v1
    v35 = Call v33, v32, v34
    v36 = PropGet v1 key="prototype"
    v37 = StrictEq v35, v36
    v38 = PropGet v32 key="getPrototypeOf"
    v39 = Construct v2
    v40 = Call v38, v32, v39
    v41 = PropGet v2 key="prototype"
    v42 = StrictEq v40, v41
    v43 = Call v10, -, v37, v42
    v44 = Undefined
    Return v44
}
function f1 name="make" flags=[HasExplicitName] thisType={type=anything} results=[{type=function flags=[IsExpected]}] {
    v0 = NewClosure function=f4
    Return v0
}
function f2 flags=[HasThis] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v1 = String string="A"
    v2 = PropGet v0 key="tag"
    v3 = Add v1, v2
    Return v3
}
function f3 flags=[HasThis] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v1 = String string="B"
    v2 = PropGet v0 key="tag"
    v3 = Add v1, v2
    Return v3
}
function f4 name="P" flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    PropSet v0, v1 key="tag"
    v3 = Undefined
    Return v3
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral createdInALoop = R"IR(layout L0 kind=Instance constructor=f1 fields=[{key="n" declared={type=10 flags=[IsExpected]}}] flags=[IsConstructorFunction]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = Data data=d0
    v1 = Integer
    Jump -> next b1(v1)
  b1(v2):
    LoopHint
    v4 = Integer integer=3
    v5 = Less v2, v4
    Branch v5 -> taken b2(| 0), notTaken b5(| 0)
  b2(v6):
    Jump -> next b3()
  b3():
    v7 = NewClosure function=f1
    v8 = PropGet v7 key="prototype"
    PropSet v8, v6 key="index"
    ElemSet v0, v6, v7
    Jump -> next b4()
  b4():
    v11 = Increment v6
    Jump -> next b1(v11)
  b5(v12):
    Jump -> next b6()
  b6():
    v13 = Builtin builtin=GlobalThis
    v14 = PropGet v13 key="print" [isFreeName]
    v15 = Integer
    v16 = ElemGet v0, v15
    v17 = Construct v16
    v18 = PropGet v17 key="index"
    v19 = Integer integer=1
    v20 = ElemGet v0, v19
    v21 = Construct v20
    v22 = PropGet v21 key="index"
    v23 = Integer integer=2
    v24 = ElemGet v0, v23
    v25 = Construct v24
    v26 = PropGet v25 key="index"
    v27 = Call v14, -, v18, v22, v26
    v28 = Builtin builtin=GlobalThis
    v29 = PropGet v28 key="print" [isFreeName]
    v30 = Integer
    v31 = ElemGet v0, v30
    v32 = Construct v31
    v33 = Integer
    v34 = ElemGet v0, v33
    v35 = InstanceOf v32, v34
    v36 = Integer
    v37 = ElemGet v0, v36
    v38 = Construct v37
    v39 = Integer integer=1
    v40 = ElemGet v0, v39
    v41 = InstanceOf v38, v40
    v42 = Integer
    v43 = ElemGet v0, v42
    v44 = Integer integer=1
    v45 = ElemGet v0, v44
    v46 = StrictEq v43, v45
    v47 = Call v29, -, v35, v41, v46
    v48 = Undefined
    Return v48
}
function f1 name="P" flags=[HasThis] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=10
    PropSet v0, v1 key="n"
    v3 = Undefined
    Return v3
}
data d0 root=Array{contents=anything}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral prototypeReplacedBeforeTheFirstConstruction = R"IR(layout L0 kind=Instance constructor=f1 fields=[{key="px" declared={type=1 flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L1 fields=[{key="sum" declared={type=function flags=[IsExpected]} flags=[IsRequired, IsMethod]}]
layout L2 fields=[{key="sum" declared={type=function flags=[IsExpected]} flags=[IsRequired, IsMethod]}]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = NewObject layout=L1
    v2 = NewClosure function=f2
    PropDefine v1, v2 key="sum"
    v4 = FinishObject v1
    PropSet v0, v4 key="prototype"
    v6 = Construct v0
    v7 = Builtin builtin=GlobalThis
    v8 = PropGet v7 key="print" [isFreeName]
    v9 = PropGet v6 key="sum"
    v10 = Call v9, v6
    v11 = InstanceOf v6, v0
    v12 = PropGet v7 key="Object" [isFreeName]
    v13 = PropGet v12 key="getPrototypeOf"
    v14 = Call v13, v12, v6
    v15 = PropGet v0 key="prototype"
    v16 = StrictEq v14, v15
    v17 = Call v8, -, v10, v11, v16
    v18 = NewObject layout=L2
    v19 = NewClosure function=f3
    PropDefine v18, v19 key="sum"
    v21 = FinishObject v18
    PropSet v0, v21 key="prototype"
    v23 = Construct v0
    v24 = PropGet v6 key="sum"
    v25 = Call v24, v6
    v26 = PropGet v23 key="sum"
    v27 = Call v26, v23
    v28 = InstanceOf v6, v0
    v29 = InstanceOf v23, v0
    v30 = Call v8, -, v25, v27, v28, v29
    v31 = PropGet v12 key="getPrototypeOf"
    v32 = Call v31, v12, v6
    v33 = PropGet v0 key="prototype"
    v34 = StrictEq v32, v33
    v35 = PropGet v12 key="getPrototypeOf"
    v36 = Call v35, v12, v23
    v37 = PropGet v0 key="prototype"
    v38 = StrictEq v36, v37
    v39 = Call v8, -, v34, v38
    v40 = Integer integer=5
    PropSet v0, v40 key="prototype"
    v42 = PropGet v12 key="getPrototypeOf"
    v43 = Construct v0
    v44 = Call v42, v12, v43
    v45 = PropGet v12 key="prototype"
    v46 = StrictEq v44, v45
    v47 = Call v8, -, v46
    v48 = Undefined
    Return v48
}
function f1 name="Point" flags=[HasThis] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    PropSet v0, v1 key="px"
    v3 = Undefined
    Return v3
}
function f2 name="sum" kind=Method flags=[HasThis, HasExplicitName] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="px"
    v2 = Integer integer=10
    v3 = Add v1, v2
    Return v3
}
function f3 name="sum" kind=Method flags=[HasThis, HasExplicitName] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="px"
    v2 = Integer integer=20
    v3 = Add v1, v2
    Return v3
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral prototypeReplacedAfterTheFirstConstruction = R"IR(layout L0 kind=Instance constructor=f1 fields=[{key="px" declared={type=1 flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L1 fields=[{key="sum" declared={type=function flags=[IsExpected]} flags=[IsRequired, IsMethod]}]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = PropGet v0 key="prototype"
    v2 = NewClosure function=f2
    PropSet v1, v2 key="sum"
    v4 = Construct v0
    v5 = NewObject layout=L1
    v6 = NewClosure function=f3
    PropDefine v5, v6 key="sum"
    v8 = FinishObject v5
    PropSet v0, v8 key="prototype"
    v10 = Construct v0
    v11 = Builtin builtin=GlobalThis
    v12 = PropGet v11 key="print" [isFreeName]
    v13 = PropGet v4 key="sum"
    v14 = Call v13, v4
    v15 = PropGet v10 key="sum"
    v16 = Call v15, v10
    v17 = InstanceOf v4, v0
    v18 = InstanceOf v10, v0
    v19 = Call v12, -, v14, v16, v17, v18
    v20 = Undefined
    Return v20
}
function f1 name="Point" flags=[HasThis] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    PropSet v0, v1 key="px"
    v3 = Undefined
    Return v3
}
function f2 flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="px"
    Return v1
}
function f3 name="sum" kind=Method flags=[HasThis, HasExplicitName] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="px"
    v2 = Integer integer=20
    v3 = Add v1, v2
    Return v3
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral constructorWithACapture = R"IR(layout L0 kind=Environment fields=[{key=@"binding.0" declared={type=number|absent flags=[IsExpected]} flags=[IsRequired]}]
layout L1 kind=Instance constructor=f2 fields=[{key="id" declared={type=number flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L2 kind=Instance name="Sub" constructor=f4 flags=[HasHeritage]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = Call v0
    v4 = Call v2, -, v3
    v5 = Undefined
    Return v5
}
function f1 name="main" flags=[HasExplicitName] thisType={type=anything} results=[{type=number flags=[IsExpected]}] {
    v0 = Absent
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = Integer
    PutEnvironmentField v1, v2 layout=L0
    v4 = NewClosure v1 function=f2
    v5 = PropGet v4 key="prototype"
    v6 = NewClosure function=f3
    PropSet v5, v6 key="who"
    v8 = Builtin builtin=GlobalThis
    v9 = PropGet v8 key="print" [isFreeName]
    v10 = Construct v4
    v11 = PropGet v10 key="who"
    v12 = Call v11, v10
    v13 = Construct v4
    v14 = PropGet v13 key="who"
    v15 = Call v14, v13
    v16 = Call v9, -, v12, v15
    v17 = Construct v4
    v18 = InstanceOf v17, v4
    v19 = PropGet v8 key="Object" [isFreeName]
    v20 = PropGet v19 key="getPrototypeOf"
    v21 = Construct v4
    v22 = Call v20, v19, v21
    v23 = PropGet v4 key="prototype"
    v24 = StrictEq v22, v23
    v25 = Construct v4
    v26 = PropGet v25 key="constructor"
    v27 = StrictEq v26, v4
    v28 = Call v9, -, v18, v24, v27
    v29 = ResolveHeritage v4
    v30 = NewClosure function=f4
    v31 = NewClass v30, v4, v29 layout=L2
    v32 = NewClosure function=f5
    DefineMethod v31, v32 layout=L2 key="more"
    v34 = FinishClass v31 layout=L2
    v35 = Construct v34
    v36 = PropGet v35 key="more"
    v37 = Call v36, v35
    v38 = PropGet v19 key="getPrototypeOf"
    v39 = Call v38, v19, v34
    v40 = StrictEq v39, v4
    v41 = Construct v34
    v42 = InstanceOf v41, v4
    v43 = Call v9, -, v37, v40, v42
    v44 = GetEnvironmentField v1 layout=L0
    v45 = CheckInitialized v44 name="count"
    Return v45
}
function f2 name="Counter" flags=[HasThis] captures=[{type=env L0}] thisType={type=L1 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: env L0, v1: anything):
    v2 = GetEnvironmentField v0 layout=L0
    v3 = CheckInitialized v2 name="count"
    v4 = Increment v3
    v5 = GetEnvironmentField v0 layout=L0
    v6 = CheckInitialized v5 name="count"
    PutEnvironmentField v0, v4 layout=L0
    PropSet v1, v4 key="id"
    v9 = Undefined
    Return v9
}
function f3 flags=[HasThis] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v1 = String string="c"
    v2 = PropGet v0 key="id"
    v3 = Add v1, v2
    Return v3
}
function f4 name="Sub" kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee, HasExplicitName] thisType={type=anything} parameters=[{type=Array}] results=[{type=object}] {
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L2
    Return v4
}
function f5 name="more" kind=Method flags=[HasThis, HasExplicitName] thisType={type=L2 flags=[IsExpected]} results=[{type=string flags=[IsExpected]}] {
  b0(v0: anything):
    v1 = PropGet v0 key="who"
    v2 = Call v1, v0
    v3 = String string="!"
    v4 = Add v2, v3
    Return v4
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral constructorThatCapturesItsHolder = R"IR(layout L0 kind=Instance constructor=f2 fields=[{key="v" declared={type=anything}}, {key="owner" declared={type=L1 flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L1 fields=[{key="Node" declared={type=function flags=[IsExpected]} flags=[IsRequired]}]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = Call v0
    v2 = PropGet v1 key="Node"
    v3 = Integer integer=5
    v4 = Construct v2, -, v3
    v5 = Builtin builtin=GlobalThis
    v6 = PropGet v5 key="print" [isFreeName]
    v7 = PropGet v4 key="get"
    v8 = Call v7, v4
    v9 = PropGet v1 key="Node"
    v10 = InstanceOf v4, v9
    v11 = PropGet v4 key="owner"
    v12 = StrictEq v11, v1
    v13 = PropGet v4 key="constructor"
    v14 = PropGet v1 key="Node"
    v15 = StrictEq v13, v14
    v16 = PropGet v5 key="Object" [isFreeName]
    v17 = PropGet v16 key="getPrototypeOf"
    v18 = Call v17, v16, v4
    v19 = PropGet v1 key="Node"
    v20 = PropGet v19 key="prototype"
    v21 = StrictEq v18, v20
    v22 = Call v6, -, v8, v10, v12, v15, v21
    v23 = Undefined
    Return v23
}
function f1 name="setUp" flags=[HasExplicitName] thisType={type=anything} results=[{type=L1 flags=[IsExpected]}] {
    v0 = Data data=d0
    v1 = NewClosure v0 function=f2
    PropSet v0, v1 key="Node"
    v3 = PropGet v0 key="Node"
    v4 = PropGet v3 key="prototype"
    v5 = NewClosure function=f3
    PropSet v4, v5 key="get"
    Return v0
}
function f2 flags=[HasThis] length=1 captures=[{type=L1 flags=[IsExpected]}] thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything):
    PropSet v1, v2 key="v"
    PropSet v1, v0 key="owner"
    v5 = Undefined
    Return v5
}
function f3 flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="v"
    Return v1
}
data d0 root=Object{layout=L1}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral constructorInAProperty = R"IR(layout L0 kind=Instance constructor=f1 fields=[{key="px" declared={type=1 flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L1 kind=Instance constructor=f3 fields=[{key="py" declared={type=2 flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L2 fields=[{key="Point" declared={type=function flags=[IsExpected]} flags=[IsRequired]}]
layout L3 fields=[{key="Point" declared={type=function flags=[IsExpected]} flags=[IsRequired]}]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = Data data=d0
    v1 = NewClosure function=f1
    PropSet v0, v1 key="Point"
    v3 = PropGet v0 key="Point"
    v4 = PropGet v3 key="prototype"
    v5 = NewClosure function=f2
    PropSet v4, v5 key="sum"
    v7 = PropGet v0 key="Point"
    v8 = Construct v7
    v9 = Builtin builtin=GlobalThis
    v10 = PropGet v9 key="print" [isFreeName]
    v11 = PropGet v8 key="sum"
    v12 = Call v11, v8
    v13 = PropGet v0 key="Point"
    v14 = InstanceOf v8, v13
    v15 = PropGet v9 key="Object" [isFreeName]
    v16 = PropGet v15 key="getPrototypeOf"
    v17 = Call v16, v15, v8
    v18 = PropGet v0 key="Point"
    v19 = PropGet v18 key="prototype"
    v20 = StrictEq v17, v19
    v21 = PropGet v8 key="constructor"
    v22 = PropGet v0 key="Point"
    v23 = StrictEq v21, v22
    v24 = Call v10, -, v12, v14, v20, v23
    v25 = NewClosure function=f3
    v26 = NewObject -, v25 layout=L3 fields=[0]
    v27 = FinishObject v26
    v28 = PropGet v27 key="Point"
    v29 = PropGet v28 key="prototype"
    v30 = NewClosure function=f4
    PropSet v29, v30 key="sum"
    v32 = PropGet v27 key="Point"
    v33 = Construct v32
    v34 = PropGet v33 key="sum"
    v35 = Call v34, v33
    v36 = PropGet v27 key="Point"
    v37 = InstanceOf v33, v36
    v38 = PropGet v15 key="getPrototypeOf"
    v39 = Call v38, v15, v33
    v40 = PropGet v27 key="Point"
    v41 = PropGet v40 key="prototype"
    v42 = StrictEq v39, v41
    v43 = PropGet v33 key="constructor"
    v44 = PropGet v27 key="Point"
    v45 = StrictEq v43, v44
    v46 = Call v10, -, v35, v37, v42, v45
    v47 = Undefined
    Return v47
}
function f1 flags=[HasThis] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    PropSet v0, v1 key="px"
    v3 = Undefined
    Return v3
}
function f2 flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="px"
    Return v1
}
function f3 name="Point" flags=[HasThis] thisType={type=L1 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=2
    PropSet v0, v1 key="py"
    v3 = Undefined
    Return v3
}
function f4 flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="py"
    Return v1
}
data d0 root=Object{layout=L2}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral membersAddedAfterInstancesExist = R"IR(layout L0 kind=Instance name="Declared" constructor=f1 flags=[IsConstructorFunction]
layout L1 kind=Instance name="DeclaredWithThis" constructor=f2 fields=[{key="a" declared={type=1 flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L2 kind=Instance constructor=f4 fields=[{key="a" declared={type=2 flags=[IsExpected]}}] flags=[IsConstructorFunction]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = NewClosure function=f2
    v2 = NewClosure function=f3
    v3 = NewClosure function=f4
    v4 = Construct v0
    v5 = Construct v1
    v6 = Construct v2
    v7 = Construct v3
    v8 = NewArray v4, v5, v6, v7 contents=anything
    v9 = Builtin builtin=GlobalThis
    v10 = PropGet v9 key="print" [isFreeName]
    v11 = Integer
    v12 = ElemGet v8, v11
    v13 = PropGet v12 key="x"
    v14 = Integer integer=1
    v15 = ElemGet v8, v14
    v16 = PropGet v15 key="x"
    v17 = Integer integer=2
    v18 = ElemGet v8, v17
    v19 = PropGet v18 key="x"
    v20 = Integer integer=3
    v21 = ElemGet v8, v20
    v22 = PropGet v21 key="x"
    v23 = Integer
    v24 = ElemGet v8, v23
    v25 = PropGet v24 key="m"
    v26 = TypeOf v25
    v27 = Integer integer=1
    v28 = ElemGet v8, v27
    v29 = PropGet v28 key="m"
    v30 = TypeOf v29
    v31 = Integer integer=2
    v32 = ElemGet v8, v31
    v33 = PropGet v32 key="m"
    v34 = TypeOf v33
    v35 = Integer integer=3
    v36 = ElemGet v8, v35
    v37 = PropGet v36 key="m"
    v38 = TypeOf v37
    v39 = Call v10, -, v13, v16, v19, v22, v26, v30, v34, v38
    v40 = PropGet v0 key="prototype"
    v41 = Integer integer=10
    PropSet v40, v41 key="x"
    v43 = PropGet v1 key="prototype"
    v44 = Integer integer=11
    PropSet v43, v44 key="x"
    v46 = PropGet v2 key="prototype"
    v47 = Integer integer=12
    PropSet v46, v47 key="x"
    v49 = PropGet v3 key="prototype"
    v50 = Integer integer=13
    PropSet v49, v50 key="x"
    v52 = PropGet v0 key="prototype"
    v53 = NewClosure function=f5
    PropSet v52, v53 key="m"
    v55 = PropGet v1 key="prototype"
    v56 = NewClosure function=f6
    PropSet v55, v56 key="m"
    v58 = PropGet v2 key="prototype"
    v59 = NewClosure function=f7
    PropSet v58, v59 key="m"
    v61 = PropGet v3 key="prototype"
    v62 = NewClosure function=f8
    PropSet v61, v62 key="m"
    v64 = Integer
    v65 = ElemGet v8, v64
    v66 = PropGet v65 key="x"
    v67 = Integer integer=1
    v68 = ElemGet v8, v67
    v69 = PropGet v68 key="x"
    v70 = Integer integer=2
    v71 = ElemGet v8, v70
    v72 = PropGet v71 key="x"
    v73 = Integer integer=3
    v74 = ElemGet v8, v73
    v75 = PropGet v74 key="x"
    v76 = Integer
    v77 = ElemGet v8, v76
    v78 = PropGet v77 key="m"
    v79 = Call v78, v77
    v80 = Integer integer=1
    v81 = ElemGet v8, v80
    v82 = PropGet v81 key="m"
    v83 = Call v82, v81
    v84 = Integer integer=2
    v85 = ElemGet v8, v84
    v86 = PropGet v85 key="m"
    v87 = Call v86, v85
    v88 = Integer integer=3
    v89 = ElemGet v8, v88
    v90 = PropGet v89 key="m"
    v91 = Call v90, v89
    v92 = Call v10, -, v66, v69, v72, v75, v79, v83, v87, v91
    v93 = Construct v0
    v94 = PropGet v93 key="m"
    v95 = Call v94, v93
    v96 = Construct v1
    v97 = PropGet v96 key="m"
    v98 = Call v97, v96
    v99 = Construct v2
    v100 = PropGet v99 key="m"
    v101 = Call v100, v99
    v102 = Construct v3
    v103 = PropGet v102 key="m"
    v104 = Call v103, v102
    v105 = Call v10, -, v95, v98, v101, v104
    v106 = PropGet v0 key="prototype"
    v107 = PropDelete v106 key="x"
    v108 = PropGet v3 key="prototype"
    v109 = PropDelete v108 key="m"
    v110 = Integer
    v111 = ElemGet v8, v110
    v112 = PropGet v111 key="x"
    v113 = Construct v0
    v114 = PropGet v113 key="x"
    v115 = Integer integer=3
    v116 = ElemGet v8, v115
    v117 = PropGet v116 key="m"
    v118 = TypeOf v117
    v119 = Construct v3
    v120 = PropGet v119 key="m"
    v121 = TypeOf v120
    v122 = Call v10, -, v112, v114, v118, v121
    v123 = Undefined
    Return v123
}
function f1 name="Declared" flags=[HasExplicitName] thisType={type=anything} results=[{type=anything}] {
    v0 = Undefined
    Return v0
}
function f2 name="DeclaredWithThis" flags=[HasThis, HasExplicitName] thisType={type=L1 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    PropSet v0, v1 key="a"
    v3 = Undefined
    Return v3
}
function f3 name="Expression" thisType={type=anything} results=[{type=anything}] {
    v0 = Undefined
    Return v0
}
function f4 name="ExpressionWithThis" flags=[HasThis] thisType={type=L2 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=2
    PropSet v0, v1 key="a"
    v3 = Undefined
    Return v3
}
function f5 flags=[HasThis] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v1 = String string="d"
    v2 = PropGet v0 key="x"
    v3 = Add v1, v2
    Return v3
}
function f6 flags=[HasThis] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v1 = String string="dt"
    v2 = PropGet v0 key="x"
    v3 = Add v1, v2
    v4 = PropGet v0 key="a"
    v5 = Add v3, v4
    Return v5
}
function f7 flags=[HasThis] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v1 = String string="e"
    v2 = PropGet v0 key="x"
    v3 = Add v1, v2
    Return v3
}
function f8 flags=[HasThis] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v1 = String string="et"
    v2 = PropGet v0 key="x"
    v3 = Add v1, v2
    v4 = PropGet v0 key="a"
    v5 = Add v3, v4
    Return v5
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral closureReadFromAnInstance = R"IR(layout L0 kind=Instance constructor=f1 fields=[{key="px" declared={type=1 flags=[IsExpected]}}] flags=[IsConstructorFunction]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = Construct v0
    v2 = PropGet v1 key="constructor"
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="print" [isFreeName]
    v5 = StrictEq v2, v0
    v6 = Construct v2
    v7 = InstanceOf v6, v0
    v8 = Construct v2
    v9 = PropGet v8 key="px"
    v10 = Call v4, -, v5, v7, v9
    v11 = Undefined
    Return v11
}
function f1 name="P" flags=[HasThis] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    PropSet v0, v1 key="px"
    v3 = Undefined
    Return v3
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral constructedInAFunctionDeclaredEarlier = R"IR(layout L0 kind=Instance constructor=f2 fields=[{key="px" declared={type=1 flags=[IsExpected]}}] flags=[IsConstructorFunction]
variable g0 name="Point" declared={type=function flags=[IsExpected]} initialization=Undefined flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = NewClosure function=f2
    PutModuleVariable v1 variable=g0
    v3 = PropGet v1 key="prototype"
    v4 = NewClosure function=f3
    PropSet v3, v4 key="sum"
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="print" [isFreeName]
    v8 = Call v0
    v9 = PropGet v8 key="sum"
    v10 = Call v9, v8
    v11 = Call v0
    v12 = InstanceOf v11, v1
    v13 = Call v0
    v14 = PropGet v13 key="constructor"
    v15 = StrictEq v14, v1
    v16 = Call v7, -, v10, v12, v15
    v17 = Undefined
    Return v17
}
function f1 name="build" flags=[HasExplicitName] thisType={type=anything} results=[{type=anything}] {
    v0 = GetModuleVariable variable=g0
    v1 = Construct v0
    Return v1
}
function f2 name="Point" flags=[HasThis] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    PropSet v0, v1 key="px"
    v3 = Undefined
    Return v3
}
function f3 flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="px"
    Return v1
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral constructedByValue = R"IR(layout L0 kind=Instance name="Q" constructor=f2 fields=[{key="q" declared={type=anything}}] flags=[IsConstructorFunction]
layout L1 kind=Instance constructor=f3 fields=[{key="p" declared={type=anything}}] flags=[IsConstructorFunction]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = NewClosure function=f2
    v2 = NewClosure function=f3
    v3 = PropGet v2 key="prototype"
    v4 = NewClosure function=f4
    PropSet v3, v4 key="who"
    v6 = PropGet v1 key="prototype"
    v7 = NewClosure function=f5
    PropSet v6, v7 key="who"
    v9 = Builtin builtin=GlobalThis
    v10 = PropGet v9 key="print" [isFreeName]
    v11 = Integer integer=1
    v12 = Call v0, -, v2, v11
    v13 = PropGet v12 key="who"
    v14 = Call v13, v12
    v15 = Integer integer=2
    v16 = Call v0, -, v1, v15
    v17 = PropGet v16 key="who"
    v18 = Call v17, v16
    v19 = Integer integer=3
    v20 = Construct v2, -, v19
    v21 = PropGet v20 key="who"
    v22 = Call v21, v20
    v23 = Integer integer=4
    v24 = Construct v1, -, v23
    v25 = PropGet v24 key="who"
    v26 = Call v25, v24
    v27 = Call v10, -, v14, v18, v22, v26
    v28 = Integer integer=1
    v29 = Call v0, -, v2, v28
    v30 = InstanceOf v29, v2
    v31 = Integer integer=2
    v32 = Call v0, -, v1, v31
    v33 = InstanceOf v32, v1
    v34 = PropGet v9 key="Object" [isFreeName]
    v35 = PropGet v34 key="getPrototypeOf"
    v36 = Integer integer=1
    v37 = Call v0, -, v2, v36
    v38 = Call v35, v34, v37
    v39 = PropGet v34 key="getPrototypeOf"
    v40 = Integer integer=1
    v41 = Construct v2, -, v40
    v42 = Call v39, v34, v41
    v43 = StrictEq v38, v42
    v44 = Call v10, -, v30, v33, v43
    v45 = Undefined
    Return v45
}
function f1 name="construct" flags=[HasExplicitName] length=2 thisType={type=anything} parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Construct v0, -, v1
    Return v2
}
function f2 name="Q" flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    PropSet v0, v1 key="q"
    v3 = Undefined
    Return v3
}
function f3 name="P" flags=[HasThis] length=1 thisType={type=L1 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    PropSet v0, v1 key="p"
    v3 = Undefined
    Return v3
}
function f4 flags=[HasThis] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v1 = String string="P"
    v2 = PropGet v0 key="p"
    v3 = Add v1, v2
    Return v3
}
function f5 flags=[HasThis] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v1 = String string="Q"
    v2 = PropGet v0 key="q"
    v3 = Add v1, v2
    Return v3
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral otherNewTarget = R"IR(layout L0 kind=Instance constructor=f1 fields=[{key="px" declared={type=1 flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L1 kind=Instance constructor=f2 fields=[{key="qx" declared={type=2 flags=[IsExpected]}}] flags=[IsConstructorFunction]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = NewClosure function=f2
    v2 = PropGet v1 key="prototype"
    v3 = NewClosure function=f3
    PropSet v2, v3 key="who"
    v5 = Builtin builtin=GlobalThis
    v6 = PropGet v5 key="Reflect" [isFreeName]
    v7 = PropGet v6 key="construct"
    v8 = Data data=d0
    v9 = Call v7, v6, v0, v8, v1
    v10 = PropGet v5 key="print" [isFreeName]
    v11 = PropGet v9 key="who"
    v12 = Call v11, v9
    v13 = InstanceOf v9, v1
    v14 = InstanceOf v9, v0
    v15 = PropGet v5 key="Object" [isFreeName]
    v16 = PropGet v15 key="getPrototypeOf"
    v17 = Call v16, v15, v9
    v18 = PropGet v1 key="prototype"
    v19 = StrictEq v17, v18
    v20 = Call v10, -, v12, v13, v14, v19
    v21 = Construct v0
    v22 = InstanceOf v21, v0
    v23 = Construct v1
    v24 = InstanceOf v23, v1
    v25 = Call v10, -, v22, v24
    v26 = Undefined
    Return v26
}
function f1 name="P" flags=[HasThis] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    PropSet v0, v1 key="px"
    v3 = Undefined
    Return v3
}
function f2 name="Q" flags=[HasThis] thisType={type=L1 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=2
    PropSet v0, v1 key="qx"
    v3 = Undefined
    Return v3
}
function f3 flags=[HasThis] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v1 = String string="q"
    v2 = PropGet v0 key="px"
    v3 = Add v1, v2
    Return v3
}
data d0 root=Array{contents=anything}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral chainOfFunctionsAndAClass = R"IR(layout L0 kind=Instance name="A" constructor=f1 fields=[{key="a" declared={type=1 flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L1 kind=Instance name="B" constructor=f2 fields=[{key="a" declared={type=2 flags=[IsExpected]}}, {key="b" declared={type=3 flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L2 kind=Instance constructor=f5 fields=[{key="a" declared={type=4 flags=[IsExpected]}}, {key="b" declared={type=5 flags=[IsExpected]}}, {key="c" declared={type=6 flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L3 kind=Instance name="D" constructor=f7 flags=[HasHeritage]
variable g0 declared={type=function} flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = NewClosure function=f2
    v2 = PropGet v0 key="prototype"
    v3 = NewClosure function=f3
    PropSet v2, v3 key="fromA"
    v5 = Construct v0
    PropSet v1, v5 key="prototype"
    v7 = PropGet v1 key="prototype"
    PropSet v7, v1 key="constructor"
    v9 = PropGet v1 key="prototype"
    v10 = NewClosure function=f4
    PropSet v9, v10 key="fromB"
    v12 = NewClosure function=f5
    v13 = Builtin builtin=GlobalThis
    v14 = PropGet v13 key="Object" [isFreeName]
    v15 = PropGet v14 key="create"
    v16 = PropGet v1 key="prototype"
    v17 = Call v15, v14, v16
    PropSet v12, v17 key="prototype"
    v19 = PropGet v12 key="prototype"
    v20 = NewClosure function=f6
    PropSet v19, v20 key="fromC"
    v22 = Construct v12
    v23 = PropGet v13 key="print" [isFreeName]
    v24 = PropGet v22 key="fromA"
    v25 = Call v24, v22
    v26 = PropGet v22 key="fromB"
    v27 = Call v26, v22
    v28 = PropGet v22 key="fromC"
    v29 = Call v28, v22
    v30 = InstanceOf v22, v0
    v31 = InstanceOf v22, v1
    v32 = InstanceOf v22, v12
    v33 = PropGet v22 key="constructor"
    v34 = StrictEq v33, v1
    v35 = Call v23, -, v25, v27, v29, v30, v31, v32, v34
    v36 = ResolveHeritage v12
    v37 = NewClosure function=f7
    v38 = NewClass v37, v12, v36 layout=L3
    PutModuleVariable v38 variable=g0
    v40 = NewClosure function=f8
    DefineMethod v38, v40 layout=L3 key="fromD"
    v42 = FinishClass v38 layout=L3
    v43 = Construct v42
    v44 = PropGet v43 key="fromA"
    v45 = Call v44, v43
    v46 = PropGet v43 key="fromD"
    v47 = Call v46, v43
    v48 = InstanceOf v43, v0
    v49 = InstanceOf v43, v12
    v50 = PropGet v14 key="getPrototypeOf"
    v51 = Call v50, v14, v42
    v52 = StrictEq v51, v12
    v53 = PropGet v14 key="getPrototypeOf"
    v54 = PropGet v42 key="prototype"
    v55 = Call v53, v14, v54
    v56 = PropGet v12 key="prototype"
    v57 = StrictEq v55, v56
    v58 = Call v23, -, v45, v47, v48, v49, v52, v57
    v59 = Undefined
    Return v59
}
function f1 name="A" flags=[HasThis, HasExplicitName] thisType={type=L0 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    PropSet v0, v1 key="a"
    v3 = Undefined
    Return v3
}
function f2 name="B" flags=[HasThis, HasExplicitName] thisType={type=L1 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=2
    PropSet v0, v1 key="a"
    v3 = Integer integer=3
    PropSet v0, v3 key="b"
    v5 = Undefined
    Return v5
}
function f3 flags=[HasThis] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v1 = String string="A"
    v2 = PropGet v0 key="a"
    v3 = Add v1, v2
    Return v3
}
function f4 flags=[HasThis] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v1 = String string="B"
    v2 = PropGet v0 key="b"
    v3 = Add v1, v2
    Return v3
}
function f5 name="C" flags=[HasThis] thisType={type=L2 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=4
    PropSet v0, v1 key="a"
    v3 = Integer integer=5
    PropSet v0, v3 key="b"
    v5 = Integer integer=6
    PropSet v0, v5 key="c"
    v7 = Undefined
    Return v7
}
function f6 flags=[HasThis] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
    v1 = String string="C"
    v2 = PropGet v0 key="c"
    v3 = Add v1, v2
    Return v3
}
function f7 name="D" kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee, HasExplicitName] thisType={type=anything} parameters=[{type=Array}] results=[{type=object}] {
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L3
    Return v4
}
function f8 name="fromD" kind=Method flags=[HasThis, HasExplicitName] thisType={type=L3 flags=[IsExpected]} results=[{type=string flags=[IsExpected]}] {
  b0(v0: anything):
    v1 = String string="D"
    v2 = PropGet v0 key="fromC"
    v3 = Call v2, v0
    v4 = Add v1, v3
    Return v4
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral nameSetAtRunTime = R"IR(layout L0 kind=Instance constructor=f1 fields=[{key="n" declared={type=number flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L1 kind=Instance constructor=f2 fields=[{key="z" declared={type=1 flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L2 fields=[{key="n" declared={type=number flags=[IsExpected]} flags=[IsRequired]}]
layout L3
module m0 path="main.js" displayPath="main.js" body=f0
function f0 kind=Module thisType={type=anything} results=[{type=anything}] {
    v0 = Integer
    v1 = NewClosure function=f1
    v2 = Builtin builtin=SymbolHasInstance
    SetFunctionName v1, v2
    v4 = NewObject -, v0 layout=L2 fields=[0]
    PropDefine v4, v1 key=@@hasInstance
    v6 = FinishObject v4
    v7 = Builtin builtin=GlobalThis
    v8 = PropGet v7 key="print" [isFreeName]
    v9 = Integer integer=1
    v10 = InstanceOf v9, v6
    v11 = PropGet v6 key="n"
    v12 = PropGet v6 key=@@hasInstance
    v13 = PropGet v12 key="name"
    v14 = Call v8, -, v10, v11, v13
    v15 = String string="madeUp"
    v16 = ToPropertyKey v15
    v17 = NewClosure function=f2
    SetFunctionName v17, v16
    v19 = NewObject layout=L3
    ElemDefine v19, v16, v17
    v21 = FinishObject v19
    v22 = ElemGet v21, v15
    v23 = PropGet v22 key="name"
    v24 = ElemGet v21, v15
    v25 = Construct v24
    v26 = PropGet v25 key="z"
    v27 = ElemGet v21, v15
    v28 = Construct v27
    v29 = ElemGet v21, v15
    v30 = InstanceOf v28, v29
    v31 = Call v8, -, v23, v26, v30
    v32 = Undefined
    Return v32
}
function f1 flags=[HasThis] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=boolean flags=[IsExpected]}] {
  b0(v0: anything, v1: anything):
    v2 = PropGet v0 key="n"
    v3 = Integer
    v4 = BitOr v2, v3
    v5 = Integer integer=1
    v6 = Add v4, v5
    PropSet v0, v6 key="n"
    v8 = Integer integer=1
    v9 = StrictEq v1, v8
    Return v9
}
function f2 flags=[HasThis] thisType={type=L1 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    PropSet v0, v1 key="z"
    v3 = Undefined
    Return v3
}
entry module=m0
)IR"_s;

static constexpr ASCIILiteral commonJSModule = R"IR(layout L0 kind=Instance name="Point" constructor=f1 fields=[{key="px" declared={type=anything}}] flags=[IsConstructorFunction]
layout L1 kind=Instance constructor=f3 fields=[{key="py" declared={type=anything}}] flags=[IsConstructorFunction]
variable g0 name="exports" module=m0 declared={type=anything} initialization=Undefined
module m0 path="/main.cjs" displayPath="main.cjs" kind=CommonJs exportRule=CommonJs requireResult=ModuleExports body=f0 exportsVariable=g0
function f0 kind=CommonJsModule flags=[IsSloppy] thisType={type=anything} results=[{type=anything}] {
    v0 = NewClosure function=f1
    v1 = PropGet v0 key="prototype"
    v2 = NewClosure function=f2
    PropSet v1, v2 key="sum" mode=Sloppy
    v4 = NewClosure function=f3
    v5 = PropGet v4 key="prototype"
    v6 = NewClosure function=f4
    PropSet v5, v6 key="sum" mode=Sloppy
    v8 = Builtin builtin=GlobalThis
    v9 = PropGet v8 key="print" [isFreeName]
    v10 = Integer integer=1
    v11 = Construct v0, -, v10
    v12 = PropGet v11 key="sum"
    v13 = Call v12, v11
    v14 = Integer integer=2
    v15 = Construct v4, -, v14
    v16 = PropGet v15 key="sum"
    v17 = Call v16, v15
    v18 = Integer integer=1
    v19 = Construct v0, -, v18
    v20 = InstanceOf v19, v0
    v21 = Integer integer=2
    v22 = Construct v4, -, v21
    v23 = InstanceOf v22, v4
    v24 = Integer integer=2
    v25 = Construct v4, -, v24
    v26 = PropGet v25 key="constructor"
    v27 = StrictEq v26, v4
    v28 = Call v9, -, v13, v17, v20, v23, v27
    PutModuleVariable v0 variable=g0
    v30 = Undefined
    Return v30
}
function f1 name="Point" flags=[IsSloppy, HasThis, HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    v2 = ToThis v0
    PropSet v2, v1 key="px" mode=Sloppy
    v4 = Undefined
    Return v4
}
function f2 flags=[IsSloppy, HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = ToThis v0
    v2 = PropGet v1 key="px"
    Return v2
}
function f3 name="Other" flags=[IsSloppy, HasThis] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
    v2 = ToThis v0
    PropSet v2, v1 key="py" mode=Sloppy
    v4 = Undefined
    Return v4
}
function f4 flags=[IsSloppy, HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = ToThis v0
    v2 = PropGet v1 key="py"
    Return v2
}
entry module=m0
)IR"_s;

static void checkInEveryMode(ASCIILiteral programText, ASCIILiteral expectedOutput)
{
    for (ASCIILiteral references : { "references=wide"_s, "references=narrow"_s }) {
        checkOutputOfProgramInShell(programText, expectedOutput, { references });
        checkOutputOfProgramInShell(programText, expectedOutput, { references, "validate"_s });
        checkOutputOfProgramInShell(programText, expectedOutput, { references, "disable=pass:InlineCalls"_s });
    }
    checkOutputOfProgramInShell(programText, expectedOutput, { "references=narrow"_s, "disable=pass:PromoteEnvironmentsToVariables"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { "references=narrow"_s, "disable=flow-results"_s });
}

static void testFunctionExpression()
{
    checkInEveryMode(functionExpression, R"(1
true
true
true
true
)"_s);
}

static void testBodiesWithoutThis()
{
    checkInEveryMode(bodiesWithoutThis, R"(7 3 true
8 4 true
6 text
)"_s);
}

static void testClassBelowAFunctionExpression()
{
    checkInEveryMode(classBelowAFunctionExpression, R"(true
true
1 3
true true false
)"_s);
}

static void testDeclaredInAFunctionThatIsCalledAtOnce()
{
    checkInEveryMode(declaredInAFunctionThatIsCalledAtOnce, R"(2 3 4
true true true
true true
)"_s);
}

static void testFunctionThatConstructsItself()
{
    checkInEveryMode(functionThatConstructsItself, R"(1 2 true true
1 2 true true
)"_s);
}

static void testDeclaredInAFunctionCalledOnceAndTwice()
{
    checkInEveryMode(declaredInAFunctionCalledOnceAndTwice, R"(1 2 2
true true false false
)"_s);
}

static void testFactoryWithACapture()
{
    checkInEveryMode(factoryWithACapture, R"(Aa Bb
true false false true
false false
true true
)"_s);
}

static void testFactoryWithoutCaptures()
{
    checkInEveryMode(factoryWithoutCaptures, R"(A1 B1
true false false true
false false
true true
)"_s);
}

static void testCreatedInALoop()
{
    checkInEveryMode(createdInALoop, R"(0 1 2
true false false
)"_s);
}

static void testPrototypeReplacedBeforeTheFirstConstruction()
{
    checkInEveryMode(prototypeReplacedBeforeTheFirstConstruction, R"(11 true true
11 21 false true
false true
true
)"_s);
}

static void testPrototypeReplacedAfterTheFirstConstruction()
{
    checkInEveryMode(prototypeReplacedAfterTheFirstConstruction, R"(1 21 false true
)"_s);
}

static void testConstructorWithACapture()
{
    checkInEveryMode(constructorWithACapture, R"(c1 c2
true true true
c6! true true
7
)"_s);
}

static void testConstructorThatCapturesItsHolder()
{
    checkInEveryMode(constructorThatCapturesItsHolder, R"(5 true true true true
)"_s);
}

static void testConstructorInAProperty()
{
    checkInEveryMode(constructorInAProperty, R"(1 true true true
2 true true true
)"_s);
}

static void testMembersAddedAfterInstancesExist()
{
    checkInEveryMode(membersAddedAfterInstancesExist, R"(undefined undefined undefined undefined undefined undefined undefined undefined
10 11 12 13 d10 dt111 e12 et132
d10 dt111 e12 et132
undefined undefined undefined undefined
)"_s);
}

static void testClosureReadFromAnInstance()
{
    checkInEveryMode(closureReadFromAnInstance, R"(true true 1
)"_s);
}

static void testConstructedInAFunctionDeclaredEarlier()
{
    checkInEveryMode(constructedInAFunctionDeclaredEarlier, R"(1 true true
)"_s);
}

static void testConstructedByValue()
{
    checkInEveryMode(constructedByValue, R"(P1 Q2 P3 Q4
true true true
)"_s);
}

static void testOtherNewTarget()
{
    checkInEveryMode(otherNewTarget, R"(q1 true false true
true true
)"_s);
}

static void testChainOfFunctionsAndAClass()
{
    checkInEveryMode(chainOfFunctionsAndAClass, R"(A4 B5 C6 true true true true
A4 DC6 true true true true
)"_s);
}

static void testNameSetAtRunTime()
{
    checkInEveryMode(nameSetAtRunTime, R"(true 1 [Symbol.hasInstance]
madeUp 1 true
)"_s);
}

static void testCommonJSModule()
{
    checkInEveryMode(commonJSModule, R"(1 2 true true true
)"_s);
}

static Vector<uint32_t> allocationsOfInstances(LoadedProgram& program, EvaluationState evaluation)
{
    const ProgramTables& tables = program.state().tables;
    std::span<const AllocationData> allocations = tables.allocations();
    Vector<uint32_t> numbers;
    for (uint32_t number = 0; number < allocations.size(); ++number) {
        if (allocations[number].isConstructed && tables.layoutForShape(allocations[number].shape).evaluation() == evaluation)
            numbers.append(number);
    }
    return numbers;
}

static JSValue prototypeAt(LoadedProgram& program, uint32_t allocation)
{
    std::optional<StructureID> structure = program.state().objects->structures.structureIDOf(allocation);
    AOT_TEST_CHECK(structure);
    return structure->decode()->storedPrototype();
}

static void allocateWithPrototype(LoadedProgram& program, uint32_t allocation, JSObject* prototype)
{
    VM& vm = program.vm();
    ObjectModel& objects = *program.state().objects;
    Object* allocated = uncheckedDowncast<Object>(objects.structures.allocate(vm, allocation));
    JSObject* instance = objects.classes.instanceWithPrototype(vm, allocated, prototype);
    AOT_TEST_CHECK(instance == allocated && instance->getPrototypeDirect() == JSValue(prototype));
    objects.structures.predictStructure(vm, allocation, uncheckedDowncast<CompiledStructure>(instance->structure()));
}

static void checkSitePredictsTheFirstPrototype(LoadedProgram& program)
{
    VM& vm = program.vm();
    AOT_TEST_CHECK(allocationsOfInstances(program, EvaluationState::ProvenOnce).isEmpty());
    Vector<uint32_t> allocations = allocationsOfInstances(program, EvaluationState::PredictedOnce);
    AOT_TEST_CHECK(!allocations.isEmpty());
    for (uint32_t allocation : allocations) {
        FunctionID constructor(program.state().tables.constructorOfAllocation(allocation));
        program.state().objects->structures.allocate(vm, allocation);
        AOT_TEST_CHECK(prototypeAt(program, allocation) == JSValue(program.realm()->objectPrototype()));
        AOT_TEST_CHECK(!program.state().objects->closures.closureIfCreated(constructor));

        JSObject* first = constructEmptyObject(program.realm());
        JSObject* second = constructEmptyObject(program.realm());
        allocateWithPrototype(program, allocation, first);
        AOT_TEST_CHECK(prototypeAt(program, allocation) == JSValue(first));
        allocateWithPrototype(program, allocation, second);
        AOT_TEST_CHECK(prototypeAt(program, allocation) == JSValue(first));
        AOT_TEST_CHECK(program.state().objects->structures.allocate(vm, allocation)->structure()->storedPrototype() == JSValue(first));
    }
}

static void checkSiteOfFunctionEvaluatedOnceKeepsItsPrototype(LoadedProgram& program)
{
    VM& vm = program.vm();
    ObjectModel& objects = *program.state().objects;
    Vector<uint32_t> allocations = allocationsOfInstances(program, EvaluationState::ProvenOnce);
    AOT_TEST_CHECK(!allocations.isEmpty());
    // Several sites may allocate for one constructor: the first of them that allocates creates its closure.
    Vector<FunctionID> constructorsSeen;
    for (uint32_t allocation : allocations) {
        FunctionID constructor(program.state().tables.constructorOfAllocation(allocation));
        bool isFirstSite = !constructorsSeen.contains(constructor);
        constructorsSeen.append(constructor);
        AOT_TEST_CHECK(objects.closures.hasSingletonClosure(constructor) && !objects.closures.closureIfCreated(constructor) == isFirstSite);
        objects.structures.allocate(vm, allocation);
        JSCell* closure = objects.closures.closureIfCreated(constructor);
        AOT_TEST_CHECK(closure && closure == objects.closures.createClosure(vm, constructor, { }, 0));
        JSValue prototype = uncheckedDowncast<Closure>(closure)->prototypeProperty(vm);
        AOT_TEST_CHECK(prototype.isObject() && prototypeAt(program, allocation) == prototype);

        allocateWithPrototype(program, allocation, constructEmptyObject(program.realm()));
        AOT_TEST_CHECK(prototypeAt(program, allocation) == prototype);
    }
}

// No operation defines the class of a constructor function: the first construction enters the function and its prototype object as the holders of the class.
static void checkFirstConstructionDefinesTheClass(LoadedProgram& program)
{
    VM& vm = program.vm();
    ObjectModel& objects = *program.state().objects;
    Vector<uint32_t> allocations = allocationsOfInstances(program, EvaluationState::ProvenOnce);
    AOT_TEST_CHECK(!allocations.isEmpty());
    uint32_t allocation = allocations[0];
    FunctionID constructor(program.state().tables.constructorOfAllocation(allocation));
    const LayoutData* classLayout = objects.classes.classConstructedBy(constructor);
    AOT_TEST_CHECK(classLayout && !objects.classes.classPrototypeIfExists(*classLayout));

    JSCell* instance = objects.structures.allocate(vm, allocation);
    auto* closure = uncheckedDowncast<Closure>(objects.closures.closureIfCreated(constructor));
    JSValue prototype = closure->prototypeProperty(vm);
    AOT_TEST_CHECK(prototype.isObject() && objects.classes.classPrototypeIfExists(*classLayout) == asObject(prototype));
    AOT_TEST_CHECK(!uncheckedDowncast<CompiledStructure>(instance->structure())->hasOtherPrototype());
    ClassSide side = ClassSide::Static;
    AOT_TEST_CHECK(objects.classes.classHeldBy(asObject(prototype), side) == classLayout && side == ClassSide::Prototype);
    AOT_TEST_CHECK(objects.classes.classHeldBy(closure, side) == classLayout && side == ClassSide::Static);

    JSObject* other = constructEmptyObject(program.realm());
    Object* allocated = uncheckedDowncast<Object>(objects.structures.allocate(vm, allocation));
    AOT_TEST_CHECK(objects.classes.instanceWithPrototype(vm, allocated, other) == allocated);
    AOT_TEST_CHECK(uncheckedDowncast<CompiledStructure>(allocated->structure())->hasOtherPrototype());
    AOT_TEST_CHECK(objects.classes.classPrototypeIfExists(*classLayout) == asObject(prototype));
}

std::span<const NativeTest> constructorFunctionTests()
{
    static constexpr NativeTest tests[] = {
        { "constructor-functions/function-expression"_s, testFunctionExpression },
        { "constructor-functions/bodies-without-this"_s, testBodiesWithoutThis },
        { "constructor-functions/class-below-a-function-expression"_s, testClassBelowAFunctionExpression },
        { "constructor-functions/declared-in-a-function-that-is-called-at-once"_s, testDeclaredInAFunctionThatIsCalledAtOnce },
        { "constructor-functions/function-that-constructs-itself"_s, testFunctionThatConstructsItself },
        { "constructor-functions/declared-in-a-function-called-once-and-twice"_s, testDeclaredInAFunctionCalledOnceAndTwice },
        { "constructor-functions/factory-with-a-capture"_s, testFactoryWithACapture },
        { "constructor-functions/factory-without-captures"_s, testFactoryWithoutCaptures },
        { "constructor-functions/created-in-a-loop"_s, testCreatedInALoop },
        { "constructor-functions/prototype-replaced-before-the-first-construction"_s, testPrototypeReplacedBeforeTheFirstConstruction },
        { "constructor-functions/prototype-replaced-after-the-first-construction"_s, testPrototypeReplacedAfterTheFirstConstruction },
        { "constructor-functions/constructor-with-a-capture"_s, testConstructorWithACapture },
        { "constructor-functions/constructor-that-captures-its-holder"_s, testConstructorThatCapturesItsHolder },
        { "constructor-functions/constructor-in-a-property"_s, testConstructorInAProperty },
        { "constructor-functions/members-added-after-instances-exist"_s, testMembersAddedAfterInstancesExist },
        { "constructor-functions/closure-read-from-an-instance"_s, testClosureReadFromAnInstance },
        { "constructor-functions/constructed-in-a-function-declared-earlier"_s, testConstructedInAFunctionDeclaredEarlier },
        { "constructor-functions/constructed-by-value"_s, testConstructedByValue },
        { "constructor-functions/other-new-target"_s, testOtherNewTarget },
        { "constructor-functions/chain-of-functions-and-a-class"_s, testChainOfFunctionsAndAClass },
        { "constructor-functions/name-set-at-run-time"_s, testNameSetAtRunTime },
        { "constructor-functions/commonjs-module"_s, testCommonJSModule },
        { "constructor-functions/site-predicts-the-first-prototype"_s, runNativeTestWithLoadedProgram<constructorFunctionTests, factoryWithoutCaptures, checkSitePredictsTheFirstPrototype> },
        { "constructor-functions/site-of-a-function-evaluated-once-keeps-its-prototype"_s, runNativeTestWithLoadedProgram<constructorFunctionTests, functionExpression, checkSiteOfFunctionEvaluatedOnceKeepsItsPrototype> },
        { "constructor-functions/first-construction-defines-the-class"_s, runNativeTestWithLoadedProgram<constructorFunctionTests, functionExpression, checkFirstConstructionDefinesTheClass> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
