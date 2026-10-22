/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

// const parent = { set v(x) { saved = x } }, child = Object.create(parent); child.v = 5; print(saved); print(child.v)
static constexpr ASCIILiteral setterOnPrototype = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Parent" fields=[]
variable g0 name="saved" module=m0 declared={type=anything} initialization=Undefined
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = NewObject layout=L0
    v3 = NewClosure function=f1
    DefineSetter v2, v3 key="v"
    v5 = Builtin builtin=ObjectCreate
    v6 = Call v5, -, v2
    v7 = Integer integer=5
    PropSet v6, v7 key="v"
    v9 = GetModuleVariable variable=g0
    v10 = Call v1, -, v9
    v11 = PropGet v6 key="v"
    v12 = Call v1, -, v11
    Return
}
function f1 name="v" kind=Setter flags=[HasThis] thisType={type=anything} parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    PutModuleVariable v1 variable=g0
    Return
}
)IR"_s;

// const child = Object.create({ valueOf() { return 5 } }); print(child + 1)
static constexpr ASCIILiteral valueOfOnPrototype = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Parent" fields=[{key="valueOf" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewObject -, v2 layout=L0 fields=[0]
    v4 = Builtin builtin=ObjectCreate
    v5 = Call v4, -, v3
    v6 = Integer integer=1
    v7 = Add v5, v6
    v8 = Call v1, -, v7
    Return
}
function f1 name="valueOf" flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=5
    Return v1
}
)IR"_s;

// const parent = { }; parent.toString = function () { return "named" }; print("<" + Object.create(parent) + ">")
static constexpr ASCIILiteral toStringOnPrototype = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Parent" fields=[]
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = NewObject layout=L0
    v3 = NewClosure function=f1
    PropSet v2, v3 key="toString"
    v5 = Builtin builtin=ObjectCreate
    v6 = Call v5, -, v2
    v7 = String string="<"
    v8 = Add v7, v6
    v9 = String string=">"
    v10 = Add v8, v9
    v11 = Call v1, -, v10
    Return
}
function f1 name="toString" flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = String string="named"
    Return v1
}
)IR"_s;

// const made = new Object(); Object.defineProperty(made, "seven", { get() { return 7 } }); print(made.seven)
static constexpr ASCIILiteral getterOfNewObject = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = Builtin builtin=Object
    v3 = Construct v2
    v4 = Convert v3 declared={type=object flags=[IsFromCompiler]}
    v5 = NewClosure function=f1
    DefineGetter v4, v5 key="seven"
    v7 = PropGet v4 key="seven"
    v8 = Call v1, -, v7
    Return
}
function f1 name="seven" kind=Getter flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=7
    Return v1
}
)IR"_s;

// function show(e) { print(e.message); print(e instanceof Error) } show(new TypeError("boom"))
static constexpr ASCIILiteral errorAsArgument = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=TypeError
    v1 = String string="boom"
    v2 = Construct v0, -, v1
    v3 = NewClosure function=f1
    v4 = Call v3, -, v2
    Return
}
function f1 name="show" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = PropGet v0 key="message"
    v4 = Call v2, -, v3
    v5 = Builtin builtin=Error
    v6 = InstanceOf v0, v5
    v7 = Call v2, -, v6
    Return
}
)IR"_s;

// function count(a) { print(a.length) } function outer() { count(arguments) } outer(1, 1)
static constexpr ASCIILiteral argumentsAsArgument = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = NewClosure function=f1
    v1 = Integer integer=1
    v2 = Call v0, -, v1, v1
    Return
}
function f1 name="outer" flags=[HasArgumentCount, HasHiddenRest] length=0 results=[{type=anything}] {
  b0(v0: int[0, 4294967295], v1: Array):
    v2 = CreateUnmappedArguments v0, v1
    v3 = NewClosure function=f2
    v4 = Call v3, -, v2
    Return
}
function f2 name="count" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = PropGet v0 key="length"
    v4 = Call v2, -, v3
    Return
}
)IR"_s;

// function show(x) { print(x.a) } show(3); show(Object.create({ a: 3 }))
static constexpr ASCIILiteral numberOrObject = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Parent" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Integer integer=3
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = Builtin builtin=ObjectCreate
    v3 = Call v2, -, v1
    v4 = NewClosure function=f1
    v5 = Call v4, -, v0
    v6 = Call v4, -, v3
    Return
}
function f1 name="show" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = PropGet v0 key="a"
    v4 = Call v2, -, v3
    Return
}
)IR"_s;

// const holder = { cache: Object.create({ a: 4 }) }, seen = id(holder); print(seen.cache.a); seen.cache = Object.create({ a: 4 }); print(holder.cache.a)
static constexpr ASCIILiteral objectInField = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Parent" fields=[{key="a" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Holder" fields=[{key="cache" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = Integer integer=4
    v3 = NewObject -, v2 layout=L0 fields=[0]
    v4 = Builtin builtin=ObjectCreate
    v5 = Call v4, -, v3
    v6 = NewObject -, v5 layout=L1 fields=[0]
    v7 = PropGet v0 key="id" [isFreeName]
    v8 = Call v7, -, v6
    v9 = PropGet v8 key="cache"
    v10 = PropGet v9 key="a"
    v11 = Call v1, -, v10
    v12 = Call v4, -, v3
    PropSet v8, v12 key="cache"
    v14 = PropGet v6 key="cache"
    v15 = PropGet v14 key="a"
    v16 = Call v1, -, v15
    Return
}
)IR"_s;

// const parent = { }; parent.toJSON = function () { return 1 }; print(JSON.stringify(Object.create(parent)))
static constexpr ASCIILiteral toJSONOnPrototype = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Parent" fields=[]
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = NewObject layout=L0
    v3 = NewClosure function=f1
    PropSet v2, v3 key="toJSON"
    v5 = Builtin builtin=ObjectCreate
    v6 = Call v5, -, v2
    v7 = Builtin builtin=JSONStringify
    v8 = Call v7, -, v6
    v9 = Call v1, -, v8
    Return
}
function f1 name="toJSON" flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = Integer integer=1
    Return v1
}
)IR"_s;

// const child = Object.create({ get double() { return this.n * 2 } }); child.n = 4; print(child.double)
static constexpr ASCIILiteral getterReadsThis = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Parent" fields=[]
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = NewObject layout=L0
    v3 = NewClosure function=f1
    DefineGetter v2, v3 key="double"
    v5 = Builtin builtin=ObjectCreate
    v6 = Call v5, -, v2
    v7 = Integer integer=4
    PropSet v6, v7 key="n"
    v9 = PropGet v6 key="double"
    v10 = Call v1, -, v9
    Return
}
function f1 name="double" kind=Getter flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
  b0(v0: anything):
    v1 = PropGet v0 key="n"
    v2 = Integer integer=2
    v3 = Mul v1, v2
    Return v3
}
)IR"_s;

// function show(n) { print(n + 1) } show(new Number(5))
static constexpr ASCIILiteral numberObjectAsArgument = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=Number
    v1 = Integer integer=5
    v2 = Construct v0, -, v1
    v3 = NewClosure function=f1
    v4 = Call v3, -, v2
    Return
}
function f1 name="show" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = Integer integer=1
    v4 = Add v0, v3
    v5 = Call v2, -, v4
    Return
}
)IR"_s;

// function show(entries) { print(entries.next().done) } show(new Map().entries())
static constexpr ASCIILiteral mapIteratorAsArgument = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=Map
    v1 = Construct v0
    v2 = PropGet v1 key="entries"
    v3 = Call v2, v1 calleeKey="entries"
    v4 = NewClosure function=f1
    v5 = Call v4, -, v3
    Return
}
function f1 name="show" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = PropGet v0 key="next"
    v4 = Call v3, v0 calleeKey="next"
    v5 = PropGet v4 key="done"
    v6 = Call v2, -, v5
    Return
}
)IR"_s;

// function show(e) { print(e.name) } try { throw new TypeError("boom") } catch (e) { show(e) }
static constexpr ASCIILiteral caughtErrorAsArgument = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = NewClosure function=f1
    Jump -> next b1()
  b1() handler=b2():
    v1 = Builtin builtin=TypeError
    v2 = String string="boom"
    v3 = Construct v1, -, v2
    Throw v3
  b2() role=CatchHandler:
    v4 = CatchValue
    v5 = Call v0, -, v4
    Return
}
function f1 name="show" parameters=[{type=anything}] results=[{type=anything}] {
  b0(v0: anything):
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="print" [isFreeName]
    v3 = PropGet v0 key="name"
    v4 = Call v2, -, v3
    Return
}
)IR"_s;

static void checkUnderEveryOption(ASCIILiteral programText, ASCIILiteral expectedOutput)
{
    static constexpr ASCIILiteral words[] = {
        "validate"_s,
        "references=narrow"_s,
        "heat=EverythingIsHot"_s,
        "heat=EverythingIsCold"_s,
        "disable=pass:ResolveCalls"_s,
        "disable=pass:ResolveAccesses"_s,
        "disable=pass:SplitUnions"_s,
        "disable=pass:PlaceTypeTests"_s,
        "disable=pass:InlineCalls"_s,
        "disable=pass:SinkAllocations"_s,
    };
    checkOutputOfProgramInShell(programText, expectedOutput, { });
    for (ASCIILiteral word : words)
        checkOutputOfProgramInShell(programText, expectedOutput, { word });
}

static void testSetterOnPrototype()
{
    checkUnderEveryOption(setterOnPrototype, "5\nundefined\n"_s);
}

static void testValueOfOnPrototype()
{
    checkUnderEveryOption(valueOfOnPrototype, "6\n"_s);
}

static void testToStringOnPrototype()
{
    checkUnderEveryOption(toStringOnPrototype, "<named>\n"_s);
}

static void testGetterOfNewObject()
{
    checkUnderEveryOption(getterOfNewObject, "7\n"_s);
}

static void testErrorAsArgument()
{
    checkUnderEveryOption(errorAsArgument, "boom\ntrue\n"_s);
}

static void testArgumentsAsArgument()
{
    checkUnderEveryOption(argumentsAsArgument, "2\n"_s);
}

static void testNumberOrObject()
{
    checkUnderEveryOption(numberOrObject, "undefined\n3\n"_s);
}

static void testObjectInField()
{
    checkUnderEveryOption(objectInField, "4\n4\n"_s);
}

static void testToJSONOnPrototype()
{
    checkUnderEveryOption(toJSONOnPrototype, "1\n"_s);
}

static void testGetterReadsThis()
{
    checkUnderEveryOption(getterReadsThis, "8\n"_s);
}

static void testNumberObjectAsArgument()
{
    checkUnderEveryOption(numberObjectAsArgument, "6\n"_s);
}

static void testMapIteratorAsArgument()
{
    checkUnderEveryOption(mapIteratorAsArgument, "true\n"_s);
}

static void testCaughtErrorAsArgument()
{
    checkUnderEveryOption(caughtErrorAsArgument, "TypeError\n"_s);
}

std::span<const NativeTest> classWithoutInstanceTestTests()
{
    static constexpr NativeTest tests[] = {
        { "class-without-instance-test/setter-found-by-name"_s, testSetterOnPrototype },
        { "class-without-instance-test/value-of-run-by-addition"_s, testValueOfOnPrototype },
        { "class-without-instance-test/to-string-run-by-concatenation"_s, testToStringOnPrototype },
        { "class-without-instance-test/getter-of-new-object"_s, testGetterOfNewObject },
        { "class-without-instance-test/error-as-argument"_s, testErrorAsArgument },
        { "class-without-instance-test/arguments-object-as-argument"_s, testArgumentsAsArgument },
        { "class-without-instance-test/number-or-object-as-argument"_s, testNumberOrObject },
        { "class-without-instance-test/object-in-a-field-reached-by-name"_s, testObjectInField },
        { "class-without-instance-test/to-json-run-by-native-code"_s, testToJSONOnPrototype },
        { "class-without-instance-test/getter-reads-this"_s, testGetterReadsThis },
        { "class-without-instance-test/number-object-as-argument"_s, testNumberObjectAsArgument },
        { "class-without-instance-test/map-iterator-as-argument"_s, testMapIteratorAsArgument },
        { "class-without-instance-test/caught-error-as-argument"_s, testCaughtErrorAsArgument },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
