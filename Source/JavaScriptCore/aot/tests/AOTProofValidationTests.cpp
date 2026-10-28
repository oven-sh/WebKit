/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTHostEntriesTestSupport.h"
#include "AOTRuntimeTable.h"
#include "AOTRuntimeValidation.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTVector.h"
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {


// The model answers the integer in a field, the native function a string. The sum with 1 folds.
static constexpr ASCIILiteral foldedConstant = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Source" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Integer integer=1
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = FinishObject v1
    v3 = PropGet v2 key="toLocaleString"
    v4 = Call v3, v2 calleeKey="toLocaleString"
    v5 = Add v4, v0
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="print" [isFreeName]
    v8 = Call v7, -, v5
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="a"
    Return v1
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

// The model answers a number, the native function a string. The test for a string folds.
static constexpr ASCIILiteral foldedTypeTest = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Source" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=Math
    v1 = PropGet v0 key="random"
    v2 = Call v1, v0 calleeKey="random"
    v3 = NewObject -, v2 layout=L0 fields=[0]
    v4 = FinishObject v3
    v5 = PropGet v4 key="toLocaleString"
    v6 = Call v5, v4 calleeKey="toLocaleString"
    v7 = Builtin builtin=GlobalThis
    v8 = PropGet v7 key="print" [isFreeName]
    v9 = TypeTest v6 testedType=string
    Branch v9 -> taken b1(), notTaken b2()
  b1():
    v10 = String string="string"
    v11 = Call v8, -, v10
    Return
  b2():
    v12 = String string="no string"
    v13 = Call v8, -, v12
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="a"
    Return v1
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

// The model answers a number, the native function a string. The product is computed on numbers.
static constexpr ASCIILiteral numberInArithmetic = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Source" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=Math
    v1 = PropGet v0 key="random"
    v2 = Call v1, v0 calleeKey="random"
    v3 = NewObject -, v2 layout=L0 fields=[0]
    v4 = FinishObject v3
    v5 = PropGet v4 key="toLocaleString"
    v6 = Call v5, v4 calleeKey="toLocaleString"
    v7 = Mul v6, v2
    v8 = Builtin builtin=GlobalThis
    v9 = PropGet v8 key="print" [isFreeName]
    v10 = Call v9, -, v7
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="a"
    Return v1
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

// The model answers an object of the layout Modeled, the native function a string. The read of m is a read of a slot.
static constexpr ASCIILiteral layoutAtSlotRead = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Source" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Modeled" fields=[{key="m" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=Math
    v1 = PropGet v0 key="random"
    v2 = Call v1, v0 calleeKey="random"
    v3 = NewObject -, v2 layout=L0 fields=[0]
    v4 = FinishObject v3
    v5 = PropGet v4 key="toLocaleString"
    v6 = Call v5, v4 calleeKey="toLocaleString"
    v7 = PropGet v6 key="m"
    v8 = Builtin builtin=GlobalThis
    v9 = PropGet v8 key="print" [isFreeName]
    v10 = Call v9, -, v7
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="a"
    v2 = NewObject -, v1 layout=L1 fields=[0]
    v3 = FinishObject v2
    Return v3
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

// The model answers a closure of "modeled", the native function what toString answers, a closure of "real". The call becomes a direct call.
static constexpr ASCIILiteral resolvedCallee = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Source" fields=[{key="toString" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f2
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = FinishObject v1
    v3 = PropGet v2 key="toLocaleString"
    v4 = Call v3, v2 calleeKey="toLocaleString"
    v5 = Call v4
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="print" [isFreeName]
    v8 = Call v7, -, v5
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="toString"
    v2 = Call v1, v0 calleeKey="toString"
    v3 = NewClosure function=f4
    Return v3
}
function f2 name="toString" flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = NewClosure function=f3
    Return v1
}
function f3 name="real" results=[{type=anything}] {
    v0 = String string="real"
    Return v0
}
function f4 name="modeled" results=[{type=anything}] {
    v0 = String string="modeled"
    Return v0
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

// Flow says nothing of a value at which nothing arrives, so no model makes code unreachable. The text does: it ends a block with Unreachable, and the script of the shell decides whether the block is entered.
static constexpr ASCIILiteral unreachableBlock = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="enters" [isFreeName]
    v2 = ToBoolean v1
    v3 = PropGet v0 key="print" [isFreeName]
    Branch v2 -> taken b1(), notTaken b2()
  b1():
    Unreachable
  b2():
    v4 = String string="went on"
    v5 = Call v3, -, v4
    Return
}
)IR"_s;

// The model reads toString and does not call it, the native function calls it.
static constexpr ASCIILiteral functionThatIsNeverReached = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Source" fields=[{key="toString" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f2
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = FinishObject v1
    v3 = PropGet v2 key="toLocaleString"
    v4 = Call v3, v2 calleeKey="toLocaleString"
    v5 = Builtin builtin=GlobalThis
    v6 = PropGet v5 key="print" [isFreeName]
    v7 = Call v6, -, v4
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="toString"
    v2 = TypeOf v1
    Return v2
}
function f2 name="toString" flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = String string="ran"
    v4 = Call v2, -, v3
    v5 = String string="text"
    Return v5
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

// As leakedClosure, and the program calls the closure itself, once. One operation enters the function, so it runs at most once, is called directly and has no entry for a call by value.
static constexpr ASCIILiteral functionThatRunsOnce = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Holder" fields=[{key="k" declared={type=anything} flags=[IsRequired]}, {key="toString" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f2
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="sink" [isFreeName]
    v3 = NewObject -, v0, v2 layout=L0 fields=[0, 1]
    v4 = FinishObject v3
    v5 = PropGet v4 key="toLocaleString"
    v6 = Call v5, v4 calleeKey="toLocaleString"
    v7 = PropGet v1 key="print" [isFreeName]
    v8 = Call v0
    v9 = Call v7, -, v8
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="toString"
    v2 = TypeOf v1
    v3 = PropGet v0 key="k"
    v4 = TypeOf v3
    Return
}
function f2 name="once" results=[{type=anything}] {
    v0 = String string="ran"
    Return v0
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

static constexpr ASCIILiteral leakedObject = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Holder" fields=[{key="k" declared={type=anything} flags=[IsRequired]}, {key="toString" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Inner" fields=[{key="x" declared={type=anything} flags=[IsRequired]}, {key="unread" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=Math
    v1 = PropGet v0 key="random"
    v2 = Call v1, v0 calleeKey="random"
    v3 = Integer integer=5
    v4 = NewObject -, v2, v3 layout=L1 fields=[0, 1]
    v5 = FinishObject v4
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="sink" [isFreeName]
    v8 = NewObject -, v5, v7 layout=L0 fields=[0, 1]
    v9 = FinishObject v8
    v10 = PropGet v9 key="toLocaleString"
    v11 = Call v10, v9 calleeKey="toLocaleString"
    v12 = PropGet v5 key="x"
    v13 = StrictEq v12, v2
    v14 = PropGet v6 key="print" [isFreeName]
    v15 = Call v14, -, v13
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="toString"
    v2 = TypeOf v1
    v3 = PropGet v0 key="k"
    v4 = TypeOf v3
    Return
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

// As leakedObject, with an array of numbers.
static constexpr ASCIILiteral leakedArray = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Holder" fields=[{key="k" declared={type=anything} flags=[IsRequired]}, {key="toString" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=Math
    v1 = PropGet v0 key="random"
    v2 = Call v1, v0 calleeKey="random"
    v3 = NewArray v2, v2 contents=anything holes=[]
    v4 = Builtin builtin=GlobalThis
    v5 = PropGet v4 key="sink" [isFreeName]
    v6 = NewObject -, v3, v5 layout=L0 fields=[0, 1]
    v7 = FinishObject v6
    v8 = PropGet v7 key="toLocaleString"
    v9 = Call v8, v7 calleeKey="toLocaleString"
    v10 = Integer integer=1
    v11 = ElemGet v3, v10
    v12 = Mul v11, v2
    v13 = TypeOf v11
    v14 = PropGet v3 key="length"
    v15 = PropGet v4 key="print" [isFreeName]
    v16 = Call v15, -, v13, v14
    ElemSet v3, v10, v2 mode=Strict
    v18 = PropGet v4 key="after" [isFreeName]
    v19 = Call v18
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="toString"
    v2 = TypeOf v1
    v3 = PropGet v0 key="k"
    v4 = TypeOf v3
    Return
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

static constexpr ASCIILiteral effectsOfCall = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Source" fields=[{key="toString" declared={type=anything} flags=[IsRequired]}, {key="a" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="mutate" [isFreeName]
    v2 = Builtin builtin=Math
    v3 = PropGet v2 key="random"
    v4 = Call v3, v2 calleeKey="random"
    v5 = NewObject -, v1, v4 layout=L0 fields=[0, 1]
    v6 = FinishObject v5
    PropSet v0, v6 key="kept" mode=Sloppy
    v8 = PropGet v6 key="a"
    v9 = PropGet v6 key="toLocaleString"
    v10 = Call v9, v6 calleeKey="toLocaleString"
    v11 = PropGet v6 key="a"
    v12 = StrictEq v8, v11
    v13 = PropGet v0 key="print" [isFreeName]
    v14 = Call v13, -, v12
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="toString"
    v2 = TypeOf v1
    Return v2
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

// A read through an expected layout is behind a test, which an option word drops.
static constexpr ASCIILiteral guardedShape = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Point" fields=[{key="x" declared={type=number} flags=[IsRequired]}, {key="y" declared={type=number} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Double number=1.5
    v1 = NewObject -, v0, v0 layout=L0 fields=[0, 1]
    v2 = FinishObject v1
    v3 = NewObject -, v0, v0 layout=L0 fields=[0, 1]
    v4 = FinishObject v3
    v5 = PropDelete v4 key="x" mode=Strict
    v6 = NewClosure function=f1
    v7 = Builtin builtin=GlobalThis
    v8 = PropGet v7 key="register" [isFreeName]
    v9 = Call v8, -, v6, v2, v4
    Return
}
function f1 name="readY" parameters=[{type=L0 flags=[IsExpected]}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="y"
    Return v1
}
)IR"_s;

// As the leaked array, and what leaks is a map from strings to numbers of which the program reads only the size afterwards.
static constexpr ASCIILiteral leakedMap = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Holder" fields=[{key="k" declared={type=anything} flags=[IsRequired]}, {key="toString" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=Math
    v1 = PropGet v0 key="random"
    v2 = Call v1, v0 calleeKey="random"
    v3 = Builtin builtin=Map
    v4 = Construct v3
    v5 = String string="a"
    v6 = PropGet v4 key="set"
    v7 = Call v6, v4, v5, v2 calleeKey="set"
    v8 = Builtin builtin=GlobalThis
    v9 = PropGet v8 key="sink" [isFreeName]
    v10 = NewObject -, v4, v9 layout=L0 fields=[0, 1]
    v11 = FinishObject v10
    v12 = PropGet v11 key="toLocaleString"
    v13 = Call v12, v11 calleeKey="toLocaleString"
    v14 = PropGet v4 key="size"
    v15 = PropGet v8 key="print" [isFreeName]
    v16 = Call v15, -, v14
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="toString"
    v2 = TypeOf v1
    v3 = PropGet v0 key="k"
    v4 = TypeOf v3
    Return
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

// As leakedObject, and what leaks is an instance of a class. The program calls a method of the class, reads a name that the class does not have and calls a method that it inherits from Object.prototype.
static constexpr ASCIILiteral leakedInstance = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Holder" fields=[{key="k" declared={type=anything} flags=[IsRequired]}, {key="toString" declared={type=anything} flags=[IsRequired]}]
layout L1 kind=Instance name="Complete" constructor=f2
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f2
    v1 = NewClass v0 layout=L1
    v2 = NewClosure function=f3
    DefineMethod v1, v2 layout=L1 key="method"
    v4 = FinishClass v1 layout=L1
    v5 = Construct v4
    v6 = Builtin builtin=GlobalThis
    v7 = PropGet v6 key="sink" [isFreeName]
    v8 = NewObject -, v5, v7 layout=L0 fields=[0, 1]
    v9 = FinishObject v8
    v10 = PropGet v9 key="toLocaleString"
    v11 = Call v10, v9 calleeKey="toLocaleString"
    v12 = PropGet v6 key="print" [isFreeName]
    v13 = PropGet v5 key="method"
    v14 = Call v13, v5 calleeKey="method"
    v15 = Call v12, -, v14
    v16 = PropGet v5 key="other"
    v17 = Call v12, -, v16
    v18 = PropGet v5 key="toString"
    v19 = Call v18, v5 calleeKey="toString"
    v20 = Call v12, -, v19
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="toString"
    v2 = TypeOf v1
    v3 = PropGet v0 key="k"
    v4 = TypeOf v3
    Return
}
function f2 name="Complete" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
    v1 = GetConstructPrototype v0 layout=L1
    v2 = NewObject v1 layout=L1 fields=[]
    Return v2
}
function f3 name="method" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = String string="as defined"
    Return v1
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

// As leakedObject, and what leaks is a closure, of which the program calls `call`.
static constexpr ASCIILiteral leakedClosure = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Holder" fields=[{key="k" declared={type=anything} flags=[IsRequired]}, {key="toString" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f2
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="sink" [isFreeName]
    v3 = NewObject -, v0, v2 layout=L0 fields=[0, 1]
    v4 = FinishObject v3
    v5 = PropGet v4 key="toLocaleString"
    v6 = Call v5, v4 calleeKey="toLocaleString"
    v7 = PropGet v1 key="print" [isFreeName]
    v8 = PropGet v0 key="call"
    v9 = Call v8, v0 calleeKey="call"
    v10 = Call v7, -, v9
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="toString"
    v2 = TypeOf v1
    v3 = PropGet v0 key="k"
    v4 = TypeOf v3
    Return
}
function f2 name="plain" results=[{type=anything}] {
    v0 = String string="as created"
    Return v0
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

static constexpr ASCIILiteral guardKeptAcrossCall = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Source" fields=[{key="toString" declared={type=anything} flags=[IsRequired]}, {key="a" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Descriptor" fields=[{key="get" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="mutate" [isFreeName]
    v2 = Builtin builtin=Math
    v3 = PropGet v2 key="random"
    v4 = Call v3, v2 calleeKey="random"
    v5 = NewObject -, v1, v4 layout=L0 fields=[0, 1]
    v6 = FinishObject v5
    v7 = PropDelete v6 key="a" mode=Sloppy
    v8 = Builtin builtin=Object
    v9 = PropGet v8 key="freeze"
    v10 = Call v9, v8, v6 calleeKey="freeze"
    v11 = NewObject -, v1, v4 layout=L0 fields=[0, 1]
    v12 = FinishObject v11
    v13 = NewClosure function=f2
    v14 = NewObject -, v13 layout=L1 fields=[0]
    v15 = FinishObject v14
    v16 = Integer integer=0
    v17 = PropGet v8 key="defineProperty"
    v18 = Call v17, v8, v12, v16, v15 calleeKey="defineProperty"
    v19 = NewObject -, v1, v4 layout=L0 fields=[0, 1]
    v20 = FinishObject v19
    PropSet v0, v20 key="kept" mode=Sloppy
    v22 = Integer integer=0
    Jump -> next b1(v22)
  b1(v23):
    LoopHint
    v25 = PropGet v20 key="a"
    v26 = PropGet v20 key="toLocaleString"
    v27 = Call v26, v20 calleeKey="toLocaleString"
    v28 = PropGet v20 key="a"
    v29 = StrictEq v25, v28
    v30 = PropGet v0 key="print" [isFreeName]
    v31 = Call v30, -, v29
    v32 = Increment v23
    v33 = Less v32, v4
    Branch v33 -> taken b1(v32), notTaken b2()
  b2():
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="toString"
    v2 = TypeOf v1
    Return v2
}
function f2 name="getter" results=[{type=anything}] {
    v0 = Integer integer=1
    Return v0
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

// As above, and another object of the layout also gets another prototype, so an inherited member is found under guards. It is read before and behind the call.
static constexpr ASCIILiteral prototypeGuardKeptAcrossCall = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Source" fields=[{key="toString" declared={type=anything} flags=[IsRequired]}, {key="a" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Descriptor" fields=[{key="get" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="mutate" [isFreeName]
    v2 = Builtin builtin=Math
    v3 = PropGet v2 key="random"
    v4 = Call v3, v2 calleeKey="random"
    v5 = NewObject -, v1, v4 layout=L0 fields=[0, 1]
    v6 = FinishObject v5
    v7 = PropDelete v6 key="a" mode=Sloppy
    v8 = Builtin builtin=Object
    v9 = PropGet v8 key="freeze"
    v10 = Call v9, v8, v6 calleeKey="freeze"
    v11 = NewObject -, v1, v4 layout=L0 fields=[0, 1]
    v12 = FinishObject v11
    v13 = NewClosure function=f2
    v14 = NewObject -, v13 layout=L1 fields=[0]
    v15 = FinishObject v14
    v16 = Integer integer=0
    v17 = PropGet v8 key="defineProperty"
    v18 = Call v17, v8, v12, v16, v15 calleeKey="defineProperty"
    v19 = PropGet v8 key="setPrototypeOf"
    v20 = Call v19, v8, v12, v15 calleeKey="setPrototypeOf"
    v21 = NewObject -, v1, v4 layout=L0 fields=[0, 1]
    v22 = FinishObject v21
    PropSet v0, v22 key="kept" mode=Sloppy
    v24 = Integer integer=0
    Jump -> next b1(v24)
  b1(v25):
    LoopHint
    v27 = PropGet v22 key="a"
    v28 = PropGet v22 key="toLocaleString"
    v29 = Call v28, v22 calleeKey="toLocaleString"
    v30 = PropGet v22 key="toLocaleString"
    v31 = StrictEq v28, v30
    v32 = PropGet v0 key="print" [isFreeName]
    v33 = Call v32, -, v31
    v34 = Increment v25
    v35 = Less v34, v4
    Branch v35 -> taken b1(v34), notTaken b2()
  b2():
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="toString"
    v2 = TypeOf v1
    Return v2
}
function f2 name="getter" results=[{type=anything}] {
    v0 = Integer integer=1
    Return v0
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

// The numbers of the models move with the table of built-ins.
static UTF8CString withNumbersOfModels(ASCIILiteral programText)
{
    String text = makeStringByReplacingAll(String(programText), "$toLocaleString"_s, String::number(modelOf(Builtins::ObjectPrototypeToLocaleString).index()));
    return text.utf8();
}

static ChildProcessResult runValidatingBuildInShell(ASCIILiteral programText, OptionWords optionWords, ASCIILiteral statementsBeforeProgram)
{
    UTF8CString numberedText = withNumbersOfModels(programText);
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileForShellOnThisThread(ASCIILiteral::fromLiteralUnsafe(byteCast<char>(numberedText.data())), optionWords);
    });
    TemporaryFile image(".image"_s, output->image.span());
    Vector<String> arguments { makeString("--aotImagePath="_s, image.path()), String("-e"_s), makeString(statementsBeforeProgram, " runCompiledEntryPoint("_s, output->entryModuleIDs[0], ')') };
    return ChildProcessLauncher(shellPath(), { }).run(arguments.span());
}

static void checkValidationFails(ASCIILiteral programText, ValidationFailureKind kind, ASCIILiteral statementsBeforeProgram = ""_s, OptionWords optionWords = { "validate"_s })
{
    ChildProcessResult result = runValidatingBuildInShell(programText, optionWords, statementsBeforeProgram);
    bool hasFailed = result.outcome == ChildProcessOutcome::Crashed && result.output.contains(makeString("AOT validation: "_s, nameOf(kind), ' '));
    // A read behind the end of a cell is reported by AddressSanitizer first.
    if (AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE)
        hasFailed |= result.output.contains("AOT: compiled code touches poisoned memory"_s);
    if (!hasFailed)
        dataLogLn(result.description(), "\n", result.output);
    AOT_TEST_CHECK(hasFailed);
}

static void testFoldedConstant()
{
    checkValidationFails(foldedConstant, ValidationFailureKind::AssumedType);
}

static void testFoldedTypeTest()
{
    checkValidationFails(foldedTypeTest, ValidationFailureKind::AssumedType);
}

static void testNumberInArithmetic()
{
    checkValidationFails(numberInArithmetic, ValidationFailureKind::AssumedType);
}

static void testLayoutAtSlotRead()
{
    checkValidationFails(layoutAtSlotRead, ValidationFailureKind::AssumedType);
}

static void testResolvedCallee()
{
    checkValidationFails(resolvedCallee, ValidationFailureKind::AssumedType);
}

// Without exact functions in types only the stamp of the call names the callees.
static void testCalleeOutsideCompleteSet()
{
    checkValidationFails(resolvedCallee, ValidationFailureKind::UnlistedCallee, ""_s, { "validate"_s, "disable=flow:ExactFunctions"_s });
}

static void testUnreachableBlockIsEntered()
{
    checkValidationFails(unreachableBlock, ValidationFailureKind::UnreachableValue, "globalThis.enters = true;"_s);
    checkOutputOfProgramInShell(unreachableBlock, "went on\n"_s, { "validate"_s }, "globalThis.enters = false;"_s);
}

static void testFunctionThatIsNeverReached()
{
    checkValidationFails(functionThatIsNeverReached, ValidationFailureKind::UnreachableValue);
}

// Whoever enters the function from outside finds the entry that was left out, and never comes to the test of the first run: that one stands against a count of compiled calls that is wrong.
static void testFunctionThatRunsOnce()
{
    checkValidationFails(functionThatRunsOnce, ValidationFailureKind::UnreachableValue, "globalThis.sink = function () { print(this.k()); };"_s);

    UTF8CString numberedText = withNumbersOfModels(functionThatRunsOnce);
    checkOutputOfProgramInShell(ASCIILiteral::fromLiteralUnsafe(byteCast<char>(numberedText.data())), "ran\n"_s, { "validate"_s }, "globalThis.sink = function () { };"_s);
}

static void testReadReplacedAcrossCall()
{
    checkValidationFails(leakedObject, ValidationFailureKind::ReadsDiffer, "globalThis.sink = function () { this.k.x = 0.5; };"_s);
}

static void testReadReplacedByEffectsOfCall()
{
    checkValidationFails(effectsOfCall, ValidationFailureKind::ReadsDiffer, "globalThis.mutate = function () { this.a = 0.5; return 'text'; };"_s);
}

static void testSlotAccessBehindDroppedTest()
{
    checkValidationFails(guardedShape, ValidationFailureKind::ValueType, "globalThis.register = (readY, whole, withoutX) => { print(readY(whole)); print(readY({ y: 'other' })); };"_s, { "validate"_s, "drop-guard=TypeTest:0"_s });
}

static void testContentsOfArray()
{
    checkValidationFails(leakedArray, ValidationFailureKind::ContainerContents, "globalThis.after = () => { }; globalThis.sink = function () { this.k[0] = 'text'; };"_s);
}

// As leakedArray, with a loop that reads the first hundred elements.
static constexpr ASCIILiteral leakedArrayReadInLoop = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Holder" fields=[{key="k" declared={type=anything} flags=[IsRequired]}, {key="toString" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=Math
    v1 = PropGet v0 key="random"
    v2 = Call v1, v0 calleeKey="random"
    v3 = NewArray v2, v2 contents=anything holes=[]
    v4 = Builtin builtin=GlobalThis
    v5 = PropGet v4 key="sink" [isFreeName]
    v6 = NewObject -, v3, v5 layout=L0 fields=[0, 1]
    v7 = FinishObject v6
    v8 = PropGet v7 key="toLocaleString"
    v9 = Call v8, v7 calleeKey="toLocaleString"
    v10 = Integer integer=0
    Jump -> next b1(v10, v2)
  b1(v11, v12):
    LoopHint
    v14 = Integer integer=100
    v15 = Less v11, v14
    Branch v15 -> taken b2(), notTaken b3()
  b2():
    v16 = ElemGet v3, v11
    v17 = Mul v16, v12
    v18 = Integer integer=1
    v19 = Add v11, v18
    Jump -> next b1(v19, v17)
  b3():
    v20 = TypeOf v12
    v21 = PropGet v4 key="print" [isFreeName]
    v22 = Call v21, -, v20
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="toString"
    v2 = TypeOf v1
    v3 = PropGet v0 key="k"
    v4 = TypeOf v3
    Return
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

// A long array is sampled. The samples that a loop takes move along the array, so they find an element that the loop never reads.
static void testContentsOfLongArray()
{
    for (ASCIILiteral statements : {
        "globalThis.sink = function () { while (this.k.length < 1000) this.k.push(0.5); this.k[777] = 'text'; };"_s,
        "globalThis.sink = function () { while (this.k.length < 1000) this.k.push(0.5); this.k[961] = 'text'; };"_s,
        "globalThis.sink = function () { while (this.k.length < 700) this.k.push(0.5); this.k[101] = 'text'; };"_s,
    })
        checkValidationFails(leakedArrayReadInLoop, ValidationFailureKind::ContainerContents, statements);

    UTF8CString numberedText = withNumbersOfModels(leakedArrayReadInLoop);
    checkOutputOfProgramInShell(ASCIILiteral::fromLiteralUnsafe(byteCast<char>(numberedText.data())), "number\n"_s, { "validate"_s }, "globalThis.sink = function () { while (this.k.length < 1000) this.k.push(0.5); };"_s);
}

// As leakedArray, with integers: the array never escapes and nothing but integers is stored, so it keeps the storage for integers that it was made with.
static constexpr ASCIILiteral leakedArrayOfIntegers = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Holder" fields=[{key="k" declared={type=anything} flags=[IsRequired]}, {key="toString" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Integer integer=7
    v1 = NewArray v0, v0, v0 contents=anything holes=[]
    v2 = Builtin builtin=GlobalThis
    v3 = PropGet v2 key="sink" [isFreeName]
    v4 = NewObject -, v1, v3 layout=L0 fields=[0, 1]
    v5 = FinishObject v4
    v6 = PropGet v5 key="toLocaleString"
    v7 = Call v6, v5 calleeKey="toLocaleString"
    v8 = Integer integer=0
    v9 = ElemGet v1, v8
    v10 = TypeOf v9
    v11 = PropGet v2 key="print" [isFreeName]
    v12 = Call v11, -, v10
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="toString"
    v2 = TypeOf v1
    v3 = PropGet v0 key="k"
    v4 = TypeOf v3
    Return
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

// The storage is asked for before the elements are looked at.
static void testStorageOfArray()
{
    checkValidationFails(leakedArrayOfIntegers, ValidationFailureKind::ArrayStorageKind, "globalThis.sink = function () { this.k[0] = 0.5; };"_s);
    checkValidationFails(leakedArrayOfIntegers, ValidationFailureKind::ArrayStorageKind, "globalThis.sink = function () { this.k[0] = 'text'; };"_s);
    checkValidationFails(leakedArrayOfIntegers, ValidationFailureKind::ArrayStorageKind, "globalThis.sink = function () { this.k[100000] = 7; };"_s);
    checkValidationFails(leakedArrayOfIntegers, ValidationFailureKind::ContainerContents, "globalThis.sink = function () { this.k[0] = 8; };"_s);

    UTF8CString numberedText = withNumbersOfModels(leakedArrayOfIntegers);
    checkOutputOfProgramInShell(ASCIILiteral::fromLiteralUnsafe(byteCast<char>(numberedText.data())), "number\n"_s, { "validate"_s }, "globalThis.sink = function () { this.k[0] = 7; };"_s);
}

// The same list, asked for its first element by Array.prototype.at, which is written out for the kind of storage that the type of the list names.
static constexpr ASCIILiteral leakedArrayOfIntegersAskedByBuiltin = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Holder" fields=[{key="k" declared={type=anything} flags=[IsRequired]}, {key="toString" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Integer integer=7
    v1 = NewArray v0, v0, v0 contents=anything holes=[]
    v2 = Builtin builtin=GlobalThis
    v3 = PropGet v2 key="sink" [isFreeName]
    v4 = NewObject -, v1, v3 layout=L0 fields=[0, 1]
    v5 = FinishObject v4
    v6 = PropGet v5 key="toLocaleString"
    v7 = Call v6, v5 calleeKey="toLocaleString"
    v8 = Integer integer=0
    v9 = PropGet v1 key="at"
    v10 = Call v9, v1, v8 calleeKey="at"
    v11 = TypeOf v10
    v12 = PropGet v2 key="print" [isFreeName]
    v13 = Call v12, -, v11
    Return
}
function f1 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=$toLocaleString thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="toString"
    v2 = TypeOf v1
    v3 = PropGet v0 key="k"
    v4 = TypeOf v3
    Return
}
runtimefunction id=$toLocaleString function=f1
)IR"_s;

// The built-in makes no test of the kind of storage. A build that checks its proofs at run time makes it, and reports a list of another kind.
static void testStorageOfArrayAskedByBuiltin()
{
    for (ASCIILiteral statements : { "globalThis.sink = function () { this.k[0] = 0.5; };"_s, "globalThis.sink = function () { this.k[0] = 'text'; };"_s }) {
        checkValidationFails(leakedArrayOfIntegersAskedByBuiltin, ValidationFailureKind::ArrayStorageKind, statements, { "validate"_s, "heat=EverythingIsHot"_s });
        checkValidationFails(leakedArrayOfIntegersAskedByBuiltin, ValidationFailureKind::ValueType, statements, { "testing=CheckProofsAtRunTime"_s, "heat=EverythingIsHot"_s });
    }

    UTF8CString numberedText = withNumbersOfModels(leakedArrayOfIntegersAskedByBuiltin);
    checkOutputOfProgramInShell(ASCIILiteral::fromLiteralUnsafe(byteCast<char>(numberedText.data())), "number\n"_s, { "testing=CheckProofsAtRunTime"_s, "heat=EverythingIsHot"_s }, "globalThis.sink = function () { this.k[0] = 7; };"_s);
}

// Nothing in the program does any of this to the object, and the object never escapes: each is a mutation that flow calls impossible for its layout.
static void testEveryKindOfMutation()
{
    UTF8CString text = makeStringByReplacingAll(String(leakedObject), "    v15 = Call v14, -, v13\n"_s, "    v15 = Call v14, -, v13\n    v16 = PropGet v5 key=\"unread\"\n    v17 = Call v14, -, v16\n"_s).utf8();
    ASCIILiteral objectWithEveryFieldRead = ASCIILiteral::fromLiteralUnsafe(byteCast<char>(text.data()));
    for (ASCIILiteral statements : {
        "globalThis.sink = function () { Object.setPrototypeOf(this.k, { }); };"_s,
        "globalThis.sink = function () { Object.setPrototypeOf(this.k, null); };"_s,
        "globalThis.sink = function () { this.k.__proto__ = { }; };"_s,
        "globalThis.sink = function () { Object.defineProperty(this.k, 'x', { get() { return 1; } }); };"_s,
        "globalThis.sink = function () { Object.defineProperty(this.k, 'x', { writable: false }); };"_s,
        "globalThis.sink = function () { delete this.k.x; };"_s,
        "globalThis.sink = function () { this.k.x = 'text'; };"_s,
        // Every allocation lists 5 for the field, which is its type.
        "globalThis.sink = function () { this.k.unread = 6; };"_s,
        "globalThis.sink = function () { this.k.added = 1; };"_s,
        "globalThis.sink = function () { this.k[Symbol('s')] = 1; };"_s,
        "globalThis.sink = function () { this.k[0] = 1; };"_s,
        "globalThis.sink = function () { Object.freeze(this.k); };"_s,
        "globalThis.sink = function () { Object.preventExtensions(this.k); };"_s,
        "globalThis.sink = function () { globalThis.child = Object.create(this.k); };"_s,
    }) {
        checkValidationFails(objectWithEveryFieldRead, ValidationFailureKind::ImpossibleMutation, statements);
        checkValidationFails(objectWithEveryFieldRead, ValidationFailureKind::ImpossibleMutation, statements, { "testing=CheckProofsAtRunTime"_s });
    }
    // A store of a value of the field's type is found where the program reads the field again.
    checkValidationFails(objectWithEveryFieldRead, ValidationFailureKind::ReadsDiffer, "globalThis.sink = function () { this.k.x = 0.5; };"_s);
}

static void testContentsOfMap()
{
    checkValidationFails(leakedMap, ValidationFailureKind::ContainerContents, "globalThis.sink = function () { this.k.set('c', 'text'); };"_s);
    checkValidationFails(leakedMap, ValidationFailureKind::ContainerContents, "globalThis.sink = function () { this.k.set(5, 1.5); };"_s);
}

static void testPristineArray()
{
    checkValidationFails(leakedArray, ValidationFailureKind::ArrayNotPristine, "globalThis.after = () => { }; globalThis.sink = function () { Object.setPrototypeOf(this.k, { }); };"_s);
}

// So many names make a dictionary of the structure, and a dictionary carries no mark. What is proven of the names stays true while none of them shadows a member.
static void testPristineArrayWithManyNames()
{
    UTF8CString numberedText = withNumbersOfModels(leakedArray);
    ASCIILiteral program = ASCIILiteral::fromLiteralUnsafe(byteCast<char>(numberedText.data()));
    checkOutputOfProgramInShell(program, "number 2\n"_s, { "validate"_s }, "globalThis.after = () => { }; globalThis.sink = function () { for (let i = -300; i < 0; i++) this.k[i] = 1; };"_s);
    checkOutputOfProgramInShell(program, "number 2\n"_s, { "validate"_s }, "globalThis.after = () => { }; globalThis.sink = function () { for (let i = 0; i < 300; i++) this.k['name' + i] = 1; };"_s);
    checkOutputOfProgramInShell(program, "number 2\n"_s, { "validate"_s }, "globalThis.after = () => { }; globalThis.sink = function () { for (let i = -300; i < 0; i++) this.k[i] = 1; for (let i = -300; i < 0; i++) delete this.k[i]; };"_s);

    for (ASCIILiteral lastStatement : {
        "this.k.pop = 1;"_s,
        "this.k.hasOwnProperty = 1;"_s,
        "this.k.then = 1;"_s,
        "this.k.toJSON = 1;"_s,
        "this.k[Symbol.iterator] = 1;"_s,
        "this.k[Symbol.isConcatSpreadable] = 1;"_s,
        "this.k[Symbol('s')] = 1;"_s,
        "delete this.k[-1]; this.k[Symbol('s')] = 1;"_s,
        "Object.setPrototypeOf(this.k, { });"_s,
        "Object.setPrototypeOf(this.k, Object.prototype);"_s,
        "Object.setPrototypeOf(this.k, null);"_s,
    }) {
        UTF8CString statements = makeString("globalThis.after = () => { }; globalThis.sink = function () { for (let i = -300; i < 0; i++) this.k[i] = 1; "_s, lastStatement, " };"_s).utf8();
        checkValidationFails(leakedArray, ValidationFailureKind::ArrayNotPristine, ASCIILiteral::fromLiteralUnsafe(byteCast<char>(statements.data())));
    }

    checkValidationFails(leakedArray, ValidationFailureKind::ArrayElementsNotOrdinary, "globalThis.after = () => { }; globalThis.sink = function () { for (let i = -300; i < 0; i++) this.k[i] = 1; Object.defineProperty(this.k, '-5', { get() { return 1; } }); };"_s);
    checkValidationFails(leakedArray, ValidationFailureKind::ArrayElementsNotOrdinary, "globalThis.after = () => { }; globalThis.sink = function () { for (let i = -300; i < 0; i++) this.k[i] = 1; Object.defineProperty(this.k, '-1000', { get() { return 1; } }); };"_s);
}

// With a property of its own the names of the array are not proven and its storage is. A data property under "-1" costs the structure its mark and breaks nothing.
static void testArrayWithProvenStorageAlone()
{
    UTF8CString text = makeStringByReplacingAll(String(leakedArray), "    v19 = Call v18\n"_s, "    v19 = Call v18\n    PropSet v3, v2 key=\"label\" mode=Strict\n"_s).utf8();
    ASCIILiteral labeledArray = ASCIILiteral::fromLiteralUnsafe(byteCast<char>(text.data()));
    checkValidationFails(labeledArray, ValidationFailureKind::ArrayElementsNotOrdinary, "globalThis.after = () => { }; globalThis.sink = function () { Object.setPrototypeOf(this.k, { }); };"_s);
    checkValidationFails(labeledArray, ValidationFailureKind::ArrayElementsNotOrdinary, "globalThis.after = () => { }; globalThis.sink = function () { Object.defineProperty(this.k, '-1', { get() { return 1; } }); };"_s);

    UTF8CString numberedText = withNumbersOfModels(labeledArray);
    checkOutputOfProgramInShell(ASCIILiteral::fromLiteralUnsafe(byteCast<char>(numberedText.data())), "number 2\n"_s, { "validate"_s }, "globalThis.after = () => { }; globalThis.sink = function () { this.k[-1] = 1; };"_s);
}

// Nothing in the program reads the field "unread", so no object has it.
static void testRemovedFieldIsRead()
{
    checkValidationFails(leakedObject, ValidationFailureKind::RemovedFieldNamed, "globalThis.sink = function () { print(this.k.unread); };"_s);
}

// A heap snapshot looks for `constructor` on every object, to name it. Nothing else may name the removed field.
static void testHeapSnapshotLooksForRemovedConstructor()
{
    UTF8CString text = makeStringByReplacingAll(String(leakedObject), "key=\"unread\""_s, "key=\"constructor\""_s).utf8();
    ASCIILiteral objectWithoutConstructor = ASCIILiteral::fromLiteralUnsafe(byteCast<char>(text.data()));
    checkValidationFails(objectWithoutConstructor, ValidationFailureKind::RemovedFieldNamed, "globalThis.sink = function () { print(this.k.constructor); };"_s);
    checkValidationFails(objectWithoutConstructor, ValidationFailureKind::RemovedFieldNamed, "globalThis.sink = function () { generateHeapSnapshot(); print(this.k.constructor); };"_s);

    UTF8CString numberedText = withNumbersOfModels(objectWithoutConstructor);
    checkOutputOfProgramInShell(ASCIILiteral::fromLiteralUnsafe(byteCast<char>(numberedText.data())), "true\n"_s, { "validate"_s }, "globalThis.sink = function () { generateHeapSnapshot(); };"_s);
}

static void testKeysOfObjectWithRemovedField()
{
    checkValidationFails(leakedObject, ValidationFailureKind::RemovedFieldNamed, "globalThis.sink = function () { print(Object.keys(this.k)); };"_s);
}

// The program reads every field, so the object keeps them all. Nothing in the program lists its keys, so the object keeps no order of them.
static void testKeysOfObjectWithoutKeyOrder()
{
    UTF8CString text = makeStringByReplacingAll(String(leakedObject), "    v15 = Call v14, -, v13\n"_s, "    v15 = Call v14, -, v13\n    v16 = PropGet v5 key=\"unread\"\n    v17 = Call v14, -, v16\n"_s).utf8();
    ASCIILiteral objectWithEveryFieldRead = ASCIILiteral::fromLiteralUnsafe(byteCast<char>(text.data()));
    checkValidationFails(objectWithEveryFieldRead, ValidationFailureKind::KeysListed, "globalThis.sink = function () { print(Object.keys(this.k)); };"_s);
    checkValidationFails(objectWithEveryFieldRead, ValidationFailureKind::KeysListed, "globalThis.sink = function () { print(JSON.stringify(this.k)); };"_s);
    checkValidationFails(objectWithEveryFieldRead, ValidationFailureKind::KeysListed, "globalThis.sink = function () { print(Object.keys({ ...this.k })); };"_s);
    checkValidationFails(objectWithEveryFieldRead, ValidationFailureKind::KeysListed, "globalThis.sink = function () { for (let key in this.k) print(key); };"_s);

    UTF8CString numberedText = withNumbersOfModels(objectWithEveryFieldRead);
    checkOutputOfProgramInShell(ASCIILiteral::fromLiteralUnsafe(byteCast<char>(numberedText.data())), "5\ntrue\n5\n"_s, { "validate"_s }, "globalThis.sink = function () { print(this.k.unread); };"_s);
}

// The program deletes one field, so an object of the layout may lose a field. It is proven of the other field alone that nothing deletes, redefines or retypes it.
static void testStableFieldOfLayoutThatLosesAnotherField()
{
    UTF8CString text = makeStringByReplacingAll(String(leakedObject), "    v15 = Call v14, -, v13\n"_s, "    v15 = Call v14, -, v13\n    v16 = PropGet v5 key=\"unread\"\n    v17 = Call v14, -, v16\n    v18 = PropDelete v5 key=\"unread\" mode=Sloppy\n"_s).utf8();
    ASCIILiteral objectThatLosesField = ASCIILiteral::fromLiteralUnsafe(byteCast<char>(text.data()));
    checkValidationFails(objectThatLosesField, ValidationFailureKind::ImpossibleMutation, "globalThis.sink = function () { delete this.k.x; };"_s);
    checkValidationFails(objectThatLosesField, ValidationFailureKind::ImpossibleMutation, "globalThis.sink = function () { delete this.k.x; };"_s, { "testing=CheckProofsAtRunTime"_s });

    UTF8CString numberedText = withNumbersOfModels(objectThatLosesField);
    checkOutputOfProgramInShell(ASCIILiteral::fromLiteralUnsafe(byteCast<char>(numberedText.data())), "true\n5\n"_s, { "validate"_s }, "globalThis.sink = function () { };"_s);
}

// The program gives the object the property "p" and no other, so a lookup of any other name goes on to the prototypes without a look at the properties beside the fields.
static void testNameOutsideCompleteListOfAddedNames()
{
    UTF8CString text = makeStringByReplacingAll(String(leakedObject), "    v15 = Call v14, -, v13\n"_s, "    v15 = Call v14, -, v13\n    PropSet v5, v2 key=\"p\" mode=Strict\n"_s).utf8();
    ASCIILiteral objectWithAddedProperty = ASCIILiteral::fromLiteralUnsafe(byteCast<char>(text.data()));
    OptionWords optionWords { "validate"_s, "disable=flow:AppendedFields"_s };
    checkValidationFails(objectWithAddedProperty, ValidationFailureKind::ImpossibleMutation, "globalThis.sink = function () { this.k.toString = () => 'own'; };"_s, optionWords);
    checkValidationFails(objectWithAddedProperty, ValidationFailureKind::ImpossibleMutation, "globalThis.sink = function () { this.k.toString = () => 'own'; };"_s, { "testing=CheckProofsAtRunTime"_s, "disable=flow:AppendedFields"_s });

    UTF8CString numberedText = withNumbersOfModels(objectWithAddedProperty);
    checkOutputOfProgramInShell(ASCIILiteral::fromLiteralUnsafe(byteCast<char>(numberedText.data())), "true\n"_s, optionWords, "globalThis.sink = function () { this.k.p = 1; };"_s);
}

static void testMembersOfClassChange()
{
    checkValidationFails(leakedInstance, ValidationFailureKind::LookupDiffers, "globalThis.sink = function () { Object.getPrototypeOf(this.k).method = () => 'replaced'; };"_s);
    checkValidationFails(leakedInstance, ValidationFailureKind::ImpossibleMutation, "globalThis.sink = function () { Object.getPrototypeOf(this.k).other = 5; };"_s);
    checkValidationFails(leakedInstance, ValidationFailureKind::ImpossibleMutation, "globalThis.sink = function () { Object.getPrototypeOf(this.k).toString = () => 'of the class'; };"_s);
    checkValidationFails(leakedInstance, ValidationFailureKind::ImpossibleMutation, "globalThis.sink = function () { Object.getPrototypeOf(this.k).other = 5; };"_s, { "testing=CheckProofsAtRunTime"_s });
    checkValidationFails(leakedInstance, ValidationFailureKind::ImpossibleMutation, "globalThis.sink = function () { Object.getPrototypeOf(this.k).toString = () => 'of the class'; };"_s, { "testing=CheckProofsAtRunTime"_s });

    UTF8CString numberedText = withNumbersOfModels(leakedInstance);
    checkOutputOfProgramInShell(ASCIILiteral::fromLiteralUnsafe(byteCast<char>(numberedText.data())), "as defined\nundefined\n[object Object]\n"_s, { "validate"_s }, "globalThis.sink = function () { };"_s);
}

static void testMemberOfClosureChanges()
{
    checkValidationFails(leakedClosure, ValidationFailureKind::AssumedType, "globalThis.sink = function () { this.k.call = () => 'replaced'; };"_s);
    checkValidationFails(leakedClosure, ValidationFailureKind::AssumedType, "globalThis.sink = function () { Object.setPrototypeOf(this.k, { call() { return 'inherited'; } }); };"_s);

    UTF8CString numberedText = withNumbersOfModels(leakedClosure);
    checkOutputOfProgramInShell(ASCIILiteral::fromLiteralUnsafe(byteCast<char>(numberedText.data())), "as created\n"_s, { "validate"_s }, "globalThis.sink = function () { };"_s);
}

// Nothing in the program names RegExp.
static void testLegacyRegExpStaticIsRead()
{
    checkValidationFails(leakedObject, ValidationFailureKind::LegacyRegExpStaticRead, "globalThis.sink = function () { print(RegExp.$1); };"_s);
    checkValidationFails(leakedObject, ValidationFailureKind::LegacyRegExpStaticRead, "globalThis.sink = function () { print(RegExp.input); };"_s);
}

// A read of a slot asks for no guard, and the inherited member is looked for again behind the call: nothing is kept across it, so what the function of the shell changes is seen and no check has anything to find.
static void testNothingIsKeptAcrossCall()
{
    UTF8CString guardText = withNumbersOfModels(guardKeptAcrossCall);
    UTF8CString prototypeGuardText = withNumbersOfModels(prototypeGuardKeptAcrossCall);
    ASCIILiteral guard = ASCIILiteral::fromLiteralUnsafe(byteCast<char>(guardText.data()));
    ASCIILiteral prototypeGuard = ASCIILiteral::fromLiteralUnsafe(byteCast<char>(prototypeGuardText.data()));
    checkOutputOfProgramInShell(guard, "true\n"_s, { "validate"_s }, "globalThis.mutate = function () { Object.defineProperty(this, 0, { get() { return 1; } }); return 'text'; };"_s);
    checkOutputOfProgramInShell(prototypeGuard, "false\n"_s, { "validate"_s }, "globalThis.mutate = function () { Object.setPrototypeOf(this, { toLocaleString() { return 'other'; } }); return 'text'; };"_s);
    for (ASCIILiteral program : { guard, prototypeGuard })
        checkOutputOfProgramInShell(program, "true\n"_s, { "validate"_s }, "globalThis.mutate = function () { return 'text'; };"_s);

    // Where the program may run in several instances, a test of the instance is among the tests that each use repeats. It takes the receiver as the type test narrowed it.
    checkOutputOfProgramInShell(guardedShape, "1.5\n1.5\n"_s, { "validate"_s, "disable=flow:ReachedFunctions"_s }, "globalThis.register = (readY, whole, withoutX) => { print(readY(whole)); print(readY(whole)); };"_s);
}

// The walks that do not ask the object for the names of its properties.
static void testWalksOverObjectWithRemovedField()
{
    checkValidationFails(leakedObject, ValidationFailureKind::RemovedFieldNamed, "globalThis.sink = function () { print(JSON.stringify(this.k)); };"_s);
    checkValidationFails(leakedObject, ValidationFailureKind::RemovedFieldNamed, "globalThis.sink = function () { print(Object.keys({ ...this.k })); };"_s);
    checkValidationFails(leakedObject, ValidationFailureKind::RemovedFieldNamed, "globalThis.sink = function () { print(Object.values(this.k)); };"_s);
}

static void testTrueProofsPass()
{
    UTF8CString objectText = withNumbersOfModels(leakedObject);
    UTF8CString arrayText = withNumbersOfModels(leakedArray);
    ASCIILiteral object = ASCIILiteral::fromLiteralUnsafe(byteCast<char>(objectText.data()));
    checkOutputOfProgramInShell(object, "true\n"_s, { "validate"_s }, "globalThis.sink = function () { };"_s);
    checkOutputOfProgramInShell(object, "undefined\ntrue\n"_s, { "validate"_s }, "globalThis.sink = function () { print(this.k.neverDeclared); };"_s);
    checkOutputOfProgramInShell(ASCIILiteral::fromLiteralUnsafe(byteCast<char>(arrayText.data())), "number 2\n"_s, { "validate"_s }, "globalThis.after = () => { }; globalThis.sink = function () { this.k[0] = 0.5; };"_s);
}

std::span<const NativeTest> proofValidationTests()
{
    static constexpr NativeTest tests[] = {
        { "proof-validation/folded-constant"_s, testFoldedConstant },
        { "proof-validation/folded-type-test"_s, testFoldedTypeTest },
        { "proof-validation/number-in-arithmetic"_s, testNumberInArithmetic },
        { "proof-validation/layout-at-slot-read"_s, testLayoutAtSlotRead },
        { "proof-validation/resolved-callee"_s, testResolvedCallee },
        { "proof-validation/callee-outside-complete-set"_s, testCalleeOutsideCompleteSet },
        { "proof-validation/unreachable-block-is-entered"_s, testUnreachableBlockIsEntered },
        { "proof-validation/function-that-is-never-reached"_s, testFunctionThatIsNeverReached },
        { "proof-validation/function-that-runs-once-runs-again"_s, testFunctionThatRunsOnce },
        { "proof-validation/read-replaced-across-call"_s, testReadReplacedAcrossCall },
        { "proof-validation/read-replaced-by-effects-of-call"_s, testReadReplacedByEffectsOfCall },
        { "proof-validation/slot-access-behind-dropped-test"_s, testSlotAccessBehindDroppedTest },
        { "proof-validation/contents-of-array"_s, testContentsOfArray },
        { "proof-validation/contents-of-long-array"_s, testContentsOfLongArray },
        { "proof-validation/contents-of-map"_s, testContentsOfMap },
        { "proof-validation/storage-of-array"_s, testStorageOfArray },
        { "proof-validation/storage-of-array-asked-by-built-in"_s, testStorageOfArrayAskedByBuiltin },
        { "proof-validation/every-kind-of-mutation"_s, testEveryKindOfMutation },
        { "proof-validation/pristine-array"_s, testPristineArray },
        { "proof-validation/pristine-array-with-many-names"_s, testPristineArrayWithManyNames },
        { "proof-validation/array-with-proven-storage-alone"_s, testArrayWithProvenStorageAlone },
        { "proof-validation/removed-field-is-read"_s, testRemovedFieldIsRead },
        { "proof-validation/heap-snapshot-looks-for-removed-constructor"_s, testHeapSnapshotLooksForRemovedConstructor },
        { "proof-validation/keys-of-object-with-removed-field"_s, testKeysOfObjectWithRemovedField },
        { "proof-validation/walks-over-object-with-removed-field"_s, testWalksOverObjectWithRemovedField },
        { "proof-validation/keys-of-object-without-key-order"_s, testKeysOfObjectWithoutKeyOrder },
        { "proof-validation/stable-field-of-layout-that-loses-another-field"_s, testStableFieldOfLayoutThatLosesAnotherField },
        { "proof-validation/name-outside-complete-list-of-added-names"_s, testNameOutsideCompleteListOfAddedNames },
        { "proof-validation/members-of-class-change"_s, testMembersOfClassChange },
        { "proof-validation/member-of-closure-changes"_s, testMemberOfClosureChanges },
        { "proof-validation/legacy-regexp-static-is-read"_s, testLegacyRegExpStaticIsRead },
        { "proof-validation/nothing-is-kept-across-call"_s, testNothingIsKeptAcrossCall },
        { "proof-validation/true-proofs-pass"_s, testTrueProofsPass },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
