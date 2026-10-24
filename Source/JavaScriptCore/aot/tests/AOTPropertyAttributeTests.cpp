/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

// Each function has a holder layout of its own, so that what one definition costs a field does not hide what another one fails to cost.
static constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="ValueOnly" fields=[{key="value" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Descriptor" fields=[{key="value" declared={type=anything} flags=[IsRequired]}, {key="writable" declared={type=anything} flags=[IsRequired]}, {key="enumerable" declared={type=anything} flags=[IsRequired]}, {key="configurable" declared={type=anything} flags=[IsRequired]}]
layout L2 name="WritableOnly" fields=[{key="writable" declared={type=anything} flags=[IsRequired]}]
layout L3 name="DescriptorMap" fields=[{key="q" declared={type=anything} flags=[IsRequired]}, {key="p" declared={type=anything} flags=[IsRequired]}]
layout L4 name="ReadOnlyHolder" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
layout L5 name="SloppyHolder" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
layout L6 name="HiddenHolder" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
layout L7 name="FixedHolder" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
layout L8 name="PlainHolder" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
layout L9 name="MapHolder" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
layout L10 name="ReflectHolder" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
layout L11 name="DeclaredHolder" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
layout L12 kind=Instance name="Emitter" constructor=f9 fields=[{key="count" declared={type=anything} flags=[IsRequired]}]
layout L13 name="StackHolder" fields=[{key="message" declared={type=anything} flags=[IsRequired]}]
layout L14 name="ChangedLaterHolder" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
layout L15 name="ParameterHolder" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
layout L16 name="Events"
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = NewClosure function=f4
    v6 = NewClosure function=f5
    v7 = NewClosure function=f6
    v8 = NewClosure function=f7
    v9 = NewClosure function=f8
    v10 = NewClosure function=f10
    v11 = NewClosure function=f11
    v12 = NewClosure function=f12
    v13 = NewClosure function=f13
    v14 = Call v1, -, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13
    Return
}
function f1 name="storeToReadOnlyName" results=[{type=anything}] {
  b0():
    v0 = Integer integer=1
    v1 = NewObject -, v0 layout=L4 fields=[0]
    v2 = NewObject -, v0 layout=L0 fields=[0]
    v3 = Builtin builtin=ObjectDefineProperty
    v4 = String string="hidden"
    v5 = Call v3, -, v1, v4, v2
    Jump -> next b1()
  b1() handler=b3():
    v6 = Integer integer=5
    PropSet v1, v6 key="hidden"
    Jump -> next b2()
  b2():
    v8 = String string="stored"
    Return v8
  b3() role=CatchHandler:
    v9 = CatchValue
    v10 = PropGet v1 key="hidden"
    Return v10
}
function f2 name="storeToReadOnlyNameInSloppyMode" results=[{type=anything}] {
  b0():
    v0 = Integer integer=1
    v1 = NewObject -, v0 layout=L5 fields=[0]
    v2 = NewObject -, v0 layout=L0 fields=[0]
    v3 = Builtin builtin=ObjectDefineProperty
    v4 = String string="hidden"
    v5 = Call v3, -, v1, v4, v2
    v6 = Integer integer=5
    PropSet v1, v6 key="hidden" mode=Sloppy
    v8 = PropGet v1 key="hidden"
    Return v8
}
function f3 name="storeToHiddenName" results=[{type=anything}] {
  b0():
    v0 = Integer integer=1
    v1 = NewObject -, v0 layout=L6 fields=[0]
    v2 = Boolean boolean=true
    v3 = Boolean
    v4 = NewObject -, v0, v2, v3, v2 layout=L1 fields=[0, 1, 2, 3]
    v5 = Builtin builtin=ObjectDefineProperty
    v6 = String string="hidden"
    v7 = Call v5, -, v1, v6, v4
    v8 = Integer integer=5
    PropSet v1, v8 key="hidden"
    v10 = PropGet v1 key="hidden"
    v11 = Builtin builtin=ObjectKeys
    v12 = Call v11, -, v1
    v13 = PropGet v12 key="length"
    v14 = Builtin builtin=JSONStringify
    v15 = Call v14, -, v1
    v16 = NewArray v10, v13, v15 contents=anything
    Return v16
}
function f4 name="deleteFixedName" results=[{type=anything}] {
  b0():
    v0 = Integer integer=1
    v1 = NewObject -, v0 layout=L7 fields=[0]
    v2 = Boolean boolean=true
    v3 = Boolean
    v4 = NewObject -, v0, v2, v2, v3 layout=L1 fields=[0, 1, 2, 3]
    v5 = Builtin builtin=ObjectDefineProperty
    v6 = String string="fixed"
    v7 = Call v5, -, v1, v6, v4
    v8 = PropDelete v1 key="fixed" mode=Sloppy
    v9 = PropGet v1 key="fixed"
    v10 = NewArray v8, v9 contents=anything
    Return v10
}
function f5 name="usePlainName" results=[{type=anything}] {
  b0():
    v0 = Integer integer=1
    v1 = NewObject -, v0 layout=L8 fields=[0]
    v2 = Boolean boolean=true
    v3 = NewObject -, v0, v2, v2, v2 layout=L1 fields=[0, 1, 2, 3]
    v4 = Builtin builtin=ObjectDefineProperty
    v5 = String string="plain"
    v6 = Call v4, -, v1, v5, v3
    v7 = Integer integer=5
    PropSet v1, v7 key="plain"
    v9 = PropGet v1 key="plain"
    v10 = Builtin builtin=JSONStringify
    v11 = Call v10, -, v1
    v12 = PropDelete v1 key="plain"
    v13 = PropHas v1 key="plain"
    v14 = NewArray v9, v11, v12, v13 contents=anything
    Return v14
}
function f6 name="readNamesOfMap" results=[{type=anything}] {
  b0():
    v0 = Integer integer=1
    v1 = NewObject -, v0 layout=L9 fields=[0]
    v2 = NewObject -, v0 layout=L0 fields=[0]
    v3 = String string="p text"
    v4 = NewObject -, v3 layout=L0 fields=[0]
    v5 = NewObject -, v2, v4 layout=L3 fields=[0, 1]
    v6 = Builtin builtin=ObjectDefineProperties
    v7 = Call v6, -, v1, v5
    v8 = PropGet v1 key="p"
    v9 = TypeOf v8
    v10 = PropGet v1 key="q"
    v11 = NewArray v9, v10 contents=anything
    Return v11
}
function f7 name="storeToNameDefinedByReflect" results=[{type=anything}] {
  b0():
    v0 = Integer integer=1
    v1 = NewObject -, v0 layout=L10 fields=[0]
    v2 = NewObject -, v0 layout=L0 fields=[0]
    v3 = Builtin builtin=ReflectDefineProperty
    v4 = String string="hidden"
    v5 = Call v3, -, v1, v4, v2
    Jump -> next b1()
  b1() handler=b3():
    v6 = Integer integer=5
    PropSet v1, v6 key="hidden"
    Jump -> next b2()
  b2():
    v8 = String string="stored"
    Return v8
  b3() role=CatchHandler:
    v9 = CatchValue
    v10 = PropGet v1 key="hidden"
    Return v10
}
function f8 name="storeToReadOnlyField" results=[{type=anything}] {
  b0():
    v0 = Integer integer=1
    v1 = NewObject -, v0 layout=L11 fields=[0]
    v2 = Boolean
    v3 = NewObject -, v2 layout=L2 fields=[0]
    v4 = Builtin builtin=ObjectDefineProperty
    v5 = String string="a"
    v6 = Call v4, -, v1, v5, v3
    Jump -> next b1()
  b1() handler=b3():
    v7 = Integer integer=5
    PropSet v1, v7 key="a"
    Jump -> next b2()
  b2():
    v9 = String string="stored"
    Return v9
  b3() role=CatchHandler:
    v10 = CatchValue
    v11 = PropGet v1 key="a"
    Return v11
}
function f9 name="Emitter" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L12
    v2 = NewObject v1 layout=L12
    v3 = NewObject layout=L16 fields=[]
    v4 = Boolean boolean=true
    v5 = Boolean
    v6 = NewObject -, v3, v4, v5, v5 layout=L1 fields=[0, 1, 2, 3]
    v7 = Builtin builtin=ObjectDefineProperty
    v8 = String string="_events"
    v9 = Call v7, -, v2, v8, v6
    v10 = Integer
    PropSet v2, v10 key="count"
    Return v2
}
function f10 name="replaceHiddenNameOfInstance" results=[{type=anything}] {
  b0():
    v0 = NewClosure function=f9
    v1 = NewClass v0 layout=L12
    v2 = FinishClass v1 layout=L12
    v3 = Construct v2
    v4 = PropGet v3 key="_events"
    v5 = TypeOf v4
    v6 = Integer integer=7
    PropSet v3, v6 key="_events"
    v8 = PropGet v3 key="_events"
    v9 = Builtin builtin=JSONStringify
    v10 = Call v9, -, v3
    v11 = NewArray v5, v8, v10 contents=anything
    Return v11
}
function f11 name="captureStack" results=[{type=anything}] {
  b0():
    v0 = String string="boom"
    v1 = NewObject -, v0 layout=L13 fields=[0]
    v2 = Builtin builtin=ErrorCaptureStackTrace
    v3 = Call v2, -, v1
    v4 = PropGet v1 key="stack"
    v5 = TypeOf v4
    v6 = Builtin builtin=JSONStringify
    v7 = Call v6, -, v1
    v8 = Integer integer=7
    PropSet v1, v8 key="stack"
    v10 = PropGet v1 key="stack"
    v11 = Call v6, -, v1
    v12 = NewArray v5, v7, v10, v11 contents=anything
    Return v12
}
function f12 name="changeDescriptorBetweenDefinitions" results=[{type=anything}] {
  b0():
    v0 = Integer integer=1
    v1 = Boolean boolean=true
    v2 = NewObject -, v0, v1, v1, v1 layout=L1 fields=[0, 1, 2, 3]
    v3 = Builtin builtin=ObjectDefineProperty
    v4 = String string="x"
    v5 = NewObject -, v0 layout=L14 fields=[0]
    v6 = Call v3, -, v5, v4, v2
    v7 = Boolean
    PropSet v2, v7 key="writable"
    v9 = NewObject -, v0 layout=L14 fields=[0]
    v10 = Call v3, -, v9, v4, v2
    v11 = Integer integer=5
    PropSet v5, v11 key="x" mode=Sloppy
    PropSet v9, v11 key="x" mode=Sloppy
    v14 = PropGet v5 key="x"
    v15 = PropGet v9 key="x"
    v16 = NewArray v14, v15 contents=anything
    Return v16
}
function f13 name="defineWithAttributes" parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything, v2: anything):
    v3 = Integer integer=1
    v4 = NewObject -, v3 layout=L15 fields=[0]
    v5 = NewObject -, v3, v0, v1, v2 layout=L1 fields=[0, 1, 2, 3]
    v6 = Builtin builtin=ObjectDefineProperty
    v7 = String string="x"
    v8 = Call v6, -, v4, v7, v5
    v9 = Integer integer=5
    PropSet v4, v9 key="x" mode=Sloppy
    v11 = PropGet v4 key="x"
    v12 = Builtin builtin=JSONStringify
    v13 = Call v12, -, v4
    v14 = PropDelete v4 key="x" mode=Sloppy
    v15 = NewArray v11, v13, v14 contents=anything
    Return v15
}
)IR"_s;

#define WITH_PUBLISHED_FUNCTIONS(statements) \
    "globalThis.publish = function (storeToReadOnlyName, storeToReadOnlyNameInSloppyMode, storeToHiddenName, deleteFixedName, usePlainName, readNamesOfMap, " \
    "storeToNameDefinedByReflect, storeToReadOnlyField, replaceHiddenNameOfInstance, captureStack, changeDescriptorBetweenDefinitions, defineWithAttributes) { " statements " };"_s

static void checkUnderEveryOption(ASCIILiteral statementsBeforeProgram, ASCIILiteral expectedOutput)
{
    static constexpr ASCIILiteral words[] = {
        "validate"_s,
        "disable=flow-results"_s,
        "disable=flow:AppendedFields"_s,
        "disable=flow:HolderMutations"_s,
        "references=narrow"_s,
        "heat=EverythingIsHot"_s,
        "heat=EverythingIsCold"_s,
        "disable=pass:ResolveAccesses"_s,
        "disable=pass:SinkAllocations"_s,
    };
    checkOutputOfProgramInShell(programText, expectedOutput, { }, statementsBeforeProgram);
    for (ASCIILiteral word : words)
        checkOutputOfProgramInShell(programText, expectedOutput, { word }, statementsBeforeProgram);
}

static void testReadOnlyNameRefusesStores()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(storeToReadOnlyName()); print(storeToReadOnlyNameInSloppyMode()); print(storeToNameDefinedByReflect()); print(storeToReadOnlyField());"), "1\n1\n1\n1\n"_s);
}

static void testHiddenNameIsStoredAndNotListed()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(storeToHiddenName()); print(replaceHiddenNameOfInstance());"), "5,1,{\"a\":1}\nobject,7,{\"count\":0}\n"_s);
}

static void testFixedNameIsNotDeleted()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(deleteFixedName());"), "false,1\n"_s);
}

static void testPlainNameIsField()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(usePlainName());"), "5,{\"a\":1,\"plain\":5},true,false\n"_s);
}

static void testNamesOfMapKeepTheirValues()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(readNamesOfMap());"), "string,1\n"_s);
}

static void testNativeDefinitionHidesName()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(captureStack());"), "string,{\"message\":\"boom\"},7,{\"message\":\"boom\"}\n"_s);
}

static void testAttributesKnownOnlyAtRunTime()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(changeDescriptorBetweenDefinitions()); for (let writable of [true, false]) { for (let enumerable of [true, false]) { for (let configurable of [true, false]) print(defineWithAttributes(writable, enumerable, configurable)); } }"),
        "5,1\n"
        "5,{\"a\":1,\"x\":5},true\n"
        "5,{\"a\":1,\"x\":5},false\n"
        "5,{\"a\":1},true\n"
        "5,{\"a\":1},false\n"
        "1,{\"a\":1,\"x\":1},true\n"
        "1,{\"a\":1,\"x\":1},false\n"
        "1,{\"a\":1},true\n"
        "1,{\"a\":1},false\n"_s);
}

std::span<const NativeTest> propertyAttributeTests()
{
    static constexpr NativeTest tests[] = {
        { "property-attributes/read-only-name-refuses-stores"_s, testReadOnlyNameRefusesStores },
        { "property-attributes/hidden-name-is-stored-and-not-listed"_s, testHiddenNameIsStoredAndNotListed },
        { "property-attributes/fixed-name-is-not-deleted"_s, testFixedNameIsNotDeleted },
        { "property-attributes/plain-name-is-field"_s, testPlainNameIsField },
        { "property-attributes/names-of-map-keep-their-values"_s, testNamesOfMapKeepTheirValues },
        { "property-attributes/native-definition-hides-name"_s, testNativeDefinitionHidesName },
        { "property-attributes/attributes-known-only-at-run-time"_s, testAttributesKnownOnlyAtRunTime },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
