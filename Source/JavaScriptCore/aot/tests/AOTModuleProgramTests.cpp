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
#include <wtf/text/StringBuilder.h>

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

// first.js and second.js import each other, and main.js requires first.js. second.js requires imports-back.js, which calls import() on second.js while it runs. first.js calls import() on second.js behind the end of second.js.
static constexpr ASCIILiteral laterImportOfCycleMember = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/first.js" displayPath="first.js" body=f1 requests=[{target=m2 completes=true}]
module m2 path="/second.js" displayPath="second.js" body=f2 requests=[{target=m1 completes=true}]
module m3 path="/imports-back.js" displayPath="imports-back.js" body=f3
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = RequireModule module=m1 referrer=m0 specifier="./first.js"
    v3 = String string="ends"
    v4 = String string="main.js"
    v5 = Call v1, -, v3, v4
    Return
}
function f1 name="first" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="first.js"
    v3 = String string="begins"
    v4 = Call v1, -, v3, v2
    v5 = ImportModule module=m2 referrer=m1
    v6 = PropGet v0 key="keep" [isFreeName]
    v7 = String string="the later import() of second.js"
    v8 = Call v6, -, v7, v5
    v9 = String string="ends"
    v10 = Call v1, -, v9, v2
    Return
}
function f2 name="second" module=m2 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="second.js"
    v3 = String string="begins"
    v4 = Call v1, -, v3, v2
    v5 = RequireModule module=m3 referrer=m2 specifier="./imports-back.js"
    v6 = String string="ends"
    v7 = Call v1, -, v6, v2
    Return
}
function f3 name="importsBack" module=m3 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="imports-back.js"
    v4 = Call v1, -, v2, v3
    v5 = ImportModule module=m2 referrer=m3
    v6 = PropGet v0 key="keep" [isFreeName]
    v7 = String string="the import() of second.js while it runs"
    v8 = Call v6, -, v7, v5
    Return
}
)IR"_s;

// throws.js and ended.js import each other, and imports-cycle.js imports throws.js. The script calls the closure of main.js, which requires imports-cycle.js. throws.js calls import() on ended.js, whose code has run, and throws.
static constexpr ASCIILiteral importOfMemberOfRequiredCycleThatFails = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/imports-cycle.js" displayPath="imports-cycle.js" body=f1 requests=[{target=m2 completes=true}]
module m2 path="/throws.js" displayPath="throws.js" body=f2 requests=[{target=m3 completes=true}]
module m3 path="/ended.js" displayPath="ended.js" body=f3 requests=[{target=m2 completes=true}]
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f4
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="register" [isFreeName]
    v3 = Call v2, -, v0
    Return
}
function f1 name="importsCycle" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="imports-cycle.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f2 name="throws" module=m2 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="begins"
    v3 = String string="throws.js"
    v4 = Call v1, -, v2, v3
    v5 = ImportModule module=m3 referrer=m2
    v6 = PropGet v0 key="keep" [isFreeName]
    v7 = String string="the import() of ended.js"
    v8 = Call v6, -, v7, v5
    v9 = String string="thrown by throws.js"
    Throw v9
}
function f3 name="ended" module=m3 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="ended.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f4 name="requireCycle" results=[{type=anything}] {
    v0 = RequireModule module=m1 referrer=m0 specifier="./imports-cycle.js"
    Return v0
}
)IR"_s;

// The same cycle with no require(): the script calls the closure of main.js, which calls import() on throws.js.
static constexpr ASCIILiteral importOfMemberOfImportedCycleThatFails = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/throws.js" displayPath="throws.js" body=f1 requests=[{target=m2 completes=true}]
module m2 path="/ended.js" displayPath="ended.js" body=f2 requests=[{target=m1 completes=true}]
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f3
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="register" [isFreeName]
    v3 = Call v2, -, v0
    Return
}
function f1 name="throws" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="begins"
    v3 = String string="throws.js"
    v4 = Call v1, -, v2, v3
    v5 = ImportModule module=m2 referrer=m1
    v6 = PropGet v0 key="keep" [isFreeName]
    v7 = String string="the import() of ended.js"
    v8 = Call v6, -, v7, v5
    v9 = String string="thrown by throws.js"
    Throw v9
}
function f2 name="ended" module=m2 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="ended.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f3 name="importCycle" results=[{type=anything}] {
    v0 = ImportModule module=m1 referrer=m0
    Return v0
}
)IR"_s;

// root.js and member.js import each other, and main.js requires root.js. While root.js runs it requires imports-member.js, which calls import() on member.js, whose code has run. Behind that root.js calls import() on member.js itself.
static constexpr ASCIILiteral importOfEndedCycleMemberWhileRootRuns = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/root.js" displayPath="root.js" body=f1 requests=[{target=m2 completes=true}]
module m2 path="/member.js" displayPath="member.js" body=f2 requests=[{target=m1 completes=true}]
module m3 path="/imports-member.js" displayPath="imports-member.js" body=f3
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = RequireModule module=m1 referrer=m0 specifier="./root.js"
    v3 = String string="ends"
    v4 = String string="main.js"
    v5 = Call v1, -, v3, v4
    Return
}
function f1 name="root" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="root.js"
    v3 = String string="begins"
    v4 = Call v1, -, v3, v2
    v5 = RequireModule module=m3 referrer=m1 specifier="./imports-member.js"
    v6 = ImportModule module=m2 referrer=m1
    v7 = PropGet v0 key="keep" [isFreeName]
    v8 = String string="the import() of member.js behind the cycle"
    v9 = Call v7, -, v8, v6
    v10 = String string="ends"
    v11 = Call v1, -, v10, v2
    Return
}
function f2 name="member" module=m2 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="member.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f3 name="importsMember" module=m3 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="imports-member.js"
    v4 = Call v1, -, v2, v3
    v5 = ImportModule module=m2 referrer=m3
    v6 = PropGet v0 key="keep" [isFreeName]
    v7 = String string="the import() of member.js while its root runs"
    v8 = Call v6, -, v7, v5
    Return
}
)IR"_s;

// first.js, behind.js and evaluated.js import each other in a ring. first.js imports requires.js in front of behind.js, and requires.js requires evaluated.js: a require() evaluates it for good.
// So behind.js leads to nothing that still runs when its code has run, though first.js, which is of the same ring, does.
static constexpr ASCIILiteral cycleMemberBehindEvaluatedMember = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/first.js" displayPath="first.js" body=f1 requests=[{target=m2 completes=true}, {target=m3 completes=true}]
module m2 path="/requires.js" displayPath="requires.js" body=f2
module m3 path="/behind.js" displayPath="behind.js" body=f3 requests=[{target=m4 completes=true}]
module m4 path="/evaluated.js" displayPath="evaluated.js" body=f4 requests=[{target=m1 completes=true}]
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f5
    v1 = NewClosure function=f6
    v2 = Builtin builtin=GlobalThis
    v3 = PropGet v2 key="registerImports" [isFreeName]
    v4 = Call v3, -, v0, v1, v1
    Return
}
function f1 name="first" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="begins"
    v3 = String string="first.js"
    v4 = Call v1, -, v2, v3
    v5 = ImportModule module=m3 referrer=m1
    v6 = PropGet v0 key="keep" [isFreeName]
    v7 = String string="the import() of behind.js"
    v8 = Call v6, -, v7, v5
    v9 = String string="thrown by first.js"
    Throw v9
}
function f2 name="requires" module=m2 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="requires.js"
    v3 = String string="begins"
    v4 = Call v1, -, v3, v2
    v5 = RequireModule module=m4 referrer=m2 specifier="./evaluated.js"
    v6 = String string="ends"
    v7 = Call v1, -, v6, v2
    Return
}
function f3 name="behind" module=m3 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="behind.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f4 name="evaluated" module=m4 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="evaluated.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f5 name="importFirst" results=[{type=anything}] {
    v0 = ImportModule module=m1 referrer=m0
    Return v0
}
function f6 name="importBehind" results=[{type=anything}] {
    v0 = ImportModule module=m3 referrer=m0
    Return v0
}
)IR"_s;

// Two cycles under one evaluation: outer.js and waits.js, and below them root.js and member.js. The inner cycle is evaluated when root.js ends, while waits.js still waits for outer.js, which throws.
static constexpr ASCIILiteral twoCyclesUnderOneEvaluation = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/outer.js" displayPath="outer.js" body=f1 requests=[{target=m2 completes=true}, {target=m3 completes=true}]
module m2 path="/waits.js" displayPath="waits.js" body=f2 requests=[{target=m1 completes=true}]
module m3 path="/between.js" displayPath="between.js" body=f3 requests=[{target=m4 completes=true}]
module m4 path="/root.js" displayPath="root.js" body=f4 requests=[{target=m5 completes=true}]
module m5 path="/member.js" displayPath="member.js" body=f5 requests=[{target=m4 completes=true}]
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f6
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="register" [isFreeName]
    v3 = Call v2, -, v0
    Return
}
function f1 name="outer" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="begins"
    v3 = String string="outer.js"
    v4 = Call v1, -, v2, v3
    v5 = PropGet v0 key="keep" [isFreeName]
    v6 = ImportModule module=m2 referrer=m1
    v7 = String string="the import() of waits.js"
    v8 = Call v5, -, v7, v6
    v9 = ImportModule module=m5 referrer=m1
    v10 = String string="the import() of member.js"
    v11 = Call v5, -, v10, v9
    v12 = String string="thrown by outer.js"
    Throw v12
}
function f2 name="waits" module=m2 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="waits.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f3 name="between" module=m3 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="between.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f4 name="root" module=m4 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="root.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f5 name="member" module=m5 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="member.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f6 name="importOuter" results=[{type=anything}] {
    v0 = ImportModule module=m1 referrer=m0
    Return v0
}
)IR"_s;

// first.js, calls.js and member.js are a cycle that a require() of the script runs, and no import() is in the program. calls.js calls a closure of main.js that requires required.js, which imports member.js:
// that require() begins an evaluation of its own, and member.js is evaluated with it while first.js, which throws later, still runs.
static constexpr ASCIILiteral moduleRequiredWhileCycleRuns = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m1 kind=Require completes=true}, {target=m3 kind=Require completes=true}, {target=m4 kind=Require completes=true}]
module m1 path="/first.js" displayPath="first.js" body=f1 requests=[{target=m2 completes=true}, {target=m3 completes=true}]
module m2 path="/calls.js" displayPath="calls.js" body=f2 requests=[{target=m1 completes=true}]
module m3 path="/member.js" displayPath="member.js" body=f3 requests=[{target=m1 completes=true}]
module m4 path="/required.js" displayPath="required.js" body=f4 requests=[{target=m3 completes=true}]
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f5
    v1 = NewClosure function=f6
    v2 = NewClosure function=f7
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="registerImports" [isFreeName]
    v5 = Call v4, -, v0, v1, v2
    Return
}
function f1 name="first" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="begins"
    v3 = String string="first.js"
    v4 = Call v1, -, v2, v3
    v5 = String string="thrown by first.js"
    Throw v5
}
function f2 name="calls" module=m2 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="calls.js"
    v3 = String string="begins"
    v4 = Call v1, -, v3, v2
    v5 = PropGet v0 key="importLast" [isFreeName]
    v6 = Call v5
    v7 = String string="ends"
    v8 = Call v1, -, v7, v2
    Return
}
function f3 name="member" module=m3 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="member.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f4 name="required" module=m4 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="required.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f5 name="requireFirst" module=m0 results=[{type=anything}] {
    EvaluateModule module=m1 referrer=m0
    Return
}
function f6 name="requireMember" module=m0 results=[{type=anything}] {
    EvaluateModule module=m3 referrer=m0
    Return
}
function f7 name="requireRequired" module=m0 results=[{type=anything}] {
    EvaluateModule module=m4 referrer=m0
    Return
}
)IR"_s;

static constexpr ASCIILiteral scriptStart = R"js(
globalThis.report = (label, value) => print(label + ": " + value);
globalThis.id = value => value;
globalThis.input = 1;
globalThis.register = closure => { globalThis.registered = closure; };
globalThis.registerImports = (first, second, last) => Object.assign(globalThis, { importFirst: first, importSecond: second, importLast: last });
globalThis.kept = [];
globalThis.keep = (label, promise) => { kept.push({ label, promise }); };
globalThis.reportKept = async () => {
    for (let turn = 0; turn < 32; ++turn)
        await null;
    const states = [];
    for (let index = 0; index < kept.length; ++index) {
        states.push("pending");
        kept[index].promise.then(() => { states[index] = "fulfilled"; }, error => { states[index] = "rejected with " + error; });
    }
    for (let turn = 0; turn < 32; ++turn)
        await null;
    for (let index = 0; index < states.length; ++index)
        report(kept[index].label, states[index]);
};
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

// In each of these the start of a member of a cycle of imports stores the status of the member. What the promises of keep() come to is what JavaScriptCore's own loader makes of the same modules.
static void testLaterImportOfCycleMember()
{
    checkShellOutput(laterImportOfCycleMember, { "validate"_s }, R"(begins: second.js
runs: imports-back.js
ends: second.js
begins: first.js
ends: first.js
ends: main.js
the import() of second.js while it runs: pending
the later import() of second.js: fulfilled
)"_s, R"js(.then(reportKept).catch(error => print("the script threw " + error));
)js"_s);
}

static void testImportOfMemberOfRequiredCycleThatFails()
{
    checkShellOutput(importOfMemberOfRequiredCycleThatFails, { "validate"_s }, R"(runs: ended.js
begins: throws.js
require() threw: thrown by throws.js
the import() of ended.js: rejected with thrown by throws.js
)"_s, R"js(.then(() => {
    try {
        registered();
        report("require()", "returned");
    } catch (error) {
        report("require() threw", error);
    }
    return reportKept();
}).catch(error => print("the script threw " + error));
)js"_s);
}

static void testImportOfMemberOfImportedCycleThatFails()
{
    checkShellOutput(importOfMemberOfImportedCycleThatFails, { "validate"_s }, R"(runs: ended.js
begins: throws.js
the import() of throws.js: rejected with thrown by throws.js
the import() of ended.js: rejected with thrown by throws.js
)"_s, R"js(.then(() => {
    keep("the import() of throws.js", registered());
    return reportKept();
}).catch(error => print("the script threw " + error));
)js"_s);
}

static void testImportOfEndedCycleMemberWhileRootRuns()
{
    checkShellOutput(importOfEndedCycleMemberWhileRootRuns, { "validate"_s }, R"(runs: member.js
begins: root.js
runs: imports-member.js
ends: root.js
ends: main.js
the import() of member.js while its root runs: pending
the import() of member.js behind the cycle: fulfilled
)"_s, R"js(.then(reportKept).catch(error => print("the script threw " + error));
)js"_s);
}

// A member that leads to no start that still runs is evaluated when its own code has run, whatever else of its ring still runs: the member that was entered first throws, and the other stays evaluated.
static void testCycleMemberBehindEvaluatedMember()
{
    checkShellOutput(cycleMemberBehindEvaluatedMember, { "validate"_s }, R"(begins: requires.js
runs: evaluated.js
ends: requires.js
runs: behind.js
begins: first.js
the import() of first.js: rejected with thrown by first.js
the import() of behind.js: fulfilled
the later import() of behind.js: fulfilled
)"_s, R"js(.then(async () => {
    keep("the import() of first.js", importFirst());
    await reportKept();
    kept.length = 0;
    keep("the later import() of behind.js", importSecond());
    await reportKept();
}).catch(error => print("the script threw " + error));
)js"_s);
}

static void testTwoCyclesUnderOneEvaluation()
{
    checkShellOutput(twoCyclesUnderOneEvaluation, { "validate"_s }, R"(runs: waits.js
runs: member.js
runs: root.js
runs: between.js
begins: outer.js
the import() of outer.js: rejected with thrown by outer.js
the import() of waits.js: rejected with thrown by outer.js
the import() of member.js: fulfilled
)"_s, R"js(.then(() => {
    keep("the import() of outer.js", registered());
    return reportKept();
}).catch(error => print("the script threw " + error));
)js"_s);
}

// Each module of a chain requires the next from its code, so more evaluations are in progress at once than there are marks, and the last of them have one mark. The last module of the chain requires outer.js,
// which requires first.js. first.js and second.js import each other, and first.js imports outer.js, which is being evaluated with the mark that the cycle has.
static String cycleBelowMoreEvaluationsThanMarks()
{
    static constexpr unsigned lengthOfChain = 256;
    unsigned outer = lengthOfChain + 1;
    unsigned first = lengthOfChain + 2;
    unsigned second = lengthOfChain + 3;
    StringBuilder modules;
    StringBuilder functions;
    modules.append("module m0 path=\"/main.js\" displayPath=\"main.js\" body=f0\n"_s);
    functions.append("function f0 name=\"main\" kind=Module results=[{type=anything}] {\n    v0 = Builtin builtin=GlobalThis\n    v1 = RequireModule module=m1 referrer=m0 specifier=\"./link1.js\"\n    v2 = ImportModule module=m"_s, second,
        " referrer=m0\n    v3 = PropGet v0 key=\"keep\" [isFreeName]\n    v4 = String string=\"the import() of second.js behind the chain\"\n    v5 = Call v3, -, v4, v2\n    Return\n}\n"_s);
    for (unsigned link = 1; link <= lengthOfChain; ++link) {
        ASCIILiteral nameOfNext = link == lengthOfChain ? "outer"_s : "link"_s;
        modules.append("module m"_s, link, " path=\"/link"_s, link, ".js\" displayPath=\"link"_s, link, ".js\" body=f"_s, link, '\n');
        functions.append("function f"_s, link, " name=\"link"_s, link, "\" module=m"_s, link, " kind=Module results=[{type=anything}] {\n    v0 = RequireModule module=m"_s, link + 1, " referrer=m"_s, link, " specifier=\"./"_s, nameOfNext);
        if (link != lengthOfChain)
            functions.append(link + 1);
        functions.append(".js\"\n    Return\n}\n"_s);
    }
    modules.append("module m"_s, outer, " path=\"/outer.js\" displayPath=\"outer.js\" body=f"_s, outer, '\n');
    modules.append("module m"_s, first, " path=\"/first.js\" displayPath=\"first.js\" body=f"_s, first, " requests=[{target=m"_s, second, " completes=true}, {target=m"_s, outer, " completes=true}]\n"_s);
    modules.append("module m"_s, second, " path=\"/second.js\" displayPath=\"second.js\" body=f"_s, second, " requests=[{target=m"_s, first, " completes=true}]\nentry module=m0\n"_s);
    functions.append("function f"_s, outer, " name=\"outer\" module=m"_s, outer, " kind=Module results=[{type=anything}] {\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"report\" [isFreeName]\n    v2 = RequireModule module=m"_s, first, " referrer=m"_s, outer,
        " specifier=\"./first.js\"\n    v3 = String string=\"ends\"\n    v4 = String string=\"outer.js\"\n    v5 = Call v1, -, v3, v4\n    Return\n}\n"_s);
    auto appendBodyThatReports = [&](unsigned module, ASCIILiteral name) {
        functions.append("function f"_s, module, " name=\""_s, name, "\" module=m"_s, module, " kind=Module results=[{type=anything}] {\n    v0 = Builtin builtin=GlobalThis\n    v1 = PropGet v0 key=\"report\" [isFreeName]\n    v2 = String string=\"runs\"\n    v3 = String string=\""_s, name,
            ".js\"\n    v4 = Call v1, -, v2, v3\n    Return\n}\n"_s);
    };
    appendBodyThatReports(first, "first"_s);
    appendBodyThatReports(second, "second"_s);
    return makeString(modules.toString(), functions.toString());
}

static void testCycleBelowMoreEvaluationsThanMarks()
{
    checkShellOutput(cycleBelowMoreEvaluationsThanMarks(), { "validate"_s }, R"(runs: second.js
runs: first.js
ends: outer.js
the import() of second.js behind the chain: fulfilled
)"_s, R"js(.then(reportKept).catch(error => print("the script threw " + error));
)js"_s);
}

static void testModuleRequiredWhileCycleRuns()
{
    checkShellOutput(moduleRequiredWhileCycleRuns, { "validate"_s }, R"(begins: calls.js
runs: member.js
runs: required.js
ends: calls.js
begins: first.js
the require() of first.js: threw thrown by first.js
the later require() of member.js: returned
the later require() of required.js: returned
)"_s, R"js(.then(() => {
    const outcome = require => {
        try {
            require();
            return "returned";
        } catch (error) {
            return "threw " + error;
        }
    };
    report("the require() of first.js", outcome(importFirst));
    report("the later require() of member.js", outcome(importSecond));
    report("the later require() of required.js", outcome(importLast));
}).catch(error => print("the script threw " + error));
)js"_s);
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
        { "module-programs/later-import-of-cycle-member"_s, testLaterImportOfCycleMember },
        { "module-programs/import-of-member-of-required-cycle-that-fails"_s, testImportOfMemberOfRequiredCycleThatFails },
        { "module-programs/import-of-member-of-imported-cycle-that-fails"_s, testImportOfMemberOfImportedCycleThatFails },
        { "module-programs/import-of-ended-cycle-member-while-root-runs"_s, testImportOfEndedCycleMemberWhileRootRuns },
        { "module-programs/cycle-member-behind-evaluated-member"_s, testCycleMemberBehindEvaluatedMember },
        { "module-programs/two-cycles-under-one-evaluation"_s, testTwoCyclesUnderOneEvaluation },
        { "module-programs/module-required-while-cycle-runs"_s, testModuleRequiredWhileCycleRuns },
        { "module-programs/cycle-below-more-evaluations-than-marks"_s, testCycleBelowMoreEvaluationsThanMarks },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
