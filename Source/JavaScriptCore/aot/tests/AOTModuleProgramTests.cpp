/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompile.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRParser.h"
#include "AOTNativeTest.h"
#include "AOTProgramFile.h"
#include "AOTShellProcess.h"
#include "AOTTemporaryFile.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <initializer_list>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

using OptionWords = std::initializer_list<ASCIILiteral>;

static constexpr ASCIILiteral mainThatRequires = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m1 kind=Require completes=true}, {target=m2 kind=Require completes=true}, {target=m3 kind=Require completes=true}]
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    EvaluateModule module=m1 referrer=m0 [isBare]
    v3 = GetModuleVariable variable=g0
    v4 = Call v3
    v5 = String string="module.exports = function, called"
    v6 = Call v1, -, v5, v4
    v7 = PropGet v3 key="tag"
    v8 = String string="a property of that function"
    v9 = Call v1, -, v8, v7
    EvaluateModule module=m2 referrer=m0 [isBare]
    v11 = GetModuleVariable variable=g1
    v12 = PropGet v11 key="a"
    v13 = String string="exports.a"
    v14 = Call v1, -, v13, v12
    EvaluateModule module=m3 referrer=m0 [isBare]
    v16 = GetModuleVariable variable=g2
    v17 = PropGet v16 key="sum"
    v18 = String string="exports.a of the other module + 1"
    v19 = Call v1, -, v18, v17
    EvaluateModule module=m3 referrer=m0 [isBare]
    v21 = GetModuleVariable variable=g2
    v22 = StrictEq v21, v16
    v23 = String string="a second require answers the same object"
    v24 = Call v1, -, v23, v22
    Return
}
)IR"_s;

// ES modules import each file through a module of its own, which evaluates the file. The names are found in what the file exports then.
static constexpr ASCIILiteral mainThatImports = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m4 completes=true}, {target=m5 completes=true}, {target=m6 completes=true}]
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = GetModuleVariable variable=g3
    v3 = Call v2
    v4 = String string="the default import of module.exports = function, called"
    v5 = Call v1, -, v4, v3
    v6 = GetModuleVariable variable=g4
    v7 = String string="a property of that function, imported by name"
    v8 = Call v1, -, v7, v6
    v9 = GetModuleVariable variable=g5
    v10 = String string="exports.a, imported by name"
    v11 = Call v1, -, v10, v9
    v12 = GetModuleVariable variable=g6
    v13 = PropGet v12 key="a"
    v14 = String string="the default import of exports, .a"
    v15 = Call v1, -, v14, v13
    v16 = GetModuleVariable variable=g7
    v17 = String string="exports.a of the other module + 1, imported by name"
    v18 = Call v1, -, v17, v16
    Return
}
)IR"_s;

// Behind commonJSModules: the records of a kind stand in the order of their numbers.
static constexpr ASCIILiteral facesOfCommonJSModules = R"IR(module m4 path="#4" displayPath="function.cjs" exportRule=CommonJs body=f5 requests=[{target=m1 kind=Require completes=true}] syntheticExports=[{name="default" variable=g3}, {name="tag" variable=g4}]
module m5 path="#5" displayPath="filled.cjs" exportRule=CommonJs body=f6 requests=[{target=m2 kind=Require completes=true}] syntheticExports=[{name="a" variable=g5}, {name="default" variable=g6}]
module m6 path="#6" displayPath="chained.cjs" exportRule=CommonJs body=f7 requests=[{target=m3 kind=Require completes=true}] syntheticExports=[{name="sum" variable=g7}]
variable g3 name="default" module=m4 declared={type=anything}
variable g4 name="tag" module=m4 declared={type=anything}
variable g5 name="a" module=m5 declared={type=anything}
variable g6 name="default" module=m5 declared={type=anything}
variable g7 name="sum" module=m6 declared={type=anything}
function f5 name="faceOfFunctionModule" module=m1 kind=Module results=[{type=anything}] {
    EvaluateModule module=m1 referrer=m4 [isBare]
    Return
}
function f6 name="faceOfFilledModule" module=m2 kind=Module results=[{type=anything}] {
    EvaluateModule module=m2 referrer=m5 [isBare]
    Return
}
function f7 name="faceOfChainedModule" module=m3 kind=Module results=[{type=anything}] {
    EvaluateModule module=m3 referrer=m6 [isBare]
    Return
}
)IR"_s;

static constexpr ASCIILiteral commonJSModules = R"IR(module m1 path="/function.cjs" displayPath="function.cjs" kind=CommonJs flags=[KeepsError] exportRule=CommonJs requireResult=ModuleExports body=f1 exportsVariable=g0
module m2 path="/filled.cjs" displayPath="filled.cjs" kind=CommonJs flags=[KeepsError] exportRule=CommonJs requireResult=ModuleExports body=f2 exportsVariable=g1
module m3 path="/chained.cjs" displayPath="chained.cjs" kind=CommonJs flags=[KeepsError] exportRule=CommonJs requireResult=ModuleExports body=f3 exportsVariable=g2 requests=[{target=m2 kind=Require completes=true}, {target=m1 kind=Require completes=true}]
variable g0 name="exports" module=m1 declared={type=anything} initialization=Undefined
variable g1 name="exports" module=m2 declared={type=anything} initialization=Undefined
variable g2 name="exports" module=m3 declared={type=anything} initialization=Undefined
function f1 name="functionModule" module=m1 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = NewClosure function=f4
    v1 = String string="tagged"
    PropSet v0, v1 key="tag"
    PutModuleVariable v0 variable=g0
    Return
}
function f2 name="filledModule" module=m2 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = NewDictionary
    PutModuleVariable v0 variable=g1
    v2 = Integer integer=41
    PropSet v0, v2 key="a"
    Return
}
function f3 name="chainedModule" module=m3 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = NewDictionary
    PutModuleVariable v2 variable=g2
    EvaluateModule module=m2 referrer=m3 [isBare]
    v5 = GetModuleVariable variable=g1
    v6 = PropGet v5 key="a"
    v7 = Integer integer=1
    v8 = Add v6, v7
    PropSet v2, v8 key="sum"
    EvaluateModule module=m1 referrer=m3 [isBare]
    v11 = GetModuleVariable variable=g0
    v12 = Call v11
    v13 = String string="the function of the other module, called"
    v14 = Call v1, -, v13, v12
    v15 = ModuleBuiltin builtin=ModuleObject module=m3
    v16 = PropGet v15 key="exports"
    v17 = StrictEq v16, v2
    v18 = String string="module.exports === exports"
    v19 = Call v1, -, v18, v17
    v20 = PropGet v15 key="id"
    v21 = String string="module.id"
    v22 = Call v1, -, v21, v20
    v23 = ModuleBuiltin builtin=ModuleRequire module=m3
    v24 = TypeOf v23
    v25 = String string="typeof require"
    v26 = Call v1, -, v25, v24
    v27 = ModuleBuiltin builtin=ModuleObject module=m3
    v28 = StrictEq v27, v15
    v29 = String string="module is one object"
    v30 = Call v1, -, v29, v28
    v31 = NewDictionary
    PropSet v15, v31 key="exports"
    v33 = GetModuleVariable variable=g2
    v34 = StrictEq v33, v31
    v35 = String string="a store to module.exports is a store to the variable"
    v36 = Call v1, -, v35, v34
    PutModuleVariable v2 variable=g2
    Return
}
function f4 name="exported" module=m1 results=[{type=anything}] {
    v0 = String string="called"
    Return v0
}
)IR"_s;

// The exports of the required module are variables and nothing else.
static constexpr ASCIILiteral commonJSEntryPoint = R"IR(module m0 path="/main.cjs" displayPath="main.cjs" kind=CommonJs flags=[KeepsError] exportRule=CommonJs requireResult=ModuleExports body=f0 exportsVariable=g0 requests=[{target=m1 kind=Require completes=true}]
module m1 path="/library.cjs" displayPath="library.cjs" kind=CommonJs flags=[KeepsError] exportRule=CommonJs requireResult=ModuleExports body=f1
entry module=m0
variable g0 name="exports" module=m0 declared={type=anything} initialization=Undefined
variable g1 name="a" module=m1 declared={type=anything} initialization=Undefined
function f0 name="main" kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = NewDictionary
    PutModuleVariable v2 variable=g0
    EvaluateModule module=m1 referrer=m0 [isBare]
    v5 = GetModuleVariable variable=g1
    v6 = String string="exports.a of the required module"
    v7 = Call v1, -, v6, v5
    v8 = ModuleBuiltin builtin=ModuleObject module=m0
    v9 = PropGet v8 key="id"
    v10 = String string="module.id"
    v11 = Call v1, -, v10, v9
    v12 = PropGet v8 key="exports"
    v13 = StrictEq v12, v2
    v14 = String string="module.exports === exports"
    v15 = Call v1, -, v14, v13
    Return
}
function f1 name="library" module=m1 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Integer integer=41
    PutModuleVariable v0 variable=g1
    Return
}
)IR"_s;

static constexpr ASCIILiteral recordsOfRequire = R"IR(module m0 path="/main.cjs" displayPath="main.cjs" kind=CommonJs flags=[KeepsError, NamesWayToModuleObjects] exportRule=CommonJs requireResult=ModuleExports body=f0 exportsVariable=g0 requests=[{target=m1 kind=Require completes=true}]
module m1 path="/child.cjs" displayPath="child.cjs" kind=CommonJs flags=[KeepsError] exportRule=CommonJs requireResult=ModuleExports body=f1 exportsVariable=g1
entry module=m0
variable g0 name="exports" module=m0 declared={type=anything} initialization=Undefined
variable g1 name="exports" module=m1 declared={type=anything} initialization=Undefined
function f0 name="main" kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = ModuleBuiltin builtin=ModuleObject module=m0
    v3 = PropGet v2 key="children"
    v4 = PropGet v3 key="length"
    v5 = String string="children before the first require"
    v6 = Call v1, -, v5, v4
    RecordRequire module=m1 referrer=m0
    EvaluateModule module=m1 referrer=m0 [isBare]
    RecordRequire module=m1 referrer=m0
    EvaluateModule module=m1 referrer=m0 [isBare]
    v11 = PropGet v2 key="children"
    v12 = PropGet v11 key="length"
    v13 = String string="children behind two requires of one module"
    v14 = Call v1, -, v13, v12
    v15 = Integer integer=0
    v16 = ElemGet v11, v15
    v17 = PropGet v16 key="parent"
    v18 = StrictEq v17, v2
    v19 = String string="the parent of the child is the module"
    v20 = Call v1, -, v19, v18
    v21 = PropGet v16 key="exports"
    v22 = GetModuleVariable variable=g1
    v23 = StrictEq v21, v22
    v24 = String string="the child shows what it exports"
    v25 = Call v1, -, v24, v23
    Return
}
function f1 name="child" module=m1 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = NewDictionary
    PutModuleVariable v2 variable=g1
    v4 = ModuleBuiltin builtin=ModuleObject module=m1
    v5 = PropGet v4 key="parent"
    v6 = PropGet v5 key="id"
    v7 = String string="the id of the parent, read while the child runs"
    v8 = Call v1, -, v7, v6
    Return
}
)IR"_s;

static constexpr ASCIILiteral importMetaKept = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m1 completes=true}]
module m1 path="/other.js" displayPath="other.js" body=f2 exports=[{name="otherMeta" variable=g1}]
entry module=m0
variable g0 name="meta" module=m0 declared={type=anything} initialization=Undefined
variable g1 name="otherMeta" module=m1 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = ModuleBuiltin builtin=ImportMeta module=m0
    PutModuleVariable v2 variable=g0
    v4 = NewClosure function=f1
    v5 = PropGet v0 key="register" [isFreeName]
    v6 = Call v5, -, v4
    v7 = ModuleBuiltin builtin=ImportMeta module=m0
    v8 = StrictEq v2, v7
    v9 = String string="import.meta === import.meta"
    v10 = Call v1, -, v9, v8
    v11 = GetModuleVariable variable=g0
    v12 = StrictEq v11, v7
    v13 = String string="the variable === import.meta"
    v14 = Call v1, -, v13, v12
    v15 = TypeOf v11
    v16 = String string="typeof the variable"
    v17 = Call v1, -, v16, v15
    v18 = GetModuleVariable variable=g1
    v19 = StrictEq v11, v18
    v20 = String string="the variable === import.meta of another module"
    v21 = Call v1, -, v20, v19
    v22 = TypeOf v18
    v23 = String string="typeof that one"
    v24 = Call v1, -, v23, v22
    Return
}
function f1 name="comparesMeta" results=[{type=anything}] {
    v0 = GetModuleVariable variable=g0
    v1 = ModuleBuiltin builtin=ImportMeta module=m0
    v2 = StrictEq v0, v1
    Return v2
}
function f2 name="other" module=m1 kind=Module results=[{type=anything}] {
    v0 = ModuleBuiltin builtin=ImportMeta module=m1
    PutModuleVariable v0 variable=g1
    Return
}
)IR"_s;

static constexpr ASCIILiteral importMetaPassedOn = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = ModuleBuiltin builtin=ImportMeta module=m0
    v3 = PropGet v0 key="id" [isFreeName]
    v4 = Call v3, -, v2
    v5 = StrictEq v4, v2
    v6 = String string="id(import.meta) === import.meta"
    v7 = Call v1, -, v6, v5
    Return
}
)IR"_s;

static constexpr ASCIILiteral moduleBuiltinsInEveryPosition = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m1 kind=Require completes=true}]
module m1 path="/library.cjs" displayPath="library.cjs" kind=CommonJs flags=[KeepsError] exportRule=CommonJs requireResult=ModuleExports body=f1 exportsVariable=g4
entry module=m0
layout L0 name="Holder" fields=[{key="meta" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Wrapper" fields=[{key="exports" declared={type=anything} flags=[IsRequired]}, {key="require" declared={type=anything} flags=[IsRequired]}, {key="module" declared={type=anything} flags=[IsRequired]}]
variable g0 name="meta" module=m0 declared={type=anything} initialization=Undefined
variable g1 name="holder" module=m0 declared={type=anything} initialization=Undefined
variable g2 name="callback" module=m0 declared={type=anything} initialization=Undefined
variable g3 name="received" module=m0 declared={type=anything} initialization=Undefined
variable g4 name="exports" module=m1 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = ModuleBuiltin builtin=ImportMeta module=m0
    PutModuleVariable v2 variable=g0
    v4 = NewObject -, v2 layout=L0 fields=[0]
    v5 = FinishObject v4
    PutModuleVariable v5 variable=g1
    v7 = PropGet v0 key="input" [isFreeName]
    v8 = ToBoolean v7
    Branch v8 -> taken b1(), notTaken b2()
  b1():
    v9 = NewClosure function=f2
    PutModuleVariable v9 variable=g2
    Jump -> next b3()
  b2():
    v11 = NewClosure function=f3
    PutModuleVariable v11 variable=g2
    Jump -> next b3()
  b3():
    v13 = GetModuleVariable variable=g2
    v14 = Call v13, -, v2
    v15 = String string="which of two callbacks was called by value"
    v16 = Call v1, -, v15, v14
    v17 = GetModuleVariable variable=g3
    v18 = StrictEq v17, v2
    v19 = String string="its parameter was import.meta"
    v20 = Call v1, -, v19, v18
    v21 = GetModuleVariable variable=g1
    v22 = PropGet v21 key="meta"
    v23 = StrictEq v22, v2
    v24 = String string="the field holds import.meta"
    v25 = Call v1, -, v24, v23
    v26 = NewClosure function=f4
    v27 = PropGet v0 key="register" [isFreeName]
    v28 = Call v27, -, v26
    EvaluateModule module=m1 referrer=m0 [isBare]
    v30 = GetModuleVariable variable=g4
    v31 = Call v30
    v32 = String string="typeof exports, require, module, module.exports, read from fields"
    v33 = Call v1, -, v32, v31
    Return
}
function f1 name="library" module=m1 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = NewDictionary
    PutModuleVariable v0 variable=g4
    v2 = ModuleBuiltin builtin=ModuleRequire module=m1
    v3 = ModuleBuiltin builtin=ModuleObject module=m1
    v4 = NewObject -, v0, v2, v3 layout=L1 fields=[0, 1, 2]
    v5 = FinishObject v4
    v6 = NewClosure v5 function=f5
    PutModuleVariable v6 variable=g4
    Return
}
function f2 name="first" parameters=[{type=anything}] results=[{type=anything}] {
    PutModuleVariable v0 variable=g3
    v2 = Integer integer=1
    Return v2
}
function f3 name="second" parameters=[{type=anything}] results=[{type=anything}] {
    PutModuleVariable v0 variable=g3
    v2 = Integer integer=2
    Return v2
}
function f4 name="comparesField" results=[{type=anything}] {
    v0 = GetModuleVariable variable=g1
    v1 = PropGet v0 key="meta"
    v2 = ModuleBuiltin builtin=ImportMeta module=m0
    v3 = StrictEq v1, v2
    Return v3
}
function f5 name="describesWrapper" module=m1 captures=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="exports"
    v2 = PropGet v0 key="require"
    v3 = PropGet v0 key="module"
    v4 = TypeOf v1
    v5 = TypeOf v2
    v6 = TypeOf v3
    v7 = String string=" "
    v8 = Add v4, v7
    v9 = Add v8, v5
    v10 = Add v9, v7
    v11 = Add v10, v6
    v12 = Add v11, v7
    v13 = PropGet v3 key="exports"
    v14 = TypeOf v13
    v15 = Add v12, v14
    Return v15
}
)IR"_s;

static constexpr ASCIILiteral dynamicImportsFromScript = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/first.js" displayPath="first.js" body=f1 requests=[{target=m2 completes=true}]
module m2 path="/below-first.js" displayPath="below-first.js" body=f2
module m3 path="/second.js" displayPath="second.js" body=f3 requests=[{target=m6 completes=true}]
module m4 path="/file.cjs" displayPath="file.cjs" kind=CommonJs flags=[KeepsError] exportRule=CommonJs requireResult=ModuleExports body=f4 exportsVariable=g0
module m5 path="/last.cjs" displayPath="last.cjs" kind=CommonJs flags=[KeepsError] exportRule=CommonJs requireResult=ModuleExports body=f5 exportsVariable=g1
module m6 path="#6" displayPath="file.cjs" exportRule=CommonJs body=f6 requests=[{target=m4 kind=Require completes=true}] syntheticExports=[{name="default" variable=g2}]
module m7 path="#7" displayPath="last.cjs" exportRule=CommonJs body=f7 requests=[{target=m5 kind=Require completes=true}] syntheticExports=[{name="default" variable=g3}]
entry module=m0
variable g0 name="exports" module=m4 declared={type=anything} initialization=Undefined
variable g1 name="exports" module=m5 declared={type=anything} initialization=Undefined
variable g2 name="default" module=m6 declared={type=anything}
variable g3 name="default" module=m7 declared={type=anything}
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f8
    v1 = NewClosure function=f9
    v2 = NewClosure function=f10
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="registerImports" [isFreeName]
    v5 = Call v4, -, v0, v1, v2
    Return
}
function f1 name="first" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="first.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f2 name="belowFirst" module=m2 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="below-first.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f3 name="second" module=m3 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="second.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f4 name="file" module=m4 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="file.cjs"
    v4 = Call v1, -, v2, v3
    v5 = NewDictionary
    PutModuleVariable v5 variable=g0
    Return
}
function f5 name="last" module=m5 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="last.cjs"
    v4 = Call v1, -, v2, v3
    v5 = NewDictionary
    PutModuleVariable v5 variable=g1
    Return
}
function f6 name="faceOfFile" module=m4 kind=Module results=[{type=anything}] {
    EvaluateModule module=m4 referrer=m6 [isBare]
    Return
}
function f7 name="faceOfLast" module=m5 kind=Module results=[{type=anything}] {
    EvaluateModule module=m5 referrer=m7 [isBare]
    Return
}
function f8 name="importFirst" results=[{type=anything}] {
    v0 = ImportModule module=m1 referrer=m0
    Return v0
}
function f9 name="importSecond" results=[{type=anything}] {
    v0 = ImportModule module=m3 referrer=m0
    Return v0
}
function f10 name="importLast" results=[{type=anything}] {
    v0 = ImportModule module=m7 referrer=m0
    Return v0
}
)IR"_s;

// main.js requires running.js, which imports imported.js, so its start evaluates it and the table has its status. It requires imports-back.js, which calls import() on running.js and hands the promise to the script.
static constexpr ASCIILiteral importOfModuleThatRequireRuns = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/running.js" displayPath="running.js" body=f1 requests=[{target=m2 completes=true}] exports=[{name="ended" variable=g0}]
module m2 path="/imported.js" displayPath="imported.js" body=f2
module m3 path="/imports-back.js" displayPath="imports-back.js" body=f3
entry module=m0
variable g0 name="ended" module=m1 declared={type=anything}
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = RequireModule module=m1 referrer=m0 specifier="./running.js"
    v3 = String string="ends"
    v4 = String string="main.js"
    v5 = Call v1, -, v3, v4
    Return
}
function f1 name="running" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="running.js"
    v3 = String string="begins"
    v4 = Call v1, -, v3, v2
    v5 = RequireModule module=m3 referrer=m1 specifier="./imports-back.js"
    v6 = String string="ends"
    v7 = Call v1, -, v6, v2
    v8 = Boolean boolean=true
    PutModuleVariable v8 variable=g0
    Return
}
function f2 name="imported" module=m2 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="imported.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f3 name="importsBack" module=m3 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="imports-back.js"
    v4 = Call v1, -, v2, v3
    v5 = ImportModule module=m1 referrer=m3
    v6 = PropGet v0 key="register" [isFreeName]
    v7 = Call v6, -, v5
    Return
}
)IR"_s;

// The same, where running.js has no code of its own: it gets no start, and the walk of the run time stores its status in the table. imported.js, which has the start here, requires imports-back.js.
static constexpr ASCIILiteral importOfModuleWithoutBodyThatRequireRuns = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/running.js" displayPath="running.js" requests=[{target=m2 completes=true}]
module m2 path="/imported.js" displayPath="imported.js" body=f1
module m3 path="/imports-back.js" displayPath="imports-back.js" body=f2
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = RequireModule module=m1 referrer=m0 specifier="./running.js"
    v3 = String string="ends"
    v4 = String string="main.js"
    v5 = Call v1, -, v3, v4
    Return
}
function f1 name="imported" module=m2 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="imported.js"
    v3 = String string="begins"
    v4 = Call v1, -, v3, v2
    v5 = RequireModule module=m3 referrer=m2 specifier="./imports-back.js"
    v6 = String string="ends"
    v7 = Call v1, -, v6, v2
    Return
}
function f2 name="importsBack" module=m3 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="imports-back.js"
    v4 = Call v1, -, v2, v3
    v5 = ImportModule module=m1 referrer=m3
    v6 = PropGet v0 key="register" [isFreeName]
    v7 = Call v6, -, v5
    Return
}
)IR"_s;

// The first program, where running.js throws behind its require() and main.js catches that. didThrowInBody() gives the record the status before the capability is rejected,
// so recordIfExists() found the record too: here takeStatus() has nothing to take.
static constexpr ASCIILiteral importOfModuleThatRequireRunsAndThatThrows = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/running.js" displayPath="running.js" body=f1 requests=[{target=m2 completes=true}]
module m2 path="/imported.js" displayPath="imported.js" body=f2
module m3 path="/imports-back.js" displayPath="imports-back.js" body=f3
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    Jump -> next b1()
  b1() handler=b2():
    v2 = RequireModule module=m1 referrer=m0 specifier="./running.js"
    Jump -> next b3()
  b2() role=CatchHandler:
    v3 = CatchValue
    v4 = String string="require() threw"
    v5 = Call v1, -, v4, v3
    Jump -> next b3()
  b3():
    v6 = String string="ends"
    v7 = String string="main.js"
    v8 = Call v1, -, v6, v7
    Return
}
function f1 name="running" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="begins"
    v3 = String string="running.js"
    v4 = Call v1, -, v2, v3
    v5 = RequireModule module=m3 referrer=m1 specifier="./imports-back.js"
    v6 = String string="thrown by running.js"
    Throw v6
}
function f2 name="imported" module=m2 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="imported.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f3 name="importsBack" module=m3 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="imports-back.js"
    v4 = Call v1, -, v2, v3
    v5 = ImportModule module=m1 referrer=m3
    v6 = PropGet v0 key="register" [isFreeName]
    v7 = Call v6, -, v5
    Return
}
)IR"_s;

static constexpr ASCIILiteral scriptStart = R"js(
globalThis.report = (label, value) => print(label + ": " + value);
globalThis.id = value => value;
globalThis.input = 1;
globalThis.register = closure => { globalThis.registered = closure; };
globalThis.registerImports = (first, second, last) => Object.assign(globalThis, { importFirst: first, importSecond: second, importLast: last });
)js"_s;

static constexpr ASCIILiteral scriptEnd = R"js(.then(() => {
    if (globalThis.registered)
        report("the closure, called later", registered());
}).catch(error => print("the script threw " + error));
)js"_s;

static std::unique_ptr<CompileOutput> compileOnThisThread(const String& programText, ASCIILiteral referencesWord, OptionWords optionWords)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    AOT_TEST_CHECK(!applyOptionWord(options, referencesWord));
    for (ASCIILiteral word : optionWords)
        AOT_TEST_CHECK(!applyOptionWord(options, word));
    BuildContext build(options);
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(programText), Typed::IRForm::AsRead, build);
    if (!program) {
        build.diagnostics().finish(nullptr);
        Typed::printDiagnostics(build.diagnostics(), WTF::dataFile());
    }
    AOT_TEST_CHECK(program);

    std::unique_ptr<CompileOutput> output = compileParsedProgram(WTF::move(program), build);
    if (output->image.isEmpty())
        Typed::printDiagnostics(output->diagnostics, WTF::dataFile());
    AOT_TEST_CHECK(!output->image.isEmpty() && output->entryModuleIDs.size() == 1);
    return output;
}

static String runInShell(const String& programText, ASCIILiteral referencesWord, OptionWords optionWords, ASCIILiteral scriptAfterEntryPoint = scriptEnd)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileOnThisThread(programText, referencesWord, optionWords);
    });
    TemporaryFile image(".image"_s, output->image.span());
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString(scriptStart, "runCompiledEntryPoint("_s, output->entryModuleIDs[0], ')', scriptAfterEntryPoint),
    };

    String shell = shellPath();
    ChildProcessResult result = ChildProcessLauncher(String(shell), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shell, " ", result.description(), "\n", result.output);
    AOT_TEST_CHECK(result.exitedWith(EXIT_SUCCESS));
    return makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
}

static void checkShellOutput(const String& programText, OptionWords optionWords, ASCIILiteral expectedOutput, ASCIILiteral scriptAfterEntryPoint = scriptEnd)
{
    for (ASCIILiteral referencesWord : { "references=wide"_s, "references=narrow"_s }) {
        String output = runInShell(programText, referencesWord, optionWords, scriptAfterEntryPoint);
        if (output != expectedOutput)
            dataLog("with ", referencesWord, " the shell printed\n", output, "expected\n", expectedOutput);
        AOT_TEST_CHECK(output == expectedOutput);
    }
}

// The modules whose start evaluates what they import, runs their code and stores their status in the table.
static uint64_t numberOfModulesWithState(const String& programText)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileOnThisThread(programText, "references=wide"_s, { });
    });
    return output->statistics.values()[static_cast<size_t>(StatisticID::ModulesWithState)];
}

static void testRequiredCommonJSModules()
{
    checkShellOutput(makeString(mainThatRequires, commonJSModules), { "validate"_s }, R"(module.exports = function, called: called
a property of that function: tagged
exports.a: 41
the function of the other module, called: called
module.exports === exports: true
module.id: /chained.cjs
typeof require: function
module is one object: true
a store to module.exports is a store to the variable: true
exports.a of the other module + 1: 42
a second require answers the same object: true
)"_s);
}

static void testImportedCommonJSModules()
{
    // The loader reads the exports under every name, so the image would list members of the host's classes as constants, and the realm of the shell has no such class.
    checkShellOutput(makeString(mainThatImports, commonJSModules, facesOfCommonJSModules), { "validate"_s, "disable=flow:ConstantBuiltinLocations"_s }, R"(the function of the other module, called: called
module.exports === exports: true
module.id: /chained.cjs
typeof require: function
module is one object: true
a store to module.exports is a store to the variable: true
the default import of module.exports = function, called: called
a property of that function, imported by name: tagged
exports.a, imported by name: 41
the default import of exports, .a: 41
exports.a of the other module + 1, imported by name: 42
)"_s);
}

static void testCommonJSEntryPoint()
{
    checkShellOutput(commonJSEntryPoint, { "validate"_s }, R"(exports.a of the required module: 41
module.id: .
module.exports === exports: true
)"_s);
}

static void testRecordsOfRequire()
{
    checkShellOutput(recordsOfRequire, { }, R"(children before the first require: 0
the id of the parent, read while the child runs: .
children behind two requires of one module: 1
the parent of the child is the module: true
the child shows what it exports: true
)"_s);
}

static void testImportMetaKept()
{
    checkShellOutput(importMetaKept, { "validate"_s }, R"(import.meta === import.meta: true
the variable === import.meta: true
typeof the variable: object
the variable === import.meta of another module: false
typeof that one: object
the closure, called later: true
)"_s);
}

static void testImportMetaPassedOn()
{
    // Whoever holds import.meta can load any module of the host, and validation looks for all of them in the realm.
    checkShellOutput(importMetaPassedOn, { }, R"(id(import.meta) === import.meta: true
)"_s);
}

static void testModuleBuiltinsInEveryPosition()
{
    checkShellOutput(moduleBuiltinsInEveryPosition, { "validate"_s }, R"(which of two callbacks was called by value: 1
its parameter was import.meta: true
the field holds import.meta: true
typeof exports, require, module, module.exports, read from fields: object function object function
the closure, called later: true
)"_s);
}

static void testTerminationDuringImportHandoff()
{
    checkShellOutput(dynamicImportsFromScript, { }, R"(termination was pending: true
runs: below-first.js
runs: first.js
runs: file.cjs
runs: second.js
first import: fulfilled
second import: fulfilled
later import: fulfilled
)"_s, R"js(.then(async () => {
    async function outcome(promise)
    {
        try {
            await promise;
            return "fulfilled";
        } catch (error) {
            return "rejected with " + error;
        }
    }
    const first = importFirst();
    for (let turn = 0; turn < 5; ++turn)
        await null;
    const second = importSecond();
    (async () => {
        for (let turn = 0; turn < 40; ++turn)
            await null;
    })();
    requestTerminationOnResolve("/below-first.js");
    report("termination was pending", callAndCancelTermination(importLast));
    report("first import", await outcome(first));
    report("second import", await outcome(second));
    report("later import", await outcome(importSecond()));
}).catch(error => print("the script threw " + error));
)js"_s);
}

static constexpr ASCIILiteral reportImportOfRunningModule = R"js(.then(async () => {
    let outcome = "pending";
    registered.then(names => { outcome = "fulfilled, ended: " + names.ended; }, error => { outcome = "rejected with " + error; });
    for (let turn = 0; turn < 16; ++turn)
        await null;
    report("import() of running.js", outcome);
}).catch(error => print("the script threw " + error));
)js"_s;

static void testImportOfModuleThatRequireRuns()
{
    // Without a start of running.js its record would have the status, and the import() would be settled on any engine.
    AOT_TEST_CHECK(numberOfModulesWithState(importOfModuleThatRequireRuns) == 1);
    checkShellOutput(importOfModuleThatRequireRuns, { "validate"_s }, R"(runs: imported.js
begins: running.js
runs: imports-back.js
ends: running.js
ends: main.js
import() of running.js: fulfilled, ended: true
)"_s, reportImportOfRunningModule);
}

static void testImportOfModuleWithoutBodyThatRequireRuns()
{
    checkShellOutput(importOfModuleWithoutBodyThatRequireRuns, { "validate"_s }, R"(begins: imported.js
runs: imports-back.js
ends: imported.js
ends: main.js
import() of running.js: fulfilled, ended: undefined
)"_s, reportImportOfRunningModule);
}

static void testImportOfModuleThatRequireRunsAndThatThrows()
{
    AOT_TEST_CHECK(numberOfModulesWithState(importOfModuleThatRequireRunsAndThatThrows) == 1);
    checkShellOutput(importOfModuleThatRequireRunsAndThatThrows, { "validate"_s }, R"(runs: imported.js
begins: running.js
runs: imports-back.js
require() threw: thrown by running.js
ends: main.js
import() of running.js: rejected with thrown by running.js
)"_s, reportImportOfRunningModule);
}

std::span<const NativeTest> moduleProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "module-programs/required-commonjs-modules"_s, testRequiredCommonJSModules },
        { "module-programs/imported-commonjs-modules"_s, testImportedCommonJSModules },
        { "module-programs/commonjs-entry-point"_s, testCommonJSEntryPoint },
        { "module-programs/records-of-require"_s, testRecordsOfRequire },
        { "module-programs/import-meta-kept"_s, testImportMetaKept },
        { "module-programs/import-meta-passed-on"_s, testImportMetaPassedOn },
        { "module-programs/module-builtins-in-every-position"_s, testModuleBuiltinsInEveryPosition },
        { "module-programs/termination-during-import-handoff"_s, testTerminationDuringImportHandoff },
        { "module-programs/import-of-module-that-require-runs"_s, testImportOfModuleThatRequireRuns },
        { "module-programs/import-of-module-without-body-that-require-runs"_s, testImportOfModuleWithoutBodyThatRequireRuns },
        { "module-programs/import-of-module-that-require-runs-and-that-throws"_s, testImportOfModuleThatRequireRunsAndThatThrows },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
