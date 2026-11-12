/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTDriver.h"
#include "AOTIRParser.h"
#include "AOTImageFormat.h"
#include "AOTNativeTest.h"
#include "AOTNativeTestReasons.h"
#include "AOTOptionWords.h"
#include "AOTProgramFile.h"
#include "AOTProgramTables.h"
#include "AOTShellProcess.h"
#include "AOTTemporaryFile.h"
#include "AOTTestHarness.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <initializer_list>
#include <stdlib.h>
#include <wtf/DataLog.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

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

static constexpr ASCIILiteral commonJSFacades = R"IR(module m4 path="#4" displayPath="function.cjs" exportRule=CommonJs body=f5 requests=[{target=m1 kind=Require completes=true}] syntheticExports=[{name="default" variable=g3}, {name="tag" variable=g4}]
module m5 path="#5" displayPath="filled.cjs" exportRule=CommonJs body=f6 requests=[{target=m2 kind=Require completes=true}] syntheticExports=[{name="a" variable=g5}, {name="default" variable=g6}]
module m6 path="#6" displayPath="chained.cjs" exportRule=CommonJs body=f7 requests=[{target=m3 kind=Require completes=true}] syntheticExports=[{name="sum" variable=g7}]
variable g3 name="default" module=m4 declared={type=anything}
variable g4 name="tag" module=m4 declared={type=anything}
variable g5 name="a" module=m5 declared={type=anything}
variable g6 name="default" module=m5 declared={type=anything}
variable g7 name="sum" module=m6 declared={type=anything}
function f5 name="facadeOfFunctionModule" module=m1 kind=Module results=[{type=anything}] {
    EvaluateModule module=m1 referrer=m4 [isBare]
    Return
}
function f6 name="facadeOfFilledModule" module=m2 kind=Module results=[{type=anything}] {
    EvaluateModule module=m2 referrer=m5 [isBare]
    Return
}
function f7 name="facadeOfChainedModule" module=m3 kind=Module results=[{type=anything}] {
    EvaluateModule module=m3 referrer=m6 [isBare]
    Return
}
)IR"_s;

static constexpr ASCIILiteral commonJSModules = R"IR(module m1 path="/function.cjs" displayPath="function.cjs" kind=CommonJs flags=[CachesEvaluationError] exportRule=CommonJs requireResult=ModuleExports body=f1 exportsVariable=g0
module m2 path="/filled.cjs" displayPath="filled.cjs" kind=CommonJs flags=[CachesEvaluationError] exportRule=CommonJs requireResult=ModuleExports body=f2 exportsVariable=g1
module m3 path="/chained.cjs" displayPath="chained.cjs" kind=CommonJs flags=[CachesEvaluationError] exportRule=CommonJs requireResult=ModuleExports body=f3 exportsVariable=g2 requests=[{target=m2 kind=Require completes=true}, {target=m1 kind=Require completes=true}]
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

static constexpr ASCIILiteral commonJSEntryPoint = R"IR(module m0 path="/main.cjs" displayPath="main.cjs" kind=CommonJs flags=[CachesEvaluationError] exportRule=CommonJs requireResult=ModuleExports body=f0 exportsVariable=g0 requests=[{target=m1 kind=Require completes=true}]
module m1 path="/library.cjs" displayPath="library.cjs" kind=CommonJs flags=[CachesEvaluationError] exportRule=CommonJs requireResult=ModuleExports body=f1
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

static constexpr ASCIILiteral recordsOfRequire = R"IR(module m0 path="/main.cjs" displayPath="main.cjs" kind=CommonJs flags=[CachesEvaluationError, ReachesModuleObjects] exportRule=CommonJs requireResult=ModuleExports body=f0 exportsVariable=g0 requests=[{target=m1 kind=Require completes=true}]
module m1 path="/child.cjs" displayPath="child.cjs" kind=CommonJs flags=[CachesEvaluationError] exportRule=CommonJs requireResult=ModuleExports body=f1 exportsVariable=g1
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

static constexpr ASCIILiteral parentsByLevel = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m1 completes=true isHoisted=true}, {target=m2 completes=true isHoisted=true}, {target=m4 completes=true isHoisted=true}, {target=m1 completes=true}, {target=m2 completes=true}, {target=m3 completes=true}]
module m1 path="/first.cjs" displayPath="first.cjs" kind=CommonJs flags=[CachesEvaluationError] exportRule=CommonJs requireResult=ModuleExports body=f1 exportsVariable=g0 requests=[{target=m2 kind=Require completes=true}, {target=m4 kind=Require completes=true}]
module m2 path="/beside.cjs" displayPath="beside.cjs" kind=CommonJs flags=[CachesEvaluationError, ReachesModuleObjects] exportRule=CommonJs requireResult=ModuleExports body=f2 exportsVariable=g1
module m3 path="/deeper.js" displayPath="deeper.js" body=f3 requests=[{target=m4 completes=true}]
module m4 path="/below.cjs" displayPath="below.cjs" kind=CommonJs flags=[CachesEvaluationError, ReachesModuleObjects] exportRule=CommonJs requireResult=ModuleExports body=f4 exportsVariable=g2
entry module=m0
variable g0 name="exports" module=m1 declared={type=anything} initialization=Undefined
variable g1 name="exports" module=m2 declared={type=anything} initialization=Undefined
variable g2 name="exports" module=m4 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
    Return
}
function f1 name="first" module=m1 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = NewDictionary
    PutModuleVariable v0 variable=g0
    RecordRequire module=m2 referrer=m1
    EvaluateModule module=m2 referrer=m1 [isBare]
    RecordRequire module=m2 referrer=m1
    RecordRequire module=m4 referrer=m1
    EvaluateModule module=m4 referrer=m1 [isBare]
    RecordRequire module=m4 referrer=m1
    Return
}
function f2 name="beside" module=m2 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = NewDictionary
    PutModuleVariable v2 variable=g1
    v4 = ModuleBuiltin builtin=ModuleObject module=m2
    v5 = PropGet v4 key="parent"
    v6 = TypeOf v5
    v7 = String string="the parent of a file that is imported as near to the root as its requirer"
    v8 = Call v1, -, v7, v6
    Return
}
function f3 name="deeper" module=m3 kind=Module results=[{type=anything}] {
    Return
}
function f4 name="below" module=m4 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = NewDictionary
    PutModuleVariable v2 variable=g2
    v4 = ModuleBuiltin builtin=ModuleObject module=m4
    v5 = PropGet v4 key="parent"
    v6 = TypeOf v5
    v7 = String string="the parent of a file that is imported farther from the root"
    v8 = Call v1, -, v7, v6
    Return
}
)IR"_s;

static constexpr ASCIILiteral cacheByLevel = R"IR(module m0 path="/main.js" displayPath="main.js" flags=[IsListedInCache] body=f0 requests=[{target=m1 completes=true isHoisted=true}, {target=m2 completes=true isHoisted=true}, {target=m3 completes=true isHoisted=true}, {target=m5 completes=true isHoisted=true}, {target=m1 completes=true}, {target=m2 completes=true}, {target=m3 completes=true}, {target=m4 completes=true}]
module m1 path="/first.cjs" displayPath="first.cjs" kind=CommonJs flags=[CachesEvaluationError, IsListedInCache] exportRule=CommonJs requireResult=ModuleExports body=f1 exportsVariable=g0 requests=[{target=m6 kind=Require completes=true}]
module m2 path="/beside.cjs" displayPath="beside.cjs" kind=CommonJs flags=[CachesEvaluationError, IsListedInCache] exportRule=CommonJs requireResult=ModuleExports body=f2 exportsVariable=g1 requests=[{target=m3 kind=Require completes=true}]
module m3 path="/last.cjs" displayPath="last.cjs" kind=CommonJs flags=[CachesEvaluationError, ReachesModuleObjects, IsListedInCache] exportRule=CommonJs requireResult=ModuleExports body=f3 exportsVariable=g2
module m4 path="/deeper.js" displayPath="deeper.js" flags=[IsListedInCache] body=f4 requests=[{target=m5 completes=true}]
module m5 path="/below.cjs" displayPath="below.cjs" kind=CommonJs flags=[CachesEvaluationError, IsListedInCache] exportRule=CommonJs requireResult=ModuleExports body=f5 exportsVariable=g3
module m6 path="/own.cjs" displayPath="own.cjs" kind=CommonJs flags=[CachesEvaluationError, IsListedInCache] exportRule=CommonJs requireResult=ModuleExports body=f6 exportsVariable=g4
entry module=m0
variable g0 name="exports" module=m1 declared={type=anything} initialization=Undefined
variable g1 name="exports" module=m2 declared={type=anything} initialization=Undefined
variable g2 name="exports" module=m3 declared={type=anything} initialization=Undefined
variable g3 name="exports" module=m5 declared={type=anything} initialization=Undefined
variable g4 name="exports" module=m6 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
    Return
}
function f1 name="first" module=m1 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = NewDictionary
    PutModuleVariable v2 variable=g0
    v4 = PropGet v0 key="filesInRequireCache" [isFreeName]
    v5 = Call v4
    v6 = String string="while first.cjs runs"
    v7 = Call v1, -, v6, v5
    RecordRequire module=m6 referrer=m1
    EvaluateModule module=m6 referrer=m1 [isBare]
    RecordRequire module=m6 referrer=m1
    Return
}
function f2 name="beside" module=m2 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = NewDictionary
    PutModuleVariable v2 variable=g1
    v4 = PropGet v0 key="deleteRequireCacheEntry" [isFreeName]
    v5 = String string="/last.cjs"
    v6 = Call v4, -, v5
    v7 = PropGet v0 key="filesInRequireCache" [isFreeName]
    v8 = Call v7
    v9 = String string="behind the deletion of last.cjs"
    v10 = Call v1, -, v9, v8
    RecordRequire module=m3 referrer=m2
    EvaluateModule module=m3 referrer=m2 [isBare]
    RecordRequire module=m3 referrer=m2
    Return
}
function f3 name="last" module=m3 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = NewDictionary
    PutModuleVariable v2 variable=g2
    v4 = ModuleBuiltin builtin=ModuleObject module=m3
    v5 = PropGet v4 key="parent"
    v6 = TypeOf v5
    v7 = String string="the parent of last.cjs, which is required behind that"
    v8 = Call v1, -, v7, v6
    Return
}
function f4 name="deeper" module=m4 kind=Module results=[{type=anything}] {
    Return
}
function f5 name="below" module=m5 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = NewDictionary
    PutModuleVariable v0 variable=g3
    Return
}
function f6 name="own" module=m6 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = NewDictionary
    PutModuleVariable v2 variable=g4
    v4 = PropGet v0 key="filesInRequireCache" [isFreeName]
    v5 = Call v4
    v6 = String string="while what first.cjs requires runs"
    v7 = Call v1, -, v6, v5
    Return
}
)IR"_s;

static constexpr ASCIILiteral nothingToRun = R"IR(module m0 path="/main.js" displayPath="main.js" flags=[IsListedInCache] body=f0 requests=[{target=m1 completes=true}]
module m1 path="/nothing.js" displayPath="nothing.js" flags=[IsListedInCache] body=f1
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = PropGet v0 key="filesInRequireCache" [isFreeName]
    v3 = Call v2
    v4 = String string="while main.js runs"
    v5 = Call v1, -, v4, v3
    Return
}
function f1 name="nothing" module=m1 kind=Module results=[{type=anything}] {
  b0():
    Jump -> next b1()
  b1():
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
module m1 path="/library.cjs" displayPath="library.cjs" kind=CommonJs flags=[CachesEvaluationError] exportRule=CommonJs requireResult=ModuleExports body=f1 exportsVariable=g4
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

static constexpr ASCIILiteral fileThatImportsAheadOfLoader = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/root.js" displayPath="root.js" body=f1 requests=[{target=m3 completes=true}, {target=m4 completes=true}]
module m2 path="/asks.cjs" displayPath="asks.cjs" kind=CommonJs flags=[CachesEvaluationError] exportRule=CommonJs requireResult=ModuleExports body=f2 exportsVariable=g0
module m3 path="#3" displayPath="asks.cjs" exportRule=CommonJs body=f3 requests=[{target=m2 kind=Require completes=true}] syntheticExports=[{name="default" variable=g1}]
module m4 path="/beside.js" displayPath="beside.js" body=f4
module m5 path="/late.js" displayPath="late.js" body=f5
module m6 path="/other.js" displayPath="other.js" body=f6
entry module=m0
variable g0 name="exports" module=m2 declared={type=anything} initialization=Undefined
variable g1 name="default" module=m3 declared={type=anything}
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f7
    v1 = NewClosure function=f8
    v2 = NewClosure function=f9
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="registerImports" [isFreeName]
    v5 = Call v4, -, v0, v1, v2
    Return
}
function f1 name="root" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="root.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f2 name="asks" module=m2 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="asks.cjs"
    v3 = String string="begins"
    v4 = Call v1, -, v3, v2
    v5 = CallDirect function=f10
    v6 = String string="goes on"
    v7 = Call v1, -, v6, v2
    v8 = NewDictionary
    PutModuleVariable v8 variable=g0
    Return
}
function f3 name="facadeOfAsks" module=m2 kind=Module results=[{type=anything}] {
    EvaluateModule module=m2 referrer=m3 [isBare]
    Return
}
function f4 name="beside" module=m4 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="beside.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f5 name="late" module=m5 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="late.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f6 name="other" module=m6 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="other.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f7 name="importRoot" results=[{type=anything}] {
    v0 = ImportModule module=m1 referrer=m0
    Return v0
}
function f8 name="importOther" results=[{type=anything}] {
    v0 = ImportModule module=m6 referrer=m0
    Return v0
}
function f9 name="importLate" results=[{type=anything}] {
    v0 = ImportModule module=m5 referrer=m0
    Return v0
}
function f10 name="importsLateUnderHandler" module=m2 results=[{type=anything}] {
  b0() handler=b2():
    v0 = ImportModule module=m5 referrer=m2
    Jump -> next b1(v0)
  b1(v1):
    Return v1
  b2() role=CatchHandler:
    v2 = CatchValue
    v3 = Builtin builtin=GlobalThis
    v4 = PropGet v3 key="report" [isFreeName]
    v5 = String string="a handler of the file caught"
    v6 = Call v4, -, v5, v2
    Return v6
}
)IR"_s;

static constexpr ASCIILiteral importCalledDuringLoad = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/root.js" displayPath="root.js" body=f1 requests=[{target=m5 completes=true}]
module m2 path="/asks.cjs" displayPath="asks.cjs" kind=CommonJs flags=[CachesEvaluationError] exportRule=CommonJs requireResult=ModuleExports body=f2 exportsVariable=g0
module m3 path="/late.js" displayPath="late.js" body=f3
module m4 path="/later.js" displayPath="later.js" body=f4
module m5 path="#5" displayPath="asks.cjs" exportRule=CommonJs body=f5 requests=[{target=m2 kind=Require completes=true}] syntheticExports=[{name="default" variable=g1}]
entry module=m0
variable g0 name="exports" module=m2 declared={type=anything} initialization=Undefined
variable g1 name="default" module=m5 declared={type=anything}
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f6
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="register" [isFreeName]
    v3 = Call v2, -, v0
    Return
}
function f1 name="root" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="root.js"
    v4 = Call v1, -, v2, v3
    v5 = ImportModule module=m4 referrer=m1
    Return
}
function f2 name="asks" module=m2 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = ImportModule module=m3 referrer=m2
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="report" [isFreeName]
    v3 = String string="runs"
    v4 = String string="asks.cjs"
    v5 = Call v2, -, v3, v4
    v6 = NewDictionary
    PutModuleVariable v6 variable=g0
    Return
}
function f3 name="late" module=m3 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="late.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f4 name="later" module=m4 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="later.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f5 name="facadeOfAsks" module=m2 kind=Module results=[{type=anything}] {
    EvaluateModule module=m2 referrer=m5 [isBare]
    Return
}
function f6 name="importRoot" results=[{type=anything}] {
    v0 = ImportModule module=m1 referrer=m0
    Return v0
}
)IR"_s;

static constexpr ASCIILiteral textsInTurns = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
module m1 path="/root.js" displayPath="root.js" body=f1 requests=[{target=m7 completes=true}, {target=m3 completes=true}]
module m2 path="/asks.cjs" displayPath="asks.cjs" kind=CommonJs flags=[CachesEvaluationError] exportRule=CommonJs requireResult=ModuleExports body=f2 exportsVariable=g0
module m3 path="/first.js" displayPath="first.js" body=f3 requests=[{target=m4 completes=true}]
module m4 path="/second.js" displayPath="second.js" body=f4 requests=[{target=m5 completes=true}]
module m5 path="/third.js" displayPath="third.js" body=f5
module m6 path="/late.js" displayPath="late.js" body=f6
module m7 path="#7" displayPath="asks.cjs" exportRule=CommonJs body=f7 requests=[{target=m2 kind=Require completes=true}] syntheticExports=[{name="default" variable=g1}]
entry module=m0
variable g0 name="exports" module=m2 declared={type=anything} initialization=Undefined
variable g1 name="default" module=m7 declared={type=anything}
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f8
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="register" [isFreeName]
    v3 = Call v2, -, v0
    Return
}
function f1 name="root" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="root.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f2 name="asks" module=m2 kind=CommonJsModule flags=[IsSloppy] results=[{type=anything}] {
    v0 = ImportModule module=m6 referrer=m2
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="report" [isFreeName]
    v3 = String string="runs"
    v4 = String string="asks.cjs"
    v5 = Call v2, -, v3, v4
    v6 = NewDictionary
    PutModuleVariable v6 variable=g0
    Return
}
function f3 name="first" module=m3 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="first.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f4 name="second" module=m4 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="second.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f5 name="third" module=m5 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="third.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f6 name="late" module=m6 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="late.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f7 name="facadeOfAsks" module=m2 kind=Module results=[{type=anything}] {
    EvaluateModule module=m2 referrer=m7 [isBare]
    Return
}
function f8 name="importRoot" results=[{type=anything}] {
    v0 = ImportModule module=m1 referrer=m0
    Return v0
}
)IR"_s;

static constexpr ASCIILiteral moduleThatMayFailWhereItIsRequired = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0 requests=[{target=m1 kind=Require}]
module m1 path="/late.js" displayPath="late.js" body=f1 requests=[{target=m2 completes=true}, {target=m3 completes=true}, {target=m4 completes=true}]
module m2 path="/first.js" displayPath="first.js" body=f2
module m3 path="/second.js" displayPath="second.js" body=f3
module m4 path="/third.js" displayPath="third.js" body=f4
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f5
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="register" [isFreeName]
    v3 = Call v2, -, v0
    Return
}
function f1 name="late" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="late.js"
    v4 = Call v1, -, v2, v3
    Return
}
function f2 name="first" module=m2 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="first.js"
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
function f4 name="third" module=m4 kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="report" [isFreeName]
    v2 = String string="runs"
    v3 = String string="third.js"
    v4 = Call v1, -, v2, v3
    v5 = PropGet v0 key="shouldThrow" [isFreeName]
    v6 = ToBoolean v5
    Branch v6 -> taken b1(), notTaken b2()
  b1():
    v7 = String string="boom"
    Throw v7
  b2():
    Return
}
function f5 name="requireLate" results=[{type=anything}] {
    EvaluateModule module=m1 referrer=m0
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
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(Typed::withStandInLibrary(programText), build);
    if (!program)
        failBecauseTextIsNoProgram(__FILE__, __LINE__, build, optionWordsOfReason(referencesWord, optionWords));

    std::unique_ptr<CompileOutput> output = Typed::compileWithMechanisms(WTF::move(program), build, Typed::mechanismsOfProcess());
    if (!output->hasImage())
        failBecauseThereIsNoImage(__FILE__, __LINE__, *output, optionWordsOfReason(referencesWord, optionWords));
    AOT_TEST_CHECK(output->records<uint32_t>(Wire::ResultSection::EntryModuleIds).size() == 1);
    return output;
}

static String runInShell(const String& programText, ASCIILiteral referencesWord, OptionWords optionWords, ASCIILiteral scriptAfterEntryPoint = scriptEnd)
{
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileOnThisThread(programText, referencesWord, optionWords);
    });
    TemporaryFile image(".image"_s, output->section(Wire::ResultSection::Image));
    Vector<String> arguments {
        makeString("--aotImagePath="_s, image.path()),
        String("-e"_s),
        makeString(scriptStart, "runCompiledEntryPoint("_s, output->records<uint32_t>(Wire::ResultSection::EntryModuleIds)[0], ')', scriptAfterEntryPoint),
    };

    String shell = shellPath();
    ChildProcessResult result = ChildProcessLauncher(String(shell), { }).run(arguments.span());
    if (!result.exitedWith(EXIT_SUCCESS))
        dataLogLn(shell, " ", result.description(), "\n", result.output);
    checkThatChildSucceeded(__FILE__, __LINE__, optionWordsOfReason(referencesWord, optionWords), result, EXIT_SUCCESS);
    return makeStringByReplacingAll(result.output, "\r\n"_s, "\n"_s);
}

static void checkShellOutput(const String& programText, OptionWords optionWords, ASCIILiteral expectedOutput, ASCIILiteral scriptAfterEntryPoint = scriptEnd)
{
    for (ASCIILiteral referencesWord : { "references=wide"_s, "references=narrow"_s }) {
        String output = runInShell(programText, referencesWord, optionWords, scriptAfterEntryPoint);
        checkPrintedLines(__FILE__, __LINE__, optionWordsOfReason(referencesWord, optionWords), output, expectedOutput);
    }
}

static void testRequiredCommonJSModules()
{
    checkShellOutput(makeString(mainThatRequires, commonJSModules), { }, R"(module.exports = function, called: called
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
    checkShellOutput(makeString(mainThatImports, commonJSModules, commonJSFacades), { }, R"(the function of the other module, called: called
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
    checkShellOutput(commonJSEntryPoint, { }, R"(exports.a of the required module: 41
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

static void testParentsByLevel()
{
    checkShellOutput(parentsByLevel, { }, R"(the parent of a file that is imported as near to the root as its requirer: undefined
the parent of a file that is imported farther from the root: object
)"_s);
}

static void testCacheByLevel()
{
    checkShellOutput(cacheByLevel, { }, R"(while first.cjs runs: /first.cjs,/beside.cjs,/last.cjs
while what first.cjs requires runs: /first.cjs,/beside.cjs,/last.cjs,/own.cjs
behind the deletion of last.cjs: /first.cjs,/beside.cjs,/own.cjs
the parent of last.cjs, which is required behind that: object
)"_s);
}

static void testNothingToRun()
{
    checkShellOutput(nothingToRun, { }, "while main.js runs: /nothing.js\n"_s);
}

static void testImportMetaKept()
{
    checkShellOutput(importMetaKept, { }, R"(import.meta === import.meta: true
the variable === import.meta: true
typeof the variable: object
the variable === import.meta of another module: false
typeof that one: object
the closure, called later: true
)"_s);
}

static void testImportMetaPassedOn()
{
    checkShellOutput(importMetaPassedOn, { }, R"(id(import.meta) === import.meta: true
)"_s);
}

static void testModuleBuiltinsInEveryPosition()
{
    checkShellOutput(moduleBuiltinsInEveryPosition, { }, R"(which of two callbacks was called by value: 1
its parameter was import.meta: true
the field holds import.meta: true
typeof exports, require, module, module.exports, read from fields: object function object function
the closure, called later: true
)"_s);
}

static void testTerminationDuringImportHandoff()
{
    checkShellOutput(fileThatImportsAheadOfLoader, { }, R"(begins: asks.cjs
termination was pending: true
runs: other.js
another graph: is loaded
runs: late.js
what the file asked for: is loaded
every job: has run
)"_s, R"js(;
drainMicrotasks();
importFirst().then(() => report("the graph", "is loaded"), error => report("the graph", "failed with " + error));
requestTerminationOnResolve("/beside.js");
report("termination was pending", callAndCancelTermination(drainMicrotasks));
importSecond().then(() => report("another graph", "is loaded"));
drainMicrotasks();
importLast().then(() => report("what the file asked for", "is loaded"));
drainMicrotasks();
report("every job", "has run");
)js"_s);
}

static void testImportCalledDuringLoad()
{
    checkShellOutput(importCalledDuringLoad, { }, R"(runs: asks.cjs
runs: root.js
the graph: is loaded
the jobs behind it: have run
runs: late.js
runs: later.js
)"_s, R"js(.then(async () => {
    await registered();
    report("the graph", "is loaded");
    for (let jobs = 0; jobs < 40; ++jobs)
        await null;
    report("the jobs behind it", "have run");
}).catch(error => print("the script threw " + error));
)js"_s);
}

static constexpr ASCIILiteral outputOfTextsInTurns = R"(runs: asks.cjs
runs: late.js
runs: third.js
runs: second.js
runs: first.js
runs: root.js
the graph: is loaded
)"_s;

static void testTextsInTurns()
{
    checkShellOutput(textsInTurns, { }, outputOfTextsInTurns, R"js(.then(async () => {
    await registered();
    report("the graph", "is loaded");
}).catch(error => print("the script threw " + error));
)js"_s);
}

static void testTextsInTurnsWhileOtherJobsWait()
{
    checkShellOutput(textsInTurns, { }, outputOfTextsInTurns, R"js(.then(async () => {
    (async () => {
        for (let jobs = 0; jobs < 40; ++jobs)
            await null;
    })();
    await registered();
    report("the graph", "is loaded");
}).catch(error => print("the script threw " + error));
)js"_s);
}

static void testThrowThroughStartOfModule()
{
    checkShellOutput(moduleThatMayFailWhereItIsRequired, { }, R"(runs: first.js
runs: second.js
runs: third.js
require threw: boom
require threw: boom
)"_s, R"js(.then(() => {
    globalThis.shouldThrow = true;
    for (let attempt = 0; attempt < 2; ++attempt) {
        try {
            registered();
            report("require", "returned");
        } catch (error) {
            report("require threw", error);
        }
    }
}).catch(error => print("the script threw " + error));
)js"_s);
}

static void testStartOfModuleThatHasRun()
{
    checkShellOutput(moduleThatMayFailWhereItIsRequired, { }, R"(runs: first.js
runs: second.js
runs: third.js
runs: late.js
require: returned
require: returned
)"_s, R"js(.then(() => {
    globalThis.shouldThrow = false;
    for (let attempt = 0; attempt < 2; ++attempt) {
        registered();
        report("require", "returned");
    }
}).catch(error => print("the script threw " + error));
)js"_s);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

static std::span<const uint8_t> sectionOfImageWithRows(std::span<const uint8_t> image, ImageSection section)
{
    AOT_TEST_CHECK(image.size() >= sizeof(ImageHeader));
    ImageSectionRange range = spanReinterpretCast<const ImageHeader>(image.first(sizeof(ImageHeader)))[0].sections[static_cast<unsigned>(section)];
    AOT_TEST_CHECK(range.offset <= image.size() && range.size <= image.size() - range.offset);
    return image.subspan(range.offset, range.size);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

static uint32_t operationsInRowsOfStarts(const String& programText)
{
    using Operation = ModuleStartStepData::Operation;
    std::unique_ptr<CompileOutput> output;
    runOnBuildThread([&] {
        output = compileOnThisThread(programText, "references=wide"_s, { });
    });
    std::span<const uint8_t> image = output->section(Wire::ResultSection::Image);
    std::span<const uint8_t> bytesOfTables = sectionOfImageWithRows(image, ImageSection::ProgramTables);
    AOT_TEST_CHECK(bytesOfTables.size() >= sizeof(ProgramTables));
    const ProgramTables& tables = spanReinterpretCast<const ProgramTables>(bytesOfTables.first(sizeof(ProgramTables)))[0];
    size_t numberOfFunctions = spanReinterpretCast<const ImageHeader>(image.first(sizeof(ImageHeader)))[0].counts.functions;
    std::span<const uint32_t> starts = tables.moduleStarts();
    std::span<const ModuleStartStepData> steps = tables.moduleStartSteps();
    AOT_TEST_CHECK(!starts.empty() && !steps.empty() && steps[0].operation() == Operation::Trap);

    uint32_t found = 1u << static_cast<uint32_t>(Operation::Trap);
    for (uint32_t firstStep : starts) {
        if (!firstStep)
            continue;
        for (size_t place = firstStep;; ++place) {
            AOT_TEST_CHECK(place < steps.size());
            ModuleStartStepData step = steps[place];
            AOT_TEST_CHECK(step.operation() != Operation::Trap && step.operation() <= Operation::CallStartUnlessStopped);
            if (step.operation() == Operation::FinishAndReturn)
                found |= 1u << static_cast<uint32_t>(Operation::Finish) | 1u << static_cast<uint32_t>(Operation::Return);
            else if (step.operation() == Operation::CallStartUnlessStopped)
                found |= 1u << static_cast<uint32_t>(Operation::ReturnIfLoadingStops) | 1u << static_cast<uint32_t>(Operation::CallStart);
            else
                found |= 1u << static_cast<uint32_t>(step.operation());
            if (step.callsStart())
                AOT_TEST_CHECK(step.operand() < starts.size() && starts[step.operand()]);
            if (step.operation() == Operation::CallBody)
                AOT_TEST_CHECK(step.operand() < numberOfFunctions);
            if (step.endsRow())
                break;
        }
    }
    return found;
}

static void testEveryKindOfStepOfStart()
{
    using Operation = ModuleStartStepData::Operation;
    auto bitsOf = [](std::initializer_list<Operation> operations) {
        uint32_t bits = 0;
        for (Operation operation : operations)
            bits |= 1u << static_cast<uint32_t>(operation);
        return bits;
    };
    uint32_t whereFilesAreListed = operationsInRowsOfStarts(cacheByLevel);
    uint32_t whereModuleMayFail = operationsInRowsOfStarts(moduleThatMayFailWhereItIsRequired);
    uint32_t onlyWhereFilesAreListed = bitsOf({ Operation::CallStart, Operation::Begin, Operation::ReturnIfLoadingStops });
    uint32_t inBoth = bitsOf({ Operation::Trap, Operation::CallBody, Operation::BeginOrReturn, Operation::Finish, Operation::Return });
    AOT_TEST_CHECK((whereFilesAreListed & onlyWhereFilesAreListed) == onlyWhereFilesAreListed);
    AOT_TEST_CHECK(whereModuleMayFail == (inBoth | bitsOf({ Operation::AbandonOnThrow })));
    AOT_TEST_CHECK((whereFilesAreListed | whereModuleMayFail) == (1u << (static_cast<uint32_t>(Operation::Return) + 1)) - 1);
}

std::span<const NativeTest> moduleProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "module-programs/required-commonjs-modules"_s, testRequiredCommonJSModules },
        { "module-programs/imported-commonjs-modules"_s, testImportedCommonJSModules },
        { "module-programs/commonjs-entry-point"_s, testCommonJSEntryPoint },
        { "module-programs/records-of-require"_s, testRecordsOfRequire },
        { "module-programs/parents-by-level"_s, testParentsByLevel },
        { "module-programs/cache-by-level"_s, testCacheByLevel },
        { "module-programs/nothing-to-run"_s, testNothingToRun },
        { "module-programs/import-meta-kept"_s, testImportMetaKept },
        { "module-programs/import-meta-passed-on"_s, testImportMetaPassedOn },
        { "module-programs/module-builtins-in-every-position"_s, testModuleBuiltinsInEveryPosition },
        { "module-programs/termination-during-import-handoff"_s, testTerminationDuringImportHandoff },
        { "module-programs/import-called-during-load"_s, testImportCalledDuringLoad },
        { "module-programs/texts-in-turns"_s, testTextsInTurns },
        { "module-programs/texts-in-turns-while-other-jobs-wait"_s, testTextsInTurnsWhileOtherJobsWait },
        { "module-programs/throw-through-start-of-module"_s, testThrowThroughStartOfModule },
        { "module-programs/start-of-module-that-has-run"_s, testStartOfModuleThatHasRun },
        { "module-programs/every-kind-of-step-of-start"_s, testEveryKindOfStepOfStart },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
