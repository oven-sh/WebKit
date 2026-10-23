/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

static constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Index" fields=[{key="first" declared={type=anything} flags=[IsRequired]}]
layout L1 name="User" fields=[{key="id" declared={type=anything} flags=[IsRequired]}, {key="extra" declared={type=anything} flags=[IsRequired]}]
layout L2 kind=Instance name="Point" constructor=f1 fields=[{key="x" declared={type=anything} flags=[IsRequired]}]
layout L3 name="Indexed"
layout L4 name="Other" fields=[{key="inherited" declared={type=anything} flags=[IsRequired]}]
layout L5 kind=Instance name="Parented" constructor=f17 fields=[{key="x" declared={type=anything} flags=[IsRequired]}]
layout L6 kind=Instance name="Static" constructor=f18
layout L7 kind=Instance name="Account" constructor=f25 fields=[{key="id" declared={type=anything} flags=[IsRequired]}, {key="visits" declared={type=anything} flags=[IsRequired]}]
variable g0 name="Point" module=m0 declared={type=anything} initialization=Undefined
variable g1 name="Account" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClass v2 layout=L2
    v4 = NewClosure function=f2
    DefineMethod v3, v4 layout=L2 key="describe"
    v6 = FinishClass v3 layout=L2
    PutModuleVariable v6 variable=g0
    v8 = NewClosure function=f3
    v9 = NewClosure function=f4
    v10 = NewClosure function=f5
    v11 = NewClosure function=f6
    v12 = NewClosure function=f7
    v13 = NewClosure function=f8
    v14 = NewClosure function=f9
    v15 = NewClosure function=f10
    v16 = NewClosure function=f11
    v17 = NewClosure function=f12
    v18 = NewClosure function=f13
    v19 = NewClosure function=f14
    v20 = NewClosure function=f15
    v21 = NewClosure function=f16
    v22 = NewClosure function=f19
    v23 = NewClosure function=f20
    v24 = NewClosure function=f21
    v25 = NewClosure function=f22
    v26 = NewClosure function=f23
    v27 = NewClosure function=f24
    v28 = NewClosure function=f25
    v29 = NewClass v28 layout=L7
    v30 = NewClosure function=f26
    DefineMethod v29, v30 layout=L7 methodKind=Getter key="label"
    v32 = NewClosure function=f27
    DefineMethod v29, v32 layout=L7 methodKind=Setter index=1 key="count"
    v34 = NewClosure function=f28
    DefineMethod v29, v34 layout=L7 index=2 key="describe"
    v36 = FinishClass v29 layout=L7
    PutModuleVariable v36 variable=g1
    v38 = NewClosure function=f29
    v39 = Call v1, -, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17, v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v38
    Return
}
function f1 name="Point" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L2
    v2 = NewObject v1 layout=L2
    v3 = Integer integer=1
    PropSet v2, v3 key="x" mode=Strict
    Return v2
}
function f2 name="describe" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="method of the class"
    Return v1
}
function f3 name="readInherited" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Integer integer=1
    v2 = NewObject -, v1 layout=L0 fields=[0]
    v3 = FinishObject v2
    v4 = String string="u1"
    v5 = Integer integer=7
    v6 = NewObject -, v4, v5 layout=L1 fields=[0, 1]
    v7 = FinishObject v6
    ElemSet v3, v0, v7 mode=Strict
    v9 = PropGet v3 key="extra"
    Return v9
}
function f4 name="readMemberOfInherited" parameters=[{type=string}, {type=anything}, {type=anything}] results=[{type=anything}] {
  b0(v0: string, v1: anything, v2: anything):
    v3 = Integer integer=1
    v4 = NewObject -, v3 layout=L0 fields=[0]
    v5 = FinishObject v4
    v6 = NewObject -, v1, v2 layout=L1 fields=[0, 1]
    v7 = FinishObject v6
    v8 = PropGet v7 key="extra"
    v9 = TypeOf v8
    ElemSet v5, v9, v7 mode=Strict
    ElemSet v5, v0, v7 mode=Strict
    v12 = PropGet v5 key="extra"
    v13 = Undefined
    v14 = StrictEq v12, v13
    Branch v14 -> taken b1(), notTaken b2()
  b1():
    v15 = String string="nothing"
    Return v15
  b2():
    v16 = PropGet v12 key="id"
    Return v16
}
function f5 name="isInstance" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = GetModuleVariable variable=g0
    v2 = Construct v1
    v3 = String string="u1"
    v4 = Integer integer=7
    v5 = NewObject -, v3, v4 layout=L1 fields=[0, 1]
    v6 = FinishObject v5
    ElemSet v2, v0, v6 mode=Strict
    v8 = InstanceOf v2, v1
    Return v8
}
function f6 name="hasInherited" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = GetModuleVariable variable=g0
    v2 = Construct v1
    v3 = String string="u1"
    v4 = Integer integer=7
    v5 = NewObject -, v3, v4 layout=L1 fields=[0, 1]
    v6 = FinishObject v5
    ElemSet v2, v0, v6 mode=Strict
    v8 = PropHas v2 key="extra"
    Return v8
}
function f7 name="typeOfMethod" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = GetModuleVariable variable=g0
    v2 = Construct v1
    v3 = Null
    ElemSet v2, v0, v3 mode=Strict
    v5 = PropGet v2 key="describe"
    v6 = TypeOf v5
    Return v6
}
function f8 name="throughUnknownReceiver" parameters=[{type=string}, {type=anything}] results=[{type=anything}] {
  b0(v0: string, v1: anything):
    v2 = Integer integer=1
    v3 = NewObject -, v2 layout=L0 fields=[0]
    v4 = FinishObject v3
    v5 = String string="u1"
    v6 = Integer integer=7
    v7 = NewObject -, v5, v6 layout=L1 fields=[0, 1]
    v8 = FinishObject v7
    v9 = Call v1, -, v4
    ElemSet v9, v0, v8 mode=Strict
    v11 = PropGet v4 key="extra"
    Return v11
}
function f9 name="byReflectSet" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Integer integer=1
    v2 = NewObject -, v1 layout=L0 fields=[0]
    v3 = FinishObject v2
    v4 = String string="u1"
    v5 = Integer integer=7
    v6 = NewObject -, v4, v5 layout=L1 fields=[0, 1]
    v7 = FinishObject v6
    v8 = Builtin builtin=ReflectSet
    v9 = Call v8, -, v3, v0, v7
    v10 = PropGet v3 key="extra"
    Return v10
}
function f10 name="byAssign" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Integer integer=1
    v2 = NewObject -, v1 layout=L0 fields=[0]
    v3 = FinishObject v2
    v4 = Builtin builtin=JSONParse
    v5 = Call v4, -, v0
    v6 = Builtin builtin=ObjectAssign
    v7 = Call v6, -, v3, v5
    v8 = PropGet v3 key="extra"
    Return v8
}
function f11 name="byLookedUpSetter" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Integer integer=1
    v2 = NewObject -, v1 layout=L0 fields=[0]
    v3 = FinishObject v2
    v4 = String string="u1"
    v5 = Integer integer=7
    v6 = NewObject -, v4, v5 layout=L1 fields=[0, 1]
    v7 = FinishObject v6
    v8 = Builtin builtin=ObjectPrototype__lookupSetter__
    v9 = Call v8, v3, v0
    v10 = Call v9, v3, v7
    v11 = PropGet v3 key="extra"
    Return v11
}
function f12 name="storeNumber" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Integer integer=1
    v2 = NewObject -, v1 layout=L0 fields=[0]
    v3 = FinishObject v2
    v4 = Integer integer=5
    ElemSet v3, v0, v4 mode=Strict
    v6 = Builtin builtin=ObjectGetPrototypeOf
    v7 = Call v6, -, v3
    v8 = Builtin builtin=ObjectPrototype
    v9 = StrictEq v7, v8
    Return v9
}
function f13 name="countKeys" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Integer integer=1
    v2 = NewObject -, v1 layout=L0 fields=[0]
    v3 = FinishObject v2
    v4 = String string="u1"
    v5 = Integer integer=7
    v6 = NewObject -, v4, v5 layout=L1 fields=[0, 1]
    v7 = FinishObject v6
    ElemSet v3, v0, v7 mode=Strict
    v9 = ForInOpen v3
    v10 = Integer integer=0
    Jump -> next b1(v10)
  b1(v11):
    LoopHint
    v13 = ForInStep v9, v3
    v14 = ResultAt v13 index=0
    Branch v14 -> taken b3(), notTaken b2()
  b2():
    v15 = Add v11, v1
    Jump -> next b1(v15)
  b3():
    Return v11
}
function f14 name="mergeFirstKey" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Integer integer=1
    v2 = NewObject -, v1 layout=L0 fields=[0]
    v3 = FinishObject v2
    v4 = Builtin builtin=JSONParse
    v5 = Call v4, -, v0
    v6 = Builtin builtin=ObjectKeys
    v7 = Call v6, -, v5
    v8 = Integer integer=0
    v9 = ElemGet v7, v8
    v10 = ElemGet v5, v9
    ElemSet v3, v9, v10 mode=Strict
    v12 = PropGet v3 key="extra"
    Return v12
}
function f15 name="mergeByForIn" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Integer integer=1
    v2 = NewObject -, v1 layout=L0 fields=[0]
    v3 = FinishObject v2
    v4 = Builtin builtin=JSONParse
    v5 = Call v4, -, v0
    v6 = ForInOpen v5
    Jump -> next b1()
  b1():
    LoopHint
    v8 = ForInStep v6, v5
    v9 = ResultAt v8 index=0
    v10 = ResultAt v8 index=1
    Branch v9 -> taken b3(), notTaken b2()
  b2():
    v11 = ElemGet v5, v10
    ElemSet v3, v10, v11 mode=Strict
    Jump -> next b1()
  b3():
    v13 = PropGet v3 key="extra"
    Return v13
}
function f16 name="keyIsParsed" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Integer integer=1
    v2 = NewObject -, v1 layout=L0 fields=[0]
    v3 = FinishObject v2
    v4 = String string="u1"
    v5 = Integer integer=7
    v6 = NewObject -, v4, v5 layout=L1 fields=[0, 1]
    v7 = FinishObject v6
    v8 = Builtin builtin=JSONParse
    v9 = Call v8, -, v0
    ElemSet v3, v9, v7 mode=Strict
    v11 = PropGet v3 key="extra"
    Return v11
}
function f17 name="Parented" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L5
    v2 = NewObject v1 layout=L5
    v3 = Integer integer=1
    PropSet v2, v3 key="x" mode=Strict
    Return v2
}
function f18 name="Static" kind=BaseConstructor flags=[HasNewTarget] results=[{type=anything}] {
  b0(v0: anything):
    v1 = GetConstructPrototype v0 layout=L6
    v2 = NewObject v1 layout=L6
    Return v2
}
function f19 name="classPrototypeGetsParent" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = NewClosure function=f17
    v2 = NewClass v1 layout=L5
    v3 = FinishClass v2 layout=L5
    v4 = Construct v3
    v5 = Integer integer=7
    v6 = NewObject -, v5 layout=L4 fields=[0]
    v7 = FinishObject v6
    v8 = PropGet v3 key="prototype"
    ElemSet v8, v0, v7 mode=Strict
    v10 = PropGet v4 key="inherited"
    Return v10
}
function f20 name="classObjectGetsParent" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = NewClosure function=f18
    v2 = NewClass v1 layout=L6
    v3 = FinishClass v2 layout=L6
    v4 = Integer integer=7
    v5 = NewObject -, v4 layout=L4 fields=[0]
    v6 = FinishObject v5
    ElemSet v3, v0, v6 mode=Strict
    v8 = PropGet v3 key="inherited"
    Return v8
}
function f21 name="holeOfArray" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Integer integer=1
    v2 = Integer integer=3
    v3 = NewArray v1, v2 contents=anything holes=[1]
    v4 = NewObject layout=L3 fields=[]
    v5 = String string="inherited element"
    ElemSet v4, v1, v5 mode=Strict
    v7 = FinishObject v4
    ElemSet v3, v0, v7 mode=Strict
    v9 = ElemGet v3, v1
    Return v9
}
function f22 name="methodOfArray" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Integer integer=1
    v2 = NewArray v1 contents=anything holes=[]
    v3 = Null
    ElemSet v2, v0, v3 mode=Strict
    v5 = PropGet v2 key="push"
    v6 = TypeOf v5
    Return v6
}
function f23 name="spreadParsed" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Builtin builtin=JSONParse
    v2 = Call v1, -, v0
    v3 = Integer integer=1
    v4 = NewObject -, v3 layout=L0 fields=[0]
    CopyDataProperties v4, v2
    v6 = FinishObject v4
    v7 = Builtin builtin=ObjectGetPrototypeOf
    v8 = Call v7, -, v6
    v9 = Builtin builtin=ObjectPrototype
    v10 = StrictEq v8, v9
    v11 = Builtin builtin=ObjectPrototypeHasOwnProperty
    v12 = String string="__proto__"
    v13 = Call v11, v6, v12
    v14 = PropGet v6 key="extra"
    v15 = NewArray v10, v13, v14 contents=anything holes=[]
    Return v15
}
function f24 name="computedLiteralKey" parameters=[{type=string}] results=[{type=anything}] {
  b0(v0: string):
    v1 = Integer integer=1
    v2 = NewObject -, v1 layout=L0 fields=[0]
    v3 = Null
    ElemDefine v2, v0, v3
    v5 = FinishObject v2
    v6 = Builtin builtin=ObjectGetPrototypeOf
    v7 = Call v6, -, v5
    v8 = Builtin builtin=ObjectPrototype
    v9 = StrictEq v7, v8
    v10 = Builtin builtin=ObjectPrototypeHasOwnProperty
    v11 = Call v10, v5, v0
    v12 = NewArray v9, v11 contents=anything holes=[]
    Return v12
}
function f25 name="Account" kind=BaseConstructor flags=[HasNewTarget] parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = GetConstructPrototype v0 layout=L7
    v3 = NewObject v2 layout=L7
    PropSet v3, v1 key="id" mode=Strict
    v5 = Integer integer=0
    PropSet v3, v5 key="visits" mode=Strict
    Return v3
}
function f26 name="label" kind=Getter flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="id"
    Return v1
}
function f27 name="count" kind=Setter flags=[HasThis] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    PropSet v0, v1 key="visits" mode=Strict
    Return
}
function f28 name="describe" kind=Method flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="visits"
    Return v1
}
function f29 name="accessorsOfStoredClass" parameters=[{type=string}, {type=string}] results=[{type=anything}] {
  b0(v0: string, v1: string):
    v2 = GetModuleVariable variable=g1
    v3 = Construct v2, -, v1
    v4 = Integer integer=1
    v5 = NewObject -, v4 layout=L0 fields=[0]
    v6 = FinishObject v5
    ElemSet v6, v0, v3 mode=Strict
    v8 = PropGet v6 key="label"
    v9 = Integer integer=5
    PropSet v6, v9 key="count" mode=Strict
    v11 = PropGet v3 key="visits"
    v12 = PropGet v6 key="visits"
    v13 = Builtin builtin=ObjectPrototypeHasOwnProperty
    v14 = String string="visits"
    v15 = Call v13, v6, v14
    v16 = PropGet v6 key="describe"
    v17 = TypeOf v16
    v18 = NewArray v8, v11, v12, v15, v17 contents=anything holes=[]
    Return v18
}
)IR"_s;

#define WITH_PUBLISHED_FUNCTIONS(statements) \
    "globalThis.publish = function (readInherited, readMemberOfInherited, isInstance, hasInherited, typeOfMethod, throughUnknownReceiver, byReflectSet, byAssign, " \
    "byLookedUpSetter, storeNumber, countKeys, mergeFirstKey, mergeByForIn, keyIsParsed, classPrototypeGetsParent, classObjectGetsParent, holeOfArray, methodOfArray, " \
    "spreadParsed, computedLiteralKey, accessorsOfStoredClass) { " statements " };"_s

static void checkUnderEveryOption(ASCIILiteral statementsBeforeProgram, ASCIILiteral expectedOutput)
{
    static constexpr ASCIILiteral words[] = {
        "validate"_s,
        "disable=flow-results"_s,
        "references=narrow"_s,
        "heat=EverythingIsHot"_s,
        "heat=EverythingIsCold"_s,
        "disable=pass:ResolveCalls"_s,
        "disable=pass:ResolveAccesses"_s,
        "disable=pass:PlaceTypeTests"_s,
        "disable=pass:RemoveUnreadFields"_s,
        "disable=pass:SinkAllocations"_s,
    };
    checkOutputOfProgramInShell(programText, expectedOutput, { }, statementsBeforeProgram);
    for (ASCIILiteral word : words)
        checkOutputOfProgramInShell(programText, expectedOutput, { word }, statementsBeforeProgram);
}

static void testRecordInheritsFromStoredObject()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(readInherited('__proto__')); print(readInherited('other')); print(readInherited('extra').extra);"), "7\nundefined\n7\n"_s);
}

static void testInheritedValueIsNotTakenForStoredObject()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(readMemberOfInherited('__proto__', 'name', 'a string and no record')); print(readMemberOfInherited('__proto__', 'name', 1234567)); print(readMemberOfInherited('__proto__', 'name', { id: 'other' })); print(readMemberOfInherited('__proto__', 'name', undefined)); print(readMemberOfInherited('other', 'name', 'text')); print(readMemberOfInherited('extra', 'name', 'text'));"), "undefined\nundefined\nother\nnothing\nnothing\nname\n"_s);
}

static void testInstanceLeavesItsClass()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(isInstance('__proto__')); print(isInstance('other')); print(hasInherited('__proto__')); print(hasInherited('other')); print(typeOfMethod('__proto__')); print(typeOfMethod('other')); print(typeOfMethod('describe'));"), "false\ntrue\ntrue\nfalse\nundefined\nfunction\nobject\n"_s);
}

static void testEscapedRecordIsReachedThroughUnknownReceiver()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(throughUnknownReceiver('__proto__', id)); print(throughUnknownReceiver('other', id)); print(throughUnknownReceiver('__proto__', () => ({ })));"), "7\nundefined\nundefined\n"_s);
}

static void testBuiltinsThatStoreByKey()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(byReflectSet('__proto__')); print(byReflectSet('other')); print(byAssign('{\"__proto__\": {\"extra\": 7}}')); print(byAssign('{\"other\": {\"extra\": 7}}')); print(byLookedUpSetter('__proto__'));"), "7\nundefined\n7\nundefined\n7\n"_s);
}

static void testKeysOfParsedInput()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(mergeFirstKey('{\"__proto__\": {\"extra\": 7}}')); print(mergeFirstKey('{\"other\": {\"extra\": 7}}')); print(mergeByForIn('{\"__proto__\": {\"extra\": 7}}')); print(mergeByForIn('{\"other\": {\"extra\": 7}}')); print(keyIsParsed('\"__proto__\"')); print(keyIsParsed('[\"__proto__\"]')); print(keyIsParsed('\"other\"'));"), "7\nundefined\n7\nundefined\n7\n7\nundefined\n"_s);
}

static void testNumberChangesNoPrototype()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(storeNumber('__proto__')); print(storeNumber('other'));"), "true\ntrue\n"_s);
}

static void testForInListsInheritedKeys()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(countKeys('__proto__')); print(countKeys('other')); print(countKeys('first'));"), "3\n2\n1\n"_s);
}

static void testClassesAsReceivers()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(classPrototypeGetsParent('__proto__')); print(classPrototypeGetsParent('other')); print(classObjectGetsParent('__proto__')); print(classObjectGetsParent('other'));"), "7\nundefined\n7\nundefined\n"_s);
}

static void testArraysAsReceivers()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(holeOfArray('__proto__')); print(holeOfArray('other')); print(methodOfArray('__proto__')); print(methodOfArray('other'));"), "inherited element\nundefined\nundefined\nfunction\n"_s);
}

static void testDefinitionsChangeNoPrototype()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("let copy = spreadParsed('{\"__proto__\": {\"extra\": 7}}'); print(copy[0]); print(copy[1]); print(copy[2]); let literal = computedLiteralKey('__proto__'); print(literal[0]); print(literal[1]);"), "true\ntrue\nundefined\ntrue\ntrue\n"_s);
}

static void testAccessorsOfStoredClassGetReceiver()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("for (let key of ['__proto__', 'other']) { let answers = accessorsOfStoredClass(key, 'u1'); for (let index = 0; index < 5; ++index) print(answers[index]); }"), "u1\n0\n5\ntrue\nfunction\nundefined\n0\nundefined\nfalse\nundefined\n"_s);
}

std::span<const NativeTest> prototypeSetByKeyTests()
{
    static constexpr NativeTest tests[] = {
        { "prototype-set-by-key/record-inherits-from-stored-object"_s, testRecordInheritsFromStoredObject },
        { "prototype-set-by-key/inherited-value-is-not-taken-for-stored-object"_s, testInheritedValueIsNotTakenForStoredObject },
        { "prototype-set-by-key/instance-leaves-its-class"_s, testInstanceLeavesItsClass },
        { "prototype-set-by-key/escaped-record-is-reached-through-unknown-receiver"_s, testEscapedRecordIsReachedThroughUnknownReceiver },
        { "prototype-set-by-key/builtins-that-store-by-key"_s, testBuiltinsThatStoreByKey },
        { "prototype-set-by-key/keys-of-parsed-input"_s, testKeysOfParsedInput },
        { "prototype-set-by-key/number-changes-no-prototype"_s, testNumberChangesNoPrototype },
        { "prototype-set-by-key/for-in-lists-inherited-keys"_s, testForInListsInheritedKeys },
        { "prototype-set-by-key/classes-as-receivers"_s, testClassesAsReceivers },
        { "prototype-set-by-key/arrays-as-receivers"_s, testArraysAsReceivers },
        { "prototype-set-by-key/definitions-change-no-prototype"_s, testDefinitionsChangeNoPrototype },
        { "prototype-set-by-key/accessors-of-stored-class-get-receiver"_s, testAccessorsOfStoredClassGetReceiver },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
