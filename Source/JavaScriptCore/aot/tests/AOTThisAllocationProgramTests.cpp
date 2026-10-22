/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTClassTable.h"
#include "AOTClassToken.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTImage.h"
#include "AOTKeyOrderTable.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTShellProgramTestSupport.h"
#include "DeferGCInlines.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> thisAllocationProgramTests();

static constexpr ASCIILiteral classFactoryText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="K" module=m0 constructor=f2 fields=[{key=#"#"@L0 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}, {key=#"#p"@L0 declared={type=number flags=[IsExpected]} flags=[IsRequired, IsPrivate]}]
layout L1 kind=Instance name="Made" module=m0 base=L0 constructor=f3 fields=[{key="tag" declared={type=anything} flags=[IsRequired]}] supertypes=[L0] flags=[HasHeritage]
variable g0 name="K" module=m0 declared={type=function|absent} flags=[IsImmutable]
variable g1 module=m0 declared={type=classtoken} flags=[IsImmutable]
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = NewClosure function=f1
    v1 = NewClassToken layout=L0
    PutModuleVariable v1 variable=g1
    v3 = NewClosure function=f2
    v4 = NewClass v3 layout=L0
    v5 = NewClosure function=f4
    DefineMethod v4, v5 layout=L0 key="getP"
    v7 = NewClosure function=f7
    DefineMethod v4, v7 layout=L0 side=Static index=1 key="has"
    v9 = FinishClass v4 layout=L0
    PutModuleVariable v9 variable=g0
    v11 = Integer integer=1
    v12 = Integer integer=2
    v13 = Call v0, -, v11
    v14 = Call v0, -, v12
    v15 = Builtin builtin=GlobalThis
    v16 = PropGet v15 key="id" [isFreeName]
    v17 = Call v16, -, v9
    v18 = PropGet v15 key="print" [isFreeName]
    v19 = Construct v13
    v20 = Construct v14
    v21 = Construct v9
    v22 = PropGet v19 key="getP"
    v23 = Call v22, v19 calleeKey="getP"
    v24 = PropGet v20 key="getP"
    v25 = Call v24, v20 calleeKey="getP"
    v26 = PropGet v21 key="getP"
    v27 = Call v26, v21 calleeKey="getP"
    v28 = PropGet v19 key="tag"
    v29 = PropGet v20 key="tag"
    v30 = PropGet v9 key="has"
    v31 = Call v30, v9, v19 calleeKey="has"
    v32 = Call v30, v9, v20 calleeKey="has"
    v33 = Call v30, v9, v15 calleeKey="has"
    v34 = PropGet v15 key="Object" [isFreeName]
    v35 = PropGet v34 key="keys"
    v36 = Call v35, v34, v19 calleeKey="keys"
    v37 = PropGet v36 key="join"
    v38 = Call v37, v36 calleeKey="join"
    v39 = Call v18, -, v23, v25, v27, v28, v29, v31, v32, v33, v38
    Return
}
function f1 name="make" length=1 parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetModuleVariable variable=g0
    v2 = CheckInitialized v1 name="K"
    v3 = ResolveHeritage v2
    v4 = NewClosure v0 function=f3
    v5 = NewClass v4, v2, v3 layout=L1
    v6 = FinishClass v5 layout=L1
    Return v6
}
function f2 name="K" kind=BaseConstructor flags=[HasNewTarget] results=[{type=object}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Convert v2 declared={type=L0|L1}
    v4 = CallDirect v3 function=f5
    Return v2
}
function f3 kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee] captures=[{type=anything}] parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: anything, v1: function, v2: anything, v3: Array):
    v4 = GetSuperBase v1
    v5 = SuperConstructList v4, v2, v3 layout=L1
    v6 = Convert v5 declared={type=L1}
    v7 = CallDirect v0, v6 function=f6
    Return v5
}
function f4 name="getP" kind=Method flags=[HasThis] thisType={type=L0|L1 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetModuleVariable variable=g1
    v2 = PrivateGet v0, v1 layout=L0 field=1
    Return v2
}
function f5 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L0|L1} results=[{type=anything}] {
  b0(v0: L0|L1):
    v1 = GetModuleVariable variable=g1
    PrivateDefine v0, v1, v1 layout=L0
    v3 = Integer integer=7
    v4 = GetModuleVariable variable=g1
    PrivateDefine v0, v4, v3 layout=L0 field=1
    v6 = Undefined
    Return v6
}
function f6 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] captures=[{type=anything}] thisType={type=L1} results=[{type=anything}] {
  b0(v0: anything, v1: L1):
    PropDefine v1, v0 key="tag"
    v3 = Undefined
    Return v3
}
function f7 name="has" kind=Method length=1 parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetModuleVariable variable=g1
    v2 = PrivateHas v0, v1 layout=L0 field=1
    Return v2
}
)IR"_s;

static constexpr ASCIILiteral overriddenMethodText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Par" module=m0 constructor=f1
layout L1 kind=Instance name="Chi" module=m0 base=L0 constructor=f3 fields=[{key="q" declared={type=number flags=[IsExpected]} flags=[IsRequired]}, {key="seen" declared={type=number flags=[IsExpected]}}] supertypes=[L0] flags=[HasHeritage]
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = NewClosure function=f1
    v1 = NewClass v0 layout=L0
    v2 = NewClosure function=f2
    DefineMethod v1, v2 layout=L0 key="init"
    v4 = FinishClass v1 layout=L0
    v5 = ResolveHeritage v4
    v6 = NewClosure function=f3
    v7 = NewClass v6, v4, v5 layout=L1
    v8 = NewClosure function=f4
    DefineMethod v7, v8 layout=L1 key="init"
    v10 = FinishClass v7 layout=L1
    v11 = Builtin builtin=GlobalThis
    v12 = PropGet v11 key="id" [isFreeName]
    v13 = Call v12, -, v4
    v14 = Call v12, -, v10
    v15 = Construct v10
    v16 = Convert v15 declared={type=L1 flags=[IsExpected]}
    v17 = Builtin builtin=GlobalThis
    v18 = PropGet v17 key="print" [isFreeName]
    v19 = PropGet v16 key="seen"
    v20 = PropGet v16 key="q"
    v21 = PropGet v17 key="Object" [isFreeName]
    v22 = PropGet v21 key="keys"
    v23 = Call v22, v21, v16 calleeKey="keys"
    v24 = PropGet v23 key="join"
    v25 = Call v24, v23 calleeKey="join"
    v26 = PropGet v17 key="JSON" [isFreeName]
    v27 = PropGet v26 key="stringify"
    v28 = Call v27, v26, v16 calleeKey="stringify"
    v29 = Call v18, -, v19, v20, v25, v28
    Return
}
function f1 name="Par" kind=BaseConstructor flags=[HasNewTarget] results=[{type=object}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Convert v2 declared={type=L0|L1 flags=[IsExpected]}
    v4 = PropGet v3 key="init"
    v5 = Call v4, v3 calleeKey="init"
    Return v2
}
function f2 name="init" kind=Method results=[{type=anything}] {
  b0():
    v0 = Undefined
    Return v0
}
function f3 name="Chi" kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee] parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L1
    v5 = Convert v4 declared={type=L1}
    v6 = CallDirect v5 function=f5
    Return v4
}
function f4 name="init" kind=Method flags=[HasThis] thisType={type=L1 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=7
    PropSet v0, v1 key="seen"
    v3 = Undefined
    Return v3
}
function f5 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L1} results=[{type=anything}] {
  b0(v0: L1):
    v1 = Integer integer=5
    PropDefine v0, v1 key="q"
    v3 = Undefined
    Return v3
}
)IR"_s;

static constexpr ASCIILiteral storeInBaseConstructorText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="Context" module=m0 constructor=f1 fields=[{key="env" declared={type=L2 flags=[IsExpected]} flags=[IsRequired]}, {key="request" declared={type=anything}}]
layout L1 kind=Instance name="Wider" module=m0 base=L0 constructor=f2 fields=[{key="extra" declared={type=Array flags=[IsExpected]} flags=[IsRequired]}] supertypes=[L0] flags=[HasHeritage]
layout L2 module=m0
data d0 root=Object{layout=L2} module=m0
data d1 root=Array{contents=anything} module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = NewClosure function=f1
    v1 = NewClass v0 layout=L0
    v2 = FinishClass v1 layout=L0
    v3 = ResolveHeritage v2
    v4 = NewClosure function=f2
    v5 = NewClass v4, v2, v3 layout=L1
    v6 = FinishClass v5 layout=L1
    v7 = Builtin builtin=GlobalThis
    v8 = PropGet v7 key="id" [isFreeName]
    v9 = Call v8, -, v2
    v10 = Call v8, -, v6
    v11 = Integer integer=1
    v12 = Construct v6, -, v11
    v13 = Convert v12 declared={type=L1 flags=[IsExpected]}
    v14 = Builtin builtin=GlobalThis
    v15 = PropGet v14 key="print" [isFreeName]
    v16 = PropGet v14 key="JSON" [isFreeName]
    v17 = PropGet v16 key="stringify"
    v18 = Call v17, v16, v13 calleeKey="stringify"
    v19 = PropGet v14 key="Object" [isFreeName]
    v20 = PropGet v19 key="keys"
    v21 = Call v20, v19, v13 calleeKey="keys"
    v22 = PropGet v21 key="join"
    v23 = Call v22, v21 calleeKey="join"
    v24 = PropGet v13 key="request"
    v25 = Integer integer=2
    v26 = Construct v2, -, v25
    v27 = Convert v26 declared={type=L0 flags=[IsExpected]}
    v28 = Call v17, v16, v27 calleeKey="stringify"
    v29 = Call v15, -, v18, v23, v24, v28
    Return
}
function f1 name="Context" kind=BaseConstructor flags=[HasNewTarget] length=1 parameters=[{type=anything}] results=[{type=object}] {
  b0(v0: anything, v1: anything):
    v2 = GetConstructPrototype v0 layout=L0
    v3 = NewObject v2 layout=L0
    v4 = Convert v3 declared={type=L0|L1}
    v5 = CallDirect v4 function=f3
    v6 = Convert v3 declared={type=L0|L1 flags=[IsExpected]}
    PropSet v6, v1 key="request"
    Return v3
}
function f2 name="Wider" kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee] parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L1
    v5 = Convert v4 declared={type=L1}
    v6 = CallDirect v5 function=f4
    Return v4
}
function f3 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L0|L1} results=[{type=anything}] {
  b0(v0: L0|L1):
    v1 = Data data=d0
    PropDefine v0, v1 key="env"
    v3 = Undefined
    Return v3
}
function f4 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L1} results=[{type=anything}] {
  b0(v0: L1):
    v1 = Data data=d1
    PropDefine v0, v1 key="extra"
    v3 = Undefined
    Return v3
}
)IR"_s;

static constexpr ASCIILiteral abstractClassesText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 kind=Instance name="A" module=m0 constructor=f1 fields=[{key="x" declared={type=number flags=[IsExpected]}}] flags=[IsConstructorFunction]
layout L1 kind=Instance name="Middle" module=m0 base=L0 constructor=f2 supertypes=[L0] flags=[HasHeritage]
layout L2 kind=Instance name="B" module=m0 base=L1 constructor=f3 supertypes=[L0, L1] flags=[HasHeritage]
layout L3 kind=Instance name="Fork" module=m0 base=L0 constructor=f6 supertypes=[L0] flags=[HasHeritage]
layout L4 kind=Instance name="Left" module=m0 base=L3 constructor=f7 fields=[{key="left" declared={type=number flags=[IsExpected]} flags=[IsRequired]}] supertypes=[L0, L3] flags=[HasHeritage]
layout L5 kind=Instance name="Right" module=m0 base=L3 constructor=f8 fields=[{key="right" declared={type=string flags=[IsExpected]} flags=[IsRequired]}] supertypes=[L0, L3] flags=[HasHeritage]
variable g0 name="A" module=m0 declared={type=function} initialization=Function function=f1
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = GetModuleVariable variable=g0
    v1 = PropGet v0 key="prototype"
    v2 = NewClosure function=f4
    PropSet v1, v2 key="m"
    v4 = ResolveHeritage v0
    v5 = NewClosure function=f2
    v6 = NewClass v5, v0, v4 layout=L1
    v7 = FinishClass v6 layout=L1
    v8 = ResolveHeritage v7
    v9 = NewClosure function=f3
    v10 = NewClass v9, v7, v8 layout=L2
    v11 = NewClosure function=f5
    DefineMethod v10, v11 layout=L2 key="m"
    v13 = FinishClass v10 layout=L2
    v14 = Construct v13
    v15 = Builtin builtin=GlobalThis
    v16 = PropGet v15 key="print" [isFreeName]
    v17 = PropGet v14 key="x"
    v18 = PropGet v14 key="m"
    v19 = Call v18, v14 calleeKey="m"
    v20 = InstanceOf v14, v0
    v21 = Call v16, -, v17, v19, v20
    v22 = ResolveHeritage v0
    v23 = NewClosure function=f6
    v24 = NewClass v23, v0, v22 layout=L3
    v25 = FinishClass v24 layout=L3
    v26 = ResolveHeritage v25
    v27 = NewClosure function=f7
    v28 = NewClass v27, v25, v26 layout=L4
    v29 = FinishClass v28 layout=L4
    v30 = ResolveHeritage v25
    v31 = NewClosure function=f8
    v32 = NewClass v31, v25, v30 layout=L5
    v33 = FinishClass v32 layout=L5
    v34 = Construct v29
    v35 = Construct v33
    v36 = PropGet v15 key="JSON" [isFreeName]
    v37 = PropGet v36 key="stringify"
    v38 = Call v37, v36, v34 calleeKey="stringify"
    v39 = Call v37, v36, v35 calleeKey="stringify"
    v40 = PropGet v34 key="m"
    v41 = Call v40, v34 calleeKey="m"
    v42 = InstanceOf v34, v25
    v43 = InstanceOf v34, v33
    v44 = InstanceOf v35, v33
    v45 = Call v16, -, v38, v39, v41, v42, v43, v44
    Return
}
function f1 name="A" flags=[HasThis] thisType={type=L0|L1|L2 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    PropSet v0, v1 key="x"
    v3 = Undefined
    Return v3
}
function f2 name="Middle" kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee] parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L1
    Return v4
}
function f3 name="B" kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee] parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L2
    Return v4
}
function f4 name="m" results=[{type=anything}] {
  b0():
    v0 = String string="A.m"
    Return v0
}
function f5 name="m" kind=Method results=[{type=anything}] {
  b0():
    v0 = String string="B.m"
    Return v0
}
function f6 name="Fork" kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee] parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L3
    Return v4
}
function f7 name="Left" kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee] parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L4
    v5 = Integer integer=2
    PropDefine v4, v5 key="left"
    Return v4
}
function f8 name="Right" kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee] parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L5
    v5 = String string="r"
    PropDefine v4, v5 key="right"
    Return v4
}
)IR"_s;

// class First { m() { return "First.m" } }  class A { constructor() { this.seen = this.m() } m() { return "A.m" } }  class B extends First { m() { return "B.m" } }  Object.setPrototypeOf(B, A)
// class Stranger { #q = 6; q() { return this.#q } m() { return "Stranger.m" } }  const b = new B(), odd = Reflect.construct(A, [], Stranger);  let thrown = "none";  try { odd.q() } catch (e) { thrown = e.name }
// print(new A().seen, b.seen, b instanceof A, b instanceof First, odd.seen, odd instanceof A, thrown, new Stranger().q())
static constexpr ASCIILiteral classesNotBelowConstructorText = R"IR(layout L0 kind=Instance name="First" module=m0 constructor=f1
layout L1 kind=Instance name="A" module=m0 constructor=f3 fields=[{key="seen" declared={type=string flags=[IsExpected]}}]
layout L2 kind=Instance name="B" module=m0 base=L0 constructor=f5 supertypes=[L0] flags=[HasHeritage]
layout L3 kind=Instance name="Stranger" module=m0 constructor=f7 fields=[{key=#"#"@L3 declared={type=classtoken} flags=[IsRequired, IsPrivate, IsImmutable, IsBrand, IsHidden]}, {key=#"#q"@L3 declared={type=number flags=[IsExpected]} flags=[IsRequired, IsPrivate]}]
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=function} flags=[IsImmutable]
variable g2 module=m0 declared={type=function} flags=[IsImmutable]
variable g3 module=m0 declared={type=function} flags=[IsImmutable]
variable g4 module=m0 declared={type=classtoken} flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = NewClass v0 layout=L0
    PutModuleVariable v1 variable=g0
    v3 = NewClosure function=f2
    DefineMethod v1, v3 layout=L0 key="m"
    v5 = FinishClass v1 layout=L0
    v6 = Convert v5 declared={type=function flags=[IsExpected]}
    v7 = Convert v5 declared={type=function flags=[IsExpected]}
    v8 = NewClosure function=f3
    v9 = NewClass v8 layout=L1
    PutModuleVariable v9 variable=g1
    v11 = NewClosure function=f4
    DefineMethod v9, v11 layout=L1 key="m"
    v13 = FinishClass v9 layout=L1
    v14 = Convert v13 declared={type=function flags=[IsExpected]}
    v15 = Convert v13 declared={type=function flags=[IsExpected]}
    v16 = ResolveHeritage v7
    v17 = NewClosure function=f5
    v18 = NewClass v17, v7, v16 layout=L2
    PutModuleVariable v18 variable=g2
    v20 = NewClosure function=f6
    DefineMethod v18, v20 layout=L2 key="m"
    v22 = FinishClass v18 layout=L2
    v23 = Convert v22 declared={type=function flags=[IsExpected]}
    v24 = Convert v22 declared={type=function flags=[IsExpected]}
    v25 = Builtin builtin=GlobalThis
    v26 = PropGet v25 key="Object" [isFreeName]
    v27 = Convert v26 declared={type=builtin(Object) flags=[IsExpected]}
    v28 = PropGet v27 key="setPrototypeOf"
    v29 = Convert v28 declared={type=function flags=[IsExpected]}
    v30 = Convert v29 declared={type=builtin(ObjectSetPrototypeOf) flags=[IsExpected]}
    v31 = Call v30, v27, v24, v15 calleeKey="setPrototypeOf" calleeName="setPrototypeOf"
    v32 = NewClassToken layout=L3
    PutModuleVariable v32 variable=g4
    v34 = NewClosure function=f7
    v35 = NewClass v34 layout=L3
    PutModuleVariable v35 variable=g3
    v37 = NewClosure function=f8
    DefineMethod v35, v37 layout=L3 key="q"
    v39 = NewClosure function=f9
    DefineMethod v35, v39 layout=L3 index=1 key="m"
    v41 = FinishClass v35 layout=L3
    v42 = Convert v41 declared={type=function flags=[IsExpected]}
    v43 = Convert v41 declared={type=function flags=[IsExpected]}
    v44 = Construct v24 calleeName="B"
    v45 = Convert v44 declared={type=L2 flags=[IsExpected]}
    v46 = Builtin builtin=GlobalThis
    v47 = PropGet v46 key="Reflect" [isFreeName]
    v48 = Convert v47 declared={type=builtin(Reflect) flags=[IsExpected]}
    v49 = PropGet v48 key="construct"
    v50 = Convert v49 declared={type=function flags=[IsExpected]}
    v51 = Convert v50 declared={type=builtin(ReflectConstruct) flags=[IsExpected]}
    v52 = Data data=d0
    v53 = Convert v52 declared={type=Array<never> flags=[IsExpected]}
    v54 = Call v51, v48, v15, v53, v43 calleeKey="construct" calleeName="construct"
    v55 = Convert v54 declared={type=L1 flags=[IsExpected]}
    v56 = String string="none"
    v57 = Convert v56 declared={type=string flags=[IsExpected]}
    Jump -> next b2()
  b2() handler=b4():
    v58 = PropGet v55 key="q"
    v59 = Call v58, v55 calleeKey="q" calleeName="q"
    Jump -> next b3()
  b3():
    Jump -> next b5(v57)
  b4() role=CatchHandler:
    v60 = CatchValue
    v61 = PropGet v60 key="name"
    v62 = Convert v61 declared={type=string flags=[IsExpected]}
    Jump -> next b5(v62)
  b5(v63):
    v64 = Builtin builtin=GlobalThis
    v65 = PropGet v64 key="print" [isFreeName]
    v66 = Construct v15 calleeName="A"
    v67 = Convert v66 declared={type=L1 flags=[IsExpected]}
    v68 = PropGet v67 key="seen"
    v69 = Convert v68 declared={type=string flags=[IsExpected]}
    v70 = PropGet v45 key="seen"
    v71 = InstanceOf v45, v15
    v72 = InstanceOf v45, v7
    v73 = PropGet v55 key="seen"
    v74 = Convert v73 declared={type=string flags=[IsExpected]}
    v75 = InstanceOf v55, v15
    v76 = Construct v43 calleeName="Stranger"
    v77 = Convert v76 declared={type=L3 flags=[IsExpected]}
    v78 = PropGet v77 key="q"
    v79 = Convert v78 declared={type=function flags=[IsExpected]}
    v80 = Call v79, v77 calleeKey="q" calleeName="q"
    v81 = Convert v80 declared={type=number flags=[IsExpected]}
    v82 = Call v65, -, v69, v70, v71, v72, v74, v75, v63, v81 calleeName="print"
    v83 = Undefined
    Return v83
}
function f1 name="First" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    Return v2
}
function f2 name="m" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = String string="First.m"
    v1 = Convert v0 declared={type=string flags=[IsExpected]}
    Return v1
}
function f3 name="A" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L1
    v2 = NewObject v1 layout=L1
    v3 = Convert v2 declared={type=L1 flags=[IsExpected]}
    v4 = Convert v2 declared={type=L1 flags=[IsExpected]}
    v5 = PropGet v4 key="m"
    v6 = Convert v5 declared={type=function flags=[IsExpected]}
    v7 = Call v6, v4 calleeKey="m" calleeName="m"
    v8 = Convert v7 declared={type=string flags=[IsExpected]}
    PropSet v3, v8 key="seen"
    v10 = Undefined
    v11 = TypeTest v10 testedType=object
    Branch v11 -> taken b2(| 0), notTaken b3(| 0)
  b2(v12):
    Jump -> next b4(v12)
  b3(v13):
    Jump -> next b4(v2)
  b4(v14):
    Return v14
}
function f4 name="m" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = String string="A.m"
    v1 = Convert v0 declared={type=string flags=[IsExpected]}
    Return v1
}
function f5 name="B" module=m0 kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee, HasExplicitName] thisType={type=anything} parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    Jump -> next b1()
  b1():
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L2
    Return v4
}
function f6 name="m" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = String string="B.m"
    v1 = Convert v0 declared={type=string flags=[IsExpected]}
    Return v1
}
function f7 name="Stranger" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L3
    v2 = NewObject v1 layout=L3
    v3 = Convert v2 declared={type=L3}
    v4 = CallDirect v3 function=f10
    Return v2
}
function f8 name="q" module=m0 kind=Method flags=[HasThis, HasExplicitName] thisType={type=L3 flags=[IsExpected]} results=[{type=number flags=[IsExpected]}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g4
    v2 = PrivateGet v0, v1 layout=L3 field=1
    v3 = Convert v2 declared={type=number flags=[IsExpected]}
    Return v3
}
function f9 name="m" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = String string="Stranger.m"
    v1 = Convert v0 declared={type=string flags=[IsExpected]}
    Return v1
}
function f10 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L3} results=[{type=anything}] {
  b0(v0: L3):
    Jump -> next b1()
  b1():
    v1 = GetModuleVariable variable=g4
    PrivateDefine v0, v1, v1 layout=L3
    v3 = Integer integer=6
    v4 = Convert v3 declared={type=number flags=[IsExpected]}
    v5 = GetModuleVariable variable=g4
    PrivateDefine v0, v5, v4 layout=L3 field=1
    v7 = Undefined
    Return v7
}
data d0 root=Array{contents=anything} module=m0
entry module=m0
)IR"_s;

static const LayoutData& classNamed(LoadedProgram& program, ASCIILiteral name)
{
    const ProgramTables& tables = program.state().tables;
    for (uint32_t shape = 1; shape < tables.firstEnvironmentShape; ++shape) {
        if (tables.hasLayoutForShape(shape) && Image::current()->text().equals(StringID(tables.layoutForShape(shape).name), name))
            return tables.layoutForShape(shape);
    }
    failNativeTest(__FILE__, __LINE__, name.characters());
}

static const FieldData& privateFieldOf(const LayoutData& layout, bool isBrand)
{
    for (const FieldData& field : layout.fields()) {
        if (field.isPrivate() && field.isBrand() == isBrand)
            return field;
    }
    failNativeTest(__FILE__, __LINE__, "the layout has no such private field");
}

static JSObject* definePrototypeOfSubclass(LoadedProgram& program, const LayoutData& base, const LayoutData& subclass)
{
    VM& vm = program.vm();
    ObjectModel& objects = *program.state().objects;
    JSObject* basePrototype = objects.classes.classPrototype(vm, base);
    AOT_TEST_CHECK(basePrototype);
    JSObject* baseClass = asObject(basePrototype->get(program.realm(), vm.propertyNames->constructor));

    FunctionID constructor(subclass.classData().classConstructor);
    CaptureCounts counts = captureCountsOf(constructor);
    Vector<uint64_t, 2> captures;
    captures.fill(JSValue::encode(jsNumber(1)), counts.captures);
    Closure* classObject = Closure::create(vm, objects.closures.closureStructureFor(vm, constructor), constructor, captures.span(), counts.references);
    objects.classes.defineClass(program.realm(), classObject, baseClass, basePrototype);
    AOT_TEST_CHECK(!vm.exceptionForInspection());
    return asObject(classObject->get(program.realm(), vm.propertyNames->prototype));
}

static void checkPrivateFieldsMoveToTheSubclassInstance(LoadedProgram& program)
{
    VM& vm = program.vm();
    DeferGC deferGC(vm);
    ObjectModel& objects = *program.state().objects;
    const LayoutData& base = classNamed(program, "K"_s);
    const LayoutData& subclass = classNamed(program, "Made"_s);
    const FieldData& brand = privateFieldOf(base, true);
    const FieldData& field = privateFieldOf(base, false);
    AOT_TEST_CHECK(!objects.layouts.valueHasType(objects.layouts.typeOf(brand), ClassToken::create(vm, base)));

    for (unsigned evaluation = 0; evaluation < 2; ++evaluation) {
        JSObject* prototype = definePrototypeOfSubclass(program, base, subclass);
        JSCell* token = ClassToken::create(vm, base);
        uint16_t listed[] { base.indexOf(brand), base.indexOf(field) };
        std::optional<uint32_t> orderWord = base.keysAreNeverEnumerated ? std::optional<uint32_t>(unorderedOrderWord) : objects.keyOrders.orderStartingWith(base, listed);
        AOT_TEST_CHECK(orderWord);
        Object* allocated = Object::create(vm, objects.structures.objectStructure(vm, base.shape), base, base.size, *orderWord);
        AOT_TEST_CHECK(allocated->store(vm, brand, AcceptedValue::provenAtBuildTime(token)) && allocated->store(vm, field, AcceptedValue::provenAtBuildTime(jsNumber(7))));

        JSObject* made = objects.classes.instanceWithPrototype(vm, allocated, prototype);
        AOT_TEST_CHECK(made != allocated && isCompiledObject(made));
        Object* instance = uncheckedDowncast<Object>(made);
        AOT_TEST_CHECK(&instance->layout() == &subclass);
        AOT_TEST_CHECK(!instance->hasFlagsInOrderWord() && !instance->extraProperties());
        AOT_TEST_CHECK(instance->getPrototypeDirect() == JSValue(prototype));
        AOT_TEST_CHECK(instance->read(privateFieldOf(subclass, true)) == JSValue(token));
        AOT_TEST_CHECK(instance->read(privateFieldOf(subclass, false)) == jsNumber(7));
        for (const FieldData& own : subclass.fields())
            AOT_TEST_CHECK(own.isPrivate() == instance->isInSlot(own));
    }
}

// class A { x = 1; y = 2 }  class B extends A { y = 20; z; x = 10; m() { return "B.m" } }  const o = Reflect.construct(A, [], B)
// print(o.x, o.y, o.z, Object.hasOwn(o, "z"), o.m(), o instanceof B, new A().x)
// Nobody constructs B, so it has no shape, and its prototype object still selects what A allocates: an ordinary object, which has the fields of A.
static constexpr ASCIILiteral subclassNeverConstructedText = R"IR(layout L0 kind=Instance name="A" module=m0 constructor=f1 fields=[{key="x" declared={type=number flags=[IsExpected]} flags=[IsRequired]}, {key="y" declared={type=number flags=[IsExpected]} flags=[IsRequired]}]
layout L1 kind=Instance name="B" module=m0 base=L0 constructor=f2 fields=[{key="y" declared={type=number flags=[IsExpected]} flags=[IsRequired] originLayout=L0 originField=1}, {key="z" declared={type=anything} flags=[IsRequired]}, {key="x" declared={type=number flags=[IsExpected]} flags=[IsRequired] originLayout=L0 originField=0}] supertypes=[L0] flags=[HasHeritage]
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=function} flags=[IsImmutable]
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
    v7 = NewClosure function=f2
    v8 = NewClass v7, v5, v6 layout=L1
    PutModuleVariable v8 variable=g1
    v10 = NewClosure function=f3
    DefineMethod v8, v10 layout=L1 key="m"
    v12 = FinishClass v8 layout=L1
    v13 = Convert v12 declared={type=function flags=[IsExpected]}
    v14 = Convert v12 declared={type=function flags=[IsExpected]}
    v15 = Builtin builtin=GlobalThis
    v16 = PropGet v15 key="Reflect" [isFreeName]
    v17 = Convert v16 declared={type=builtin(Reflect) flags=[IsExpected]}
    v18 = PropGet v17 key="construct"
    v19 = Convert v18 declared={type=function flags=[IsExpected]}
    v20 = Convert v19 declared={type=builtin(ReflectConstruct) flags=[IsExpected]}
    v21 = Data data=d0
    v22 = Convert v21 declared={type=Array<never> flags=[IsExpected]}
    v23 = Call v20, v17, v5, v22, v14 calleeKey="construct" calleeName="construct"
    v24 = Convert v23 declared={type=L0 flags=[IsExpected]}
    v25 = Builtin builtin=GlobalThis
    v26 = PropGet v25 key="print" [isFreeName]
    v27 = PropGet v24 key="x"
    v28 = Convert v27 declared={type=number flags=[IsExpected]}
    v29 = PropGet v24 key="y"
    v30 = Convert v29 declared={type=number flags=[IsExpected]}
    v31 = PropGet v24 key="z"
    v32 = Builtin builtin=GlobalThis
    v33 = PropGet v32 key="Object" [isFreeName]
    v34 = Convert v33 declared={type=builtin(Object) flags=[IsExpected]}
    v35 = PropGet v34 key="hasOwn"
    v36 = Convert v35 declared={type=function flags=[IsExpected]}
    v37 = Convert v36 declared={type=builtin(ObjectHasOwn) flags=[IsExpected]}
    v38 = String string="z"
    v39 = Call v37, v34, v24, v38 calleeKey="hasOwn" calleeName="hasOwn"
    v40 = Convert v39 declared={type=boolean flags=[IsExpected]}
    v41 = PropGet v24 key="m"
    v42 = Call v41, v24 calleeKey="m" calleeName="m"
    v43 = InstanceOf v24, v14
    v44 = Construct v5 calleeName="A"
    v45 = Convert v44 declared={type=L0 flags=[IsExpected]}
    v46 = PropGet v45 key="x"
    v47 = Convert v46 declared={type=number flags=[IsExpected]}
    v48 = Call v26, -, v28, v30, v31, v40, v42, v43, v47 calleeName="print"
    v49 = Undefined
    Return v49
}
function f1 name="A" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Convert v2 declared={type=L0}
    v4 = CallDirect v3 function=f4
    Return v2
}
function f2 name="B" module=m0 kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee, HasExplicitName] thisType={type=anything} parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    Jump -> next b1()
  b1():
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L1
    v5 = Convert v4 declared={type=L1}
    v6 = CallDirect v5 function=f5
    Return v4
}
function f3 name="m" module=m0 kind=Method flags=[HasExplicitName] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = String string="B.m"
    v1 = Convert v0 declared={type=string flags=[IsExpected]}
    Return v1
}
function f4 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L0} results=[{type=anything}] {
  b0(v0: L0):
    Jump -> next b1()
  b1():
    v1 = Integer integer=1
    v2 = Convert v1 declared={type=number flags=[IsExpected]}
    PropDefine v0, v2 key="x"
    v4 = Integer integer=2
    v5 = Convert v4 declared={type=number flags=[IsExpected]}
    PropDefine v0, v5 key="y"
    v7 = Undefined
    Return v7
}
function f5 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L1} results=[{type=anything}] {
  b0(v0: L1):
    Jump -> next b1()
  b1():
    v1 = Integer integer=20
    v2 = Convert v1 declared={type=number flags=[IsExpected]}
    PropDefine v0, v2 key="y"
    v4 = Undefined
    PropDefine v0, v4 key="z"
    v6 = Integer integer=10
    v7 = Convert v6 declared={type=number flags=[IsExpected]}
    PropDefine v0, v7 key="x"
    v9 = Undefined
    Return v9
}
data d0 root=Array{contents=anything} module=m0
entry module=m0
)IR"_s;

// class A { constructor() { this.x = 1; delete this.x; this.seen = this.x } }  class B extends A { }  B.prototype.x = "proto x"
// const o = Reflect.construct(A, [], B);  print(o.seen, o.x, Object.hasOwn(o, "x"), o instanceof B)
// new.target of A is unknown, so `this` is the unknown value beside an instance of A, and at run time it is an instance of B: what A does to it by name counts for B too.
static constexpr ASCIILiteral deleteInBaseConstructorText = R"IR(layout L0 kind=Instance name="A" module=m0 constructor=f1 fields=[{key="x" declared={type=1 flags=[IsExpected]}}, {key="seen" declared={type=undefined flags=[IsExpected]}}]
layout L1 kind=Instance name="B" module=m0 base=L0 constructor=f2 supertypes=[L0] flags=[HasHeritage]
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=function} flags=[IsImmutable]
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
    v7 = NewClosure function=f2
    v8 = NewClass v7, v5, v6 layout=L1
    PutModuleVariable v8 variable=g1
    v10 = FinishClass v8 layout=L1
    v11 = Convert v10 declared={type=function flags=[IsExpected]}
    v12 = Convert v10 declared={type=function flags=[IsExpected]}
    v13 = PropGet v12 key="prototype"
    v14 = Convert v13 declared={type=L1 flags=[IsExpected]}
    v15 = String string="proto x"
    v16 = Convert v15 declared={type=number flags=[IsExpected]}
    PropSet v14, v16 key="x"
    v18 = Builtin builtin=GlobalThis
    v19 = PropGet v18 key="Reflect" [isFreeName]
    v20 = Convert v19 declared={type=builtin(Reflect) flags=[IsExpected]}
    v21 = PropGet v20 key="construct"
    v22 = Convert v21 declared={type=function flags=[IsExpected]}
    v23 = Convert v22 declared={type=builtin(ReflectConstruct) flags=[IsExpected]}
    v24 = Data data=d0
    v25 = Convert v24 declared={type=Array<never> flags=[IsExpected]}
    v26 = Call v23, v20, v5, v25, v12 calleeKey="construct" calleeName="construct"
    v27 = Convert v26 declared={type=L0 flags=[IsExpected]}
    v28 = Builtin builtin=GlobalThis
    v29 = PropGet v28 key="print" [isFreeName]
    v30 = PropGet v27 key="seen"
    v31 = PropGet v27 key="x"
    v32 = Convert v31 declared={type=number flags=[IsExpected]}
    v33 = Builtin builtin=GlobalThis
    v34 = PropGet v33 key="Object" [isFreeName]
    v35 = Convert v34 declared={type=builtin(Object) flags=[IsExpected]}
    v36 = PropGet v35 key="hasOwn"
    v37 = Convert v36 declared={type=function flags=[IsExpected]}
    v38 = Convert v37 declared={type=builtin(ObjectHasOwn) flags=[IsExpected]}
    v39 = String string="x"
    v40 = Call v38, v35, v27, v39 calleeKey="hasOwn" calleeName="hasOwn"
    v41 = Convert v40 declared={type=boolean flags=[IsExpected]}
    v42 = InstanceOf v27, v12
    v43 = Call v29, -, v30, v32, v41, v42 calleeName="print"
    v44 = Undefined
    Return v44
}
function f1 name="A" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Convert v2 declared={type=L0 flags=[IsExpected]}
    v4 = Integer integer=1
    v5 = Convert v4 declared={type=number flags=[IsExpected]}
    PropSet v3, v5 key="x"
    v7 = Convert v2 declared={type=L0 flags=[IsExpected]}
    v8 = PropDelete v7 key="x"
    v9 = Convert v2 declared={type=L0 flags=[IsExpected]}
    v10 = Convert v2 declared={type=L0 flags=[IsExpected]}
    v11 = PropGet v10 key="x"
    v12 = Convert v11 declared={type=undefined flags=[IsExpected]}
    PropSet v9, v12 key="seen"
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
function f2 name="B" module=m0 kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee, HasExplicitName] thisType={type=anything} parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    Jump -> next b1()
  b1():
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L1
    Return v4
}
data d0 root=Array{contents=anything} module=m0
entry module=m0
)IR"_s;

// TypeScript without useDefineForClassFields: class Base { n = 0; constructor() { (this as unknown as Heir).y = 5 } }  class Heir extends Base { y: number | undefined; z = 1 }
// print(Object.keys(new Heir()).join(), JSON.stringify(new Heir()))
// Nobody enumerates an instance of Base, so its slot for y has no absent state. Heir declares the name again, with an absent state: the placeholder of the object that Base allocates creates no field of the instance.
static constexpr ASCIILiteral fieldOfBaseDeclaredAgainText = R"IR(typesite w0 module=m0 kind=Field
typesite w1 module=m0 kind=CallResult
typesite w2 module=m0 kind=CallResult
typesite w3 module=m0 kind=Assertion
typesite w4 module=m0 kind=Field
layout L0 kind=Instance name="Base" module=m0 constructor=f1 fields=[{key="n" declared={type=number flags=[IsExpected]} flags=[IsRequired]}, {key="y" declared={type=5 flags=[IsExpected]}}]
layout L1 kind=Instance name="Heir" module=m0 base=L0 constructor=f2 fields=[{key="y" declared={type=undefined|null|number site=w0} flags=[IsRequired] originLayout=L0 originField=1}, {key="z" declared={type=number flags=[IsExpected]} flags=[IsRequired]}] supertypes=[L0] flags=[HasHeritage]
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=function} flags=[IsImmutable]
module m0 path="main.ts" displayPath="main.ts" body=f0
function f0 module=m0 kind=Module flags=[IsChecked] thisType={type=anything} results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = NewClass v0 layout=L0
    PutModuleVariable v1 variable=g0
    v3 = FinishClass v1 layout=L0
    v4 = Convert v3 declared={type=function}
    v5 = Convert v3 declared={type=function}
    v6 = ResolveHeritage v5
    v7 = NewClosure function=f2
    v8 = NewClass v7, v5, v6 layout=L1
    PutModuleVariable v8 variable=g1
    v10 = FinishClass v8 layout=L1
    v11 = Convert v10 declared={type=function}
    v12 = Convert v10 declared={type=function}
    v13 = Builtin builtin=GlobalThis
    v14 = PropGet v13 key="print" [isFreeName]
    v15 = Convert v14 declared={type=function flags=[IsExpected]}
    v16 = Builtin builtin=GlobalThis
    v17 = PropGet v16 key="Object" [isFreeName]
    v18 = Convert v17 declared={type=builtin(Object) flags=[IsExpected]}
    v19 = PropGet v18 key="keys"
    v20 = Convert v19 declared={type=function flags=[IsExpected]}
    v21 = Convert v20 declared={type=builtin(ObjectKeys) flags=[IsExpected]}
    v22 = Construct v12 calleeName="Heir"
    v23 = Convert v22 declared={type=L1 flags=[IsExpected]}
    v24 = Call v21, v18, v23 calleeKey="keys" calleeName="keys"
    v25 = Convert v24 declared={type=Array<string> flags=[IsExpected]}
    v26 = PropGet v25 key="join"
    v27 = Convert v26 declared={type=function flags=[IsExpected]}
    v28 = Call v27, v25 calleeKey="join" calleeName="join"
    v29 = Convert v28 declared={type=string flags=[IsExpected]}
    v30 = Builtin builtin=GlobalThis
    v31 = PropGet v30 key="JSON" [isFreeName]
    v32 = Convert v31 declared={type=builtin(JSON) flags=[IsExpected]}
    v33 = PropGet v32 key="stringify"
    v34 = Convert v33 declared={type=function flags=[IsExpected]}
    v35 = Convert v34 declared={type=builtin(JSONStringify) flags=[IsExpected]}
    v36 = Construct v12 calleeName="Heir"
    v37 = Convert v36 declared={type=L1 flags=[IsExpected]}
    v38 = Call v35, v32, v37 calleeKey="stringify" calleeName="stringify"
    v39 = Convert v38 declared={type=string flags=[IsExpected]}
    v40 = Call v15, -, v29, v39 calleeName="print"
    v41 = Undefined
    Return v41
}
function f1 name="Base" module=m0 kind=BaseConstructor flags=[HasNewTarget, IsChecked, HasExplicitName] thisType={type=anything} results=[{type=object}] {
  b0(v0: anything):
    Jump -> next b1()
  b1():
    v1 = GetConstructPrototype v0 layout=L0
    v2 = NewObject v1 layout=L0
    v3 = Convert v2 declared={type=L0}
    v4 = CallDirect v3 function=f3
    v5 = Convert v2 declared={type=L0 flags=[IsExpected]}
    v6 = Convert v5 declared={type=undefined|null|L1 site=w3} illTypedCause=FromLooseType
    v7 = Integer integer=5
    v8 = Convert v7 declared={type=undefined|null|number site=w4}
    PropSet v6, v8 key="y"
    v10 = Undefined
    v11 = TypeTest v10 testedType=object
    Branch v11 -> taken b2(| 0), notTaken b3(| 0)
  b2(v12):
    Jump -> next b4(v12)
  b3(v13):
    Jump -> next b4(v2)
  b4(v14):
    Return v14
}
function f2 name="Heir" module=m0 kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee, IsChecked, HasExplicitName] thisType={type=anything} parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    Jump -> next b1()
  b1():
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L1
    v5 = Convert v4 declared={type=L1}
    v6 = CallDirect v5 function=f4
    Return v4
}
function f3 module=m0 kind=Initializer flags=[HasThis, IsChecked, IsHiddenInStackTraces] thisType={type=L0} results=[{type=anything}] {
  b0(v0: L0):
    Jump -> next b1()
  b1():
    v1 = Integer
    v2 = Convert v1 declared={type=number flags=[IsExpected]}
    PropSet v0, v2 key="n"
    v4 = Undefined
    Return v4
}
function f4 module=m0 kind=Initializer flags=[HasThis, IsChecked, IsHiddenInStackTraces] thisType={type=L1} results=[{type=anything}] {
  b0(v0: L1):
    Jump -> next b1()
  b1():
    v1 = Integer integer=1
    v2 = Convert v1 declared={type=number flags=[IsExpected]}
    PropSet v0, v2 key="z"
    v4 = Undefined
    Return v4
}
entry module=m0
)IR"_s;

// class WithSetter { constructor(stores) { this.caught = "nothing"; if (stores) this.guarded = "stored" }  set guarded(value) { this.caught = "caught " + value }  get guarded() { return "from the getter" } }
// class DefinesOverSetter extends WithSetter { guarded = "defined" }  const first = new DefinesOverSetter(true), second = new DefinesOverSetter(false)
// print(first.caught, first.guarded, Object.keys(first).join(), second.caught, second.guarded, Object.keys(second).join())
// The store of the base constructor runs before the definition of the field, finds no own property and runs the setter.
static constexpr ASCIILiteral storeBeforeDefinitionText = R"IR(layout L0 kind=Instance name="WithSetter" module=m0 constructor=f1 fields=[{key="caught" declared={type="nothing" flags=[IsExpected]}}]
layout L1 kind=Instance name="DefinesOverSetter" module=m0 base=L0 constructor=f4 fields=[{key="guarded" declared={type=string flags=[IsExpected]} flags=[IsRequired]}] supertypes=[L0] flags=[HasHeritage]
variable g0 module=m0 declared={type=function} flags=[IsImmutable]
variable g1 module=m0 declared={type=function} flags=[IsImmutable]
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = NewClosure function=f1
    v1 = NewClass v0 layout=L0
    PutModuleVariable v1 variable=g0
    v3 = NewClosure function=f2
    DefineMethod v1, v3 layout=L0 methodKind=Setter key="guarded"
    v5 = NewClosure function=f3
    DefineMethod v1, v5 layout=L0 methodKind=Getter index=1 key="guarded"
    v7 = FinishClass v1 layout=L0
    v8 = Convert v7 declared={type=function flags=[IsExpected]}
    v9 = Convert v7 declared={type=function flags=[IsExpected]}
    v10 = ResolveHeritage v9
    v11 = NewClosure function=f4
    v12 = NewClass v11, v9, v10 layout=L1
    PutModuleVariable v12 variable=g1
    v14 = FinishClass v12 layout=L1
    v15 = Convert v14 declared={type=function flags=[IsExpected]}
    v16 = Convert v14 declared={type=function flags=[IsExpected]}
    v17 = Boolean boolean=true
    v18 = Construct v16, -, v17 calleeName="DefinesOverSetter"
    v19 = Convert v18 declared={type=L1 flags=[IsExpected]}
    v20 = Boolean
    v21 = Construct v16, -, v20 calleeName="DefinesOverSetter"
    v22 = Convert v21 declared={type=L1 flags=[IsExpected]}
    v23 = Builtin builtin=GlobalThis
    v24 = PropGet v23 key="print" [isFreeName]
    v25 = PropGet v19 key="caught"
    v26 = Convert v25 declared={type=string flags=[IsExpected]}
    v27 = PropGet v19 key="guarded"
    v28 = Convert v27 declared={type=string flags=[IsExpected]}
    v29 = Builtin builtin=GlobalThis
    v30 = PropGet v29 key="Object" [isFreeName]
    v31 = Convert v30 declared={type=builtin(Object) flags=[IsExpected]}
    v32 = PropGet v31 key="keys"
    v33 = Convert v32 declared={type=function flags=[IsExpected]}
    v34 = Convert v33 declared={type=builtin(ObjectKeys) flags=[IsExpected]}
    v35 = Call v34, v31, v19 calleeKey="keys" calleeName="keys"
    v36 = Convert v35 declared={type=Array<string> flags=[IsExpected]}
    v37 = PropGet v36 key="join"
    v38 = Convert v37 declared={type=function flags=[IsExpected]}
    v39 = Call v38, v36 calleeKey="join" calleeName="join"
    v40 = Convert v39 declared={type=string flags=[IsExpected]}
    v41 = PropGet v22 key="caught"
    v42 = Convert v41 declared={type=string flags=[IsExpected]}
    v43 = PropGet v22 key="guarded"
    v44 = Convert v43 declared={type=string flags=[IsExpected]}
    v45 = Builtin builtin=GlobalThis
    v46 = PropGet v45 key="Object" [isFreeName]
    v47 = Convert v46 declared={type=builtin(Object) flags=[IsExpected]}
    v48 = PropGet v47 key="keys"
    v49 = Convert v48 declared={type=function flags=[IsExpected]}
    v50 = Convert v49 declared={type=builtin(ObjectKeys) flags=[IsExpected]}
    v51 = Call v50, v47, v22 calleeKey="keys" calleeName="keys"
    v52 = Convert v51 declared={type=Array<string> flags=[IsExpected]}
    v53 = PropGet v52 key="join"
    v54 = Convert v53 declared={type=function flags=[IsExpected]}
    v55 = Call v54, v52 calleeKey="join" calleeName="join"
    v56 = Convert v55 declared={type=string flags=[IsExpected]}
    v57 = Call v24, -, v26, v28, v40, v42, v44, v56 calleeName="print"
    v58 = Undefined
    Return v58
}
function f1 name="WithSetter" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=object}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = GetConstructPrototype v0 layout=L0
    v3 = NewObject v2 layout=L0
    v4 = Convert v3 declared={type=L0 flags=[IsExpected]}
    v5 = String string="nothing"
    v6 = Convert v5 declared={type=string flags=[IsExpected]}
    PropSet v4, v6 key="caught"
    v8 = ToBoolean v1
    Branch v8 -> taken b2(| 0), notTaken b4(| 0)
  b2(v9):
    Jump -> next b3()
  b3():
    v10 = Convert v3 declared={type=L0 flags=[IsExpected]}
    v11 = String string="stored"
    v12 = Convert v11 declared={type=string flags=[IsExpected]}
    PropSet v10, v12 key="guarded"
    Jump -> next b5()
  b4(v14):
    Jump -> next b5()
  b5():
    v15 = Undefined
    v16 = TypeTest v15 testedType=object
    Branch v16 -> taken b6(| 0), notTaken b7(| 0)
  b6(v17):
    Jump -> next b8(v17)
  b7(v18):
    Jump -> next b8(v3)
  b8(v19):
    Return v19
}
function f2 name="guarded" module=m0 kind=Setter flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=string flags=[IsExpected]}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    Jump -> next b1()
  b1():
    v2 = String string="caught "
    v3 = Add v2, v1
    v4 = Convert v3 declared={type=string flags=[IsExpected]}
    PropSet v0, v4 key="caught"
    v6 = Undefined
    Return v6
}
function f3 name="guarded" module=m0 kind=Getter flags=[HasExplicitName] thisType={type=anything} results=[{type=string flags=[IsExpected]}] {
  b0():
    Jump -> next b1()
  b1():
    v0 = String string="from the getter"
    v1 = Convert v0 declared={type=string flags=[IsExpected]}
    Return v1
}
function f4 name="DefinesOverSetter" module=m0 kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee, HasExplicitName] thisType={type=anything} parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    Jump -> next b1()
  b1():
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L1
    v5 = Convert v4 declared={type=L1}
    v6 = CallDirect v5 function=f5
    Return v4
}
function f5 module=m0 kind=Initializer flags=[HasThis, IsHiddenInStackTraces] thisType={type=L1} results=[{type=anything}] {
  b0(v0: L1):
    Jump -> next b1()
  b1():
    v1 = String string="defined"
    v2 = Convert v1 declared={type=string flags=[IsExpected]}
    PropDefine v0, v2 key="guarded"
    v4 = Undefined
    Return v4
}
entry module=m0
)IR"_s;

// let seen = ""  function Base(v) { this.tag = v }  class Logged extends Base { set tag(v) { seen = seen + "setter " + v } }  new Logged(1); new Base(2)
// class Valued { constructor(v) { this.value = v } }  const observable = Parent => class extends Parent { set value(v) { this.stored = v + v }  get value() { return this.stored } }
// const Watched = observable(Valued); new Valued(1); const watched = new Watched(21); print(seen, watched.value, watched.stored, Object.keys(watched).join())
// The instances of the classes below have the slot of the field that the base constructor stores, and the store runs their setter: the slot stays empty. Nobody reads the fields.
static constexpr ASCIILiteral storeRunsSetterOfSubclassText = R"IR(layout L0 kind=Instance name="Base" module=m0 constructor=f1 fields=[{key="tag" declared={type=anything}}] flags=[IsConstructorFunction]
layout L1 kind=Instance name="Logged" module=m0 constructor=f2 flags=[HasHeritage]
layout L2 kind=Instance name="Valued" module=m0 constructor=f4 fields=[{key="value" declared={type=anything}}]
layout L3 kind=Instance name="Watched" module=m0 constructor=f6 fields=[{key="stored" declared={type=anything}}] flags=[HasHeritage]
variable g0 name="seen" module=m0 declared={type=anything}
module m0 path="main.js" displayPath="main.js" body=f0
function f0 module=m0 kind=Module thisType={type=anything} results=[{type=anything}] {
  b0():
    v0 = String string=""
    PutModuleVariable v0 variable=g0
    v2 = NewClosure function=f1
    v3 = ResolveHeritage v2
    v4 = NewClosure function=f2
    v5 = NewClass v4, v2, v3 layout=L1
    v6 = NewClosure function=f3
    DefineMethod v5, v6 layout=L1 methodKind=Setter key="tag"
    v8 = FinishClass v5 layout=L1
    v9 = Integer integer=1
    v10 = Integer integer=2
    v11 = Construct v8, -, v9 calleeName="Logged"
    v12 = Construct v2, -, v10 calleeName="Base"
    v13 = NewClosure function=f4
    v14 = NewClass v13 layout=L2
    v15 = FinishClass v14 layout=L2
    v16 = NewClosure function=f5
    v17 = Call v16, -, v15 calleeName="observable"
    v18 = Construct v15, -, v9 calleeName="Valued"
    v19 = Integer integer=21
    v20 = Construct v17, -, v19 calleeName="Watched"
    v21 = Builtin builtin=GlobalThis
    v22 = PropGet v21 key="print" [isFreeName]
    v23 = GetModuleVariable variable=g0
    v24 = PropGet v20 key="value"
    v25 = PropGet v20 key="stored"
    v26 = PropGet v21 key="Object" [isFreeName]
    v27 = PropGet v26 key="keys"
    v28 = Call v27, v26, v20 calleeKey="keys" calleeName="keys"
    v29 = PropGet v28 key="join"
    v30 = Call v29, v28 calleeKey="join" calleeName="join"
    v31 = Call v22, -, v23, v24, v25, v30 calleeName="print"
    v32 = Undefined
    Return v32
}
function f1 name="Base" module=m0 flags=[HasThis, HasExplicitName] length=1 thisType={type=L0 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    PropSet v0, v1 key="tag"
    v3 = Undefined
    Return v3
}
function f2 name="Logged" module=m0 kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee, HasExplicitName] thisType={type=anything} parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L1
    Return v4
}
function f3 name="tag" module=m0 kind=Setter flags=[HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetModuleVariable variable=g0
    v2 = String string="setter "
    v3 = Add v1, v2
    v4 = Add v3, v0
    PutModuleVariable v4 variable=g0
    v6 = Undefined
    Return v6
}
function f4 name="Valued" module=m0 kind=BaseConstructor flags=[HasNewTarget, HasExplicitName] length=1 thisType={type=anything} parameters=[{type=anything}] results=[{type=object}] {
  b0(v0: anything, v1: anything):
    v2 = GetConstructPrototype v0 layout=L2
    v3 = NewObject v2 layout=L2
    PropSet v3, v1 key="value"
    Return v3
}
function f5 name="observable" module=m0 length=1 parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = ResolveHeritage v0
    v2 = NewClosure function=f6
    v3 = NewClass v2, v0, v1 layout=L3
    v4 = NewClosure function=f7
    DefineMethod v3, v4 layout=L3 methodKind=Setter key="value"
    v6 = NewClosure function=f8
    DefineMethod v3, v6 layout=L3 methodKind=Getter index=1 key="value"
    v8 = FinishClass v3 layout=L3
    Return v8
}
function f6 module=m0 kind=DerivedConstructor flags=[HasRestParameter, HasNewTarget, TakesCallee] thisType={type=anything} parameters=[{type=Array}] results=[{type=object}] {
  b0(v0: function, v1: anything, v2: Array):
    v3 = GetSuperBase v0
    v4 = SuperConstructList v3, v1, v2 layout=L3
    Return v4
}
function f7 name="value" module=m0 kind=Setter flags=[HasThis, HasExplicitName] length=1 thisType={type=L3 flags=[IsExpected]} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = Add v1, v1
    PropSet v0, v2 key="stored"
    v4 = Undefined
    Return v4
}
function f8 name="value" module=m0 kind=Getter flags=[HasThis, HasExplicitName] thisType={type=L3 flags=[IsExpected]} results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="stored"
    Return v1
}
entry module=m0
)IR"_s;

static void checkInShellWithAndWithoutPasses(ASCIILiteral programText, ASCIILiteral expectedOutput)
{
    checkOutputOfProgramInShell(programText, expectedOutput, { });
    checkOutputOfProgramInShell(programText, expectedOutput, { "validate"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { "disable=pass:PlaceTypeTests"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { "disable=pass:InlineCalls"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { "disable=pass:SinkAllocations"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { "only-passes=ResolveAccesses"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { "references=narrow"_s, "validate"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { "admit=Inspector"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { "admit=Inspector"_s, "validate"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { "admit=Inspector"_s, "disable=pass:PlaceTypeTests"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { "admit=Inspector"_s, "disable=pass:InlineCalls"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { "admit=Inspector"_s, "disable=pass:SinkAllocations"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { "admit=Inspector"_s, "only-passes=ResolveAccesses"_s });
    checkOutputOfProgramInShell(programText, expectedOutput, { "admit=Inspector"_s, "references=narrow"_s, "validate"_s });
}

static void testClassFactoryOverPrivateField()
{
    checkInShellWithAndWithoutPasses(classFactoryText, "7 7 7 1 2 true true false tag\n"_s);
}

static void testOverriddenMethodCalledByBaseConstructor()
{
    checkInShellWithAndWithoutPasses(overriddenMethodText, "7 5 seen,q {\"seen\":7,\"q\":5}\n"_s);
}

static void testStoreInBaseConstructorBesideFieldInitializer()
{
    checkInShellWithAndWithoutPasses(storeInBaseConstructorText, "{\"env\":{},\"request\":1,\"extra\":[]} env,request,extra 1 {\"env\":{},\"request\":2}\n"_s);
}

static void testAbstractClassesOverConstructorFunction()
{
    checkInShellWithAndWithoutPasses(abstractClassesText, "1 B.m true\n{\"x\":1,\"left\":2} {\"x\":1,\"right\":\"r\"} A.m true false true\n"_s);
}

static void testClassesNotBelowConstructor()
{
    checkInShellWithAndWithoutPasses(classesNotBelowConstructorText, "A.m B.m false true Stranger.m false TypeError 6\n"_s);
}

static void testSubclassNeverConstructed()
{
    checkInShellWithAndWithoutPasses(subclassNeverConstructedText, "1 2 undefined false B.m true 1\n"_s);
}

static void testDeleteInBaseConstructorForSubclass()
{
    checkInShellWithAndWithoutPasses(deleteInBaseConstructorText, "proto x proto x false true\n"_s);
}

static void testFieldOfBaseDeclaredAgainBySubclass()
{
    checkInShellWithAndWithoutPasses(fieldOfBaseDeclaredAgainText, "n,y,z {\"n\":0,\"y\":5,\"z\":1}\n"_s);
}

static void testStoreOfBaseConstructorBeforeDefinitionBySubclass()
{
    checkInShellWithAndWithoutPasses(storeBeforeDefinitionText, "caught stored defined caught,guarded nothing defined caught,guarded\n"_s);
}

static void testStoreOfBaseConstructorRunsSetterOfSubclass()
{
    checkInShellWithAndWithoutPasses(storeRunsSetterOfSubclassText, "setter 1 42 42 stored\n"_s);
    checkOutputOfProgramInShell(storeRunsSetterOfSubclassText, "setter 1 42 42 stored\n"_s, { "disable=pass:ResolveCalls"_s });
    checkOutputOfProgramInShell(storeRunsSetterOfSubclassText, "setter 1 42 42 stored\n"_s, { "disable=flow-results"_s });
}

std::span<const NativeTest> thisAllocationProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "this-allocation/programs/private-fields-move-to-the-subclass-instance"_s, runNativeTestWithLoadedProgram<thisAllocationProgramTests, classFactoryText, checkPrivateFieldsMoveToTheSubclassInstance> },
        { "this-allocation/programs/class-factory-over-private-field"_s, testClassFactoryOverPrivateField },
        { "this-allocation/programs/overridden-method-called-by-base-constructor"_s, testOverriddenMethodCalledByBaseConstructor },
        { "this-allocation/programs/store-in-base-constructor-beside-field-initializer"_s, testStoreInBaseConstructorBesideFieldInitializer },
        { "this-allocation/programs/abstract-classes-over-constructor-function"_s, testAbstractClassesOverConstructorFunction },
        { "this-allocation/programs/classes-not-below-constructor"_s, testClassesNotBelowConstructor },
        { "this-allocation/programs/subclass-never-constructed"_s, testSubclassNeverConstructed },
        { "this-allocation/programs/delete-in-base-constructor-for-subclass"_s, testDeleteInBaseConstructorForSubclass },
        { "this-allocation/programs/field-of-base-declared-again-by-subclass"_s, testFieldOfBaseDeclaredAgainBySubclass },
        { "this-allocation/programs/store-of-base-constructor-before-definition-by-subclass"_s, testStoreOfBaseConstructorBeforeDefinitionBySubclass },
        { "this-allocation/programs/store-of-base-constructor-runs-setter-of-subclass"_s, testStoreOfBaseConstructorRunsSetterOfSubclass },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
