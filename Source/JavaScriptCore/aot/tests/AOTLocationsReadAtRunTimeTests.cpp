/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

// const amount = { valueOf() { return 5 }, unused: 1 }; print(amount + 1); print(amount < 6); print(String({ toString() { return "label" } }))
static constexpr ASCIILiteral methodsRunByConversions = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Amount" fields=[{key="valueOf" declared={type=anything} flags=[IsRequired]}, {key="unused" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Label" fields=[{key="toString" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = Integer integer=1
    v3 = NewClosure function=f1
    v4 = NewObject -, v3, v2 layout=L0 fields=[0, 1]
    v5 = Add v4, v2
    v6 = Call v1, -, v5
    v7 = Integer integer=6
    v8 = Less v4, v7
    v9 = Call v1, -, v8
    v10 = NewClosure function=f2
    v11 = NewObject -, v10 layout=L1 fields=[0]
    v12 = ToString v11
    v13 = Call v1, -, v12
    Return
}
function f1 name="valueOf" flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = Integer integer=5
    Return v1
}
function f2 name="toString" flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = String string="label"
    Return v1
}
)IR"_s;

// const amount = { valueOf: () => 5 }; print(amount + 1); const hinted = { [Symbol.toPrimitive]: hint => hint }; print(hinted + 1)
static constexpr ASCIILiteral arrowFunctionsRunByConversions = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Amount" fields=[{key="valueOf" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Hinted" fields=[{key=@@toPrimitive declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = Integer integer=1
    v3 = NewClosure function=f1
    v4 = NewObject -, v3 layout=L0 fields=[0]
    v5 = Add v4, v2
    v6 = Call v1, -, v5
    v7 = NewClosure function=f2
    v8 = NewObject -, v7 layout=L1 fields=[0]
    v9 = Add v8, v2
    v10 = Call v1, -, v9
    Return
}
function f1 name="valueOf" kind=Arrow results=[{type=anything}] {
    v0 = Integer integer=5
    Return v0
}
function f2 name="toPrimitive" kind=Arrow length=1 parameters=[{type=anything}] results=[{type=anything}] {
    Return v0
}
)IR"_s;

// const parent = { m() { return 7 }, unused: 1 }, heir = { x: 1 }; Object.setPrototypeOf(heir, parent); print(heir.m()); print(Object.create(Object.create(parent)).m())
static constexpr ASCIILiteral methodCalledThroughHeirs = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Heir" fields=[{key="x" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Parent" fields=[{key="m" declared={type=anything} flags=[IsRequired]}, {key="unused" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = Integer integer=1
    v3 = NewClosure function=f1
    v4 = NewObject -, v3, v2 layout=L1 fields=[0, 1]
    v5 = NewObject -, v2 layout=L0 fields=[0]
    v6 = Builtin builtin=ObjectSetPrototypeOf
    v7 = Call v6, -, v5, v4
    v8 = PropGet v5 key="m"
    v9 = Call v8, v5 calleeKey="m"
    v10 = Call v1, -, v9
    v11 = Builtin builtin=ObjectCreate
    v12 = Call v11, -, v4
    v13 = Call v11, -, v12
    v14 = PropGet v13 key="m"
    v15 = Call v14, v13 calleeKey="m"
    v16 = Call v1, -, v15
    Return
}
function f1 name="m" results=[{type=anything}] {
    v0 = Integer integer=7
    Return v0
}
)IR"_s;

// const label = { toString() { return "label" } }; print(label.toLocaleString())
static constexpr ASCIILiteral methodCalledByModel = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
module m1 path="models/Objects.ts" displayPath="models/Objects.ts" flags=[IsLibrary]
entry module=m0
layout L0 name="Label" fields=[{key="toString" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewObject -, v2 layout=L0 fields=[0]
    v4 = PropGet v3 key="toLocaleString"
    v5 = Call v4, v3 calleeKey="toLocaleString"
    v6 = Call v1, -, v5
    Return
}
function f1 name="toString" flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = String string="label"
    Return v1
}
function f2 name="toLocaleString" module=m1 kind=Method role=Model flags=[HasThis] enclosingRuntimeFunction=131086 thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="toString"
    v2 = Call v1, v0 calleeKey="toString"
    ReturnCall v2
}
runtimefunction id=131086 function=f2
)IR"_s;

// const parent = { listed: 1 }; for (const key in Object.create(Object.create(parent))) print(key)
static constexpr ASCIILiteral keyListedThroughHeirs = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Parent" fields=[{key="listed" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = Integer integer=1
    v3 = NewObject -, v2 layout=L0 fields=[0]
    v4 = Builtin builtin=ObjectCreate
    v5 = Call v4, -, v3
    v6 = Call v4, -, v5
    v7 = ForInOpen v6
    Jump -> next b1()
  b1():
    LoopHint
    v9 = ForInStep v7, v6
    v10 = ResultAt v9
    v11 = ResultAt v9 index=1
    Branch v10 -> taken b2(), notTaken b3()
  b2():
    Return
  b3():
    v12 = Call v1, -, v11
    Jump -> next b1()
}
)IR"_s;

// function five() { return 5 } function twice() { return five() + five() } print(twice())
static constexpr ASCIILiteral declarationThatIsOnlyCalled = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
variable g0 name="five" module=m0 declared={type=function} initialization=Function function=f1
variable g1 name="twice" module=m0 declared={type=function} initialization=Function function=f2
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = GetModuleVariable variable=g1
    v3 = Call v2 calleeName="twice"
    v4 = Call v1, -, v3
    Return
}
function f1 name="five" results=[{type=anything}] {
    v0 = Integer integer=5
    Return v0
}
function f2 name="twice" results=[{type=anything}] {
    v0 = GetModuleVariable variable=g0
    v1 = Call v0 calleeName="five"
    v2 = GetModuleVariable variable=g0
    v3 = Call v2 calleeName="five"
    v4 = Add v1, v3
    Return v4
}
)IR"_s;

// library.js: export function five() { return 5 }. main.js: import * as library from "library.js"; print(Object.create(library).five())
static constexpr ASCIILiteral exportCalledThroughHeirOfNamespace = R"IR(module m0 path="library.js" displayPath="library.js" body=f1 exports=[{name="five" variable=g0}]
module m1 path="main.js" displayPath="main.js" body=f0 requests=[{target=m0 completes=true}]
entry module=m1
variable g0 name="five" module=m0 declared={type=function} initialization=Function function=f2 flags=[IsImmutable]
function f0 name="main" module=m1 kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = ModuleNamespace module=m0
    v3 = Builtin builtin=ObjectCreate
    v4 = Call v3, -, v2
    v5 = PropGet v4 key="five"
    v6 = Call v5, v4 calleeKey="five"
    v7 = Call v1, -, v6
    Return
}
function f1 name="library" module=m0 kind=Module results=[{type=anything}] {
    Return
}
function f2 name="five" module=m0 results=[{type=anything}] {
    v0 = Integer integer=5
    Return v0
}
)IR"_s;

// print(JSON.parse("1", (key, value) => 5)); print(JSON.stringify(1, (key, value) => 2))
static constexpr ASCIILiteral callbacksOfNativeCode = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = NewClosure function=f1
    v3 = Builtin builtin=JSONParse
    v4 = String string="1"
    v5 = Call v3, -, v4, v2
    v6 = Call v1, -, v5
    v7 = NewClosure function=f2
    v8 = Builtin builtin=JSONStringify
    v9 = Integer integer=1
    v10 = Call v8, -, v9, v7
    v11 = Call v1, -, v10
    Return
}
function f1 name="reviver" kind=Arrow length=2 parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Integer integer=5
    Return v2
}
function f2 name="replacer" kind=Arrow length=2 parameters=[{type=anything}, {type=anything}] results=[{type=anything}] {
    v2 = Integer integer=2
    Return v2
}
)IR"_s;

// const holder = { m: () => 8 }; function run(x) { return x.m() } print(run(holder), run(outside)). The last function makes the program large enough for a second version of run.
static constexpr ASCIILiteral methodCalledInGenericVersion = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Holder" fields=[{key="m" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f2
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = NewClosure function=f1
    v3 = Call v2, -, v1
    v4 = Builtin builtin=GlobalThis
    v5 = PropGet v4 key="outside" [isFreeName]
    v6 = Call v2, -, v5
    v7 = PropGet v4 key="print" [isFreeName]
    v8 = Call v7, -, v3, v6
    v9 = CallDirect function=f3
    Return
}
function f1 name="run" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="m"
    v2 = Call v1, v0 calleeKey="m"
    Return v2
}
function f2 name="m" kind=Arrow results=[{type=anything}] {
    v0 = Integer integer=8
    Return v0
}
function f3 name="padding" results=[{type=anything}] {
    v0 = Undefined
    v1 = Undefined
    v2 = Undefined
    v3 = Undefined
    v4 = Undefined
    v5 = Undefined
    v6 = Undefined
    v7 = Undefined
    v8 = Undefined
    v9 = Undefined
    v10 = Undefined
    v11 = Undefined
    v12 = Undefined
    v13 = Undefined
    v14 = Undefined
    v15 = Undefined
    v16 = Undefined
    v17 = Undefined
    v18 = Undefined
    v19 = Undefined
    v20 = Undefined
    v21 = Undefined
    v22 = Undefined
    v23 = Undefined
    v24 = Undefined
    v25 = Undefined
    v26 = Undefined
    v27 = Undefined
    v28 = Undefined
    v29 = Undefined
    Return
}
)IR"_s;

static void checkUnderEveryOption(ASCIILiteral programText, ASCIILiteral expectedOutput, ASCIILiteral statementsBeforeProgram = ""_s)
{
    static constexpr ASCIILiteral words[] = {
        "validate"_s,
        "heat=EverythingIsHot"_s,
        "heat=EverythingIsCold"_s,
        "only-passes=AnalyzeFlow"_s,
        "only-passes=RemoveUnreadFields"_s,
        "only-passes=ResolveCalls,RemoveUnreadFields"_s,
        "disable=pass:RemoveUnreadFields"_s,
        "disable=pass:ResolveCalls"_s,
        "disable=pass:ResolveAccesses"_s,
        "disable=pass:EliminateDeadCode"_s,
        "disable=pass:InlineCalls"_s,
        "disable=pass:SinkAllocations"_s,
        "disable=pass:AllocateInFrames"_s,
        "disable=pass:DeferClosuresInFields"_s,
        "disable=flow:ObjectUses"_s,
        "disable=flow:CalleeSets"_s,
        "disable=flow:ReceiverSets"_s,
    };
    checkOutputOfProgramInShell(programText, expectedOutput, { }, statementsBeforeProgram);
    for (ASCIILiteral word : words)
        checkOutputOfProgramInShell(programText, expectedOutput, { word }, statementsBeforeProgram);
}

static void testMethodsRunByConversions()
{
    checkUnderEveryOption(methodsRunByConversions, "6\ntrue\nlabel\n"_s);
}

static void testArrowFunctionsRunByConversions()
{
    checkUnderEveryOption(arrowFunctionsRunByConversions, "6\ndefault1\n"_s);
}

static void testMethodCalledThroughHeirs()
{
    checkUnderEveryOption(methodCalledThroughHeirs, "7\n7\n"_s);
}

static void testMethodCalledByModel()
{
    checkUnderEveryOption(methodCalledByModel, "label\n"_s);
}

static void testKeyListedThroughHeirs()
{
    checkUnderEveryOption(keyListedThroughHeirs, "listed\n"_s);
}

static void testDeclarationThatIsOnlyCalled()
{
    checkUnderEveryOption(declarationThatIsOnlyCalled, "10\n"_s);
}

static void testExportCalledThroughHeirOfNamespace()
{
    checkUnderEveryOption(exportCalledThroughHeirOfNamespace, "5\n"_s);
}

static void testMethodCalledInGenericVersion()
{
    constexpr ASCIILiteral defineOutside = "globalThis.outside = { m() { return 9; } };"_s;
    checkUnderEveryOption(methodCalledInGenericVersion, "8 9\n"_s, defineOutside);
    checkOutputOfProgramInShell(methodCalledInGenericVersion, "8 9\n"_s, { "disable=flow:CalleeSets,pass:InlineCalls"_s }, defineOutside);
    checkOutputOfProgramInShell(methodCalledInGenericVersion, "8 9\n"_s, { "disable=flow:CalleeSets"_s, "limit=MaxInlinedCallerBytes:0"_s }, defineOutside);
}

static void testCallbacksOfNativeCode()
{
    checkUnderEveryOption(callbacksOfNativeCode, "5\n2\n"_s);
}

std::span<const NativeTest> locationsReadAtRunTimeTests()
{
    static constexpr NativeTest tests[] = {
        { "locations-read-at-run-time/methods-run-by-conversions"_s, testMethodsRunByConversions },
        { "locations-read-at-run-time/arrow-functions-run-by-conversions"_s, testArrowFunctionsRunByConversions },
        { "locations-read-at-run-time/method-called-through-heirs"_s, testMethodCalledThroughHeirs },
        { "locations-read-at-run-time/method-called-by-model"_s, testMethodCalledByModel },
        { "locations-read-at-run-time/key-listed-through-heirs"_s, testKeyListedThroughHeirs },
        { "locations-read-at-run-time/declaration-that-is-only-called"_s, testDeclarationThatIsOnlyCalled },
        { "locations-read-at-run-time/export-called-through-heir-of-namespace"_s, testExportCalledThroughHeirOfNamespace },
        { "locations-read-at-run-time/method-called-in-generic-version"_s, testMethodCalledInGenericVersion },
        { "locations-read-at-run-time/callbacks-of-native-code"_s, testCallbacksOfNativeCode },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
