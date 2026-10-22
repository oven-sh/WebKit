/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTShellProgramTestSupport.h"

namespace JSC { namespace AOT {

// function double(n: number) { return n * 2 } try { print(double("4" as any)) } catch (e) { print(e.name) } print("after")
static constexpr ASCIILiteral onlyFalseArgument = R"IR(module m0 path="main.ts" displayPath="main.ts" body=f0
entry module=m0
typesite w0 module=m0 position=17 kind=Parameter
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = NewClosure function=f1
    Jump -> next b1()
  b1() handler=b2():
    v3 = String string="4"
    v4 = Call v2, -, v3
    v5 = Call v1, -, v4
    Jump -> next b3()
  b2() role=CatchHandler:
    v6 = CatchValue
    v7 = PropGet v6 key="name"
    v8 = Call v1, -, v7
    Jump -> next b3()
  b3():
    v9 = String string="after"
    v10 = Call v1, -, v9
    Return
}
function f1 name="double" parameters=[{type=undefined|null|number site=w0}] results=[{type=anything}] {
  b0(v0: undefined|null|number):
    v1 = Integer integer=2
    v2 = Mul v0, v1
    Return v2
}
)IR"_s;

// The same, and nothing uses what the call returns: try { double("4" as any) } catch (e) { print(e.name) }
static constexpr ASCIILiteral resultUnused = R"IR(module m0 path="main.ts" displayPath="main.ts" body=f0
entry module=m0
typesite w0 module=m0 position=17 kind=Parameter
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = NewClosure function=f1
    Jump -> next b1()
  b1() handler=b2():
    v3 = String string="4"
    v4 = Call v2, -, v3
    v5 = String string="no error"
    v6 = Call v1, -, v5
    Jump -> next b3()
  b2() role=CatchHandler:
    v7 = CatchValue
    v8 = PropGet v7 key="name"
    v9 = Call v1, -, v8
    Jump -> next b3()
  b3():
    Return
}
function f1 name="ignore" parameters=[{type=undefined|null|number site=w0}] results=[{type=anything}] {
  b0(v0: undefined|null|number):
    v1 = Integer integer=1
    Return v1
}
)IR"_s;

// const untyped = id(double); try { print(untyped("4")) } catch (e) { print(e.name) }
static constexpr ASCIILiteral calleeFromUnknownCode = R"IR(module m0 path="main.ts" displayPath="main.ts" body=f0
entry module=m0
typesite w0 module=m0 position=17 kind=Parameter
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = PropGet v0 key="id" [isFreeName]
    v3 = NewClosure function=f1
    v4 = Call v2, -, v3
    Jump -> next b1()
  b1() handler=b2():
    v5 = String string="4"
    v6 = Call v4, -, v5
    v7 = Call v1, -, v6
    Jump -> next b3()
  b2() role=CatchHandler:
    v8 = CatchValue
    v9 = PropGet v8 key="name"
    v10 = Call v1, -, v9
    Jump -> next b3()
  b3():
    Return
}
function f1 name="double" parameters=[{type=undefined|null|number site=w0}] results=[{type=anything}] {
  b0(v0: undefined|null|number):
    v1 = Integer integer=2
    v2 = Mul v0, v1
    Return v2
}
)IR"_s;

// print(double(4)); try { print(double("4" as any)) } catch (e) { print(e.name) } print(double(undefined)); print(double(4.5))
static constexpr ASCIILiteral fittingAndFalseArguments = R"IR(module m0 path="main.ts" displayPath="main.ts" body=f0
entry module=m0
typesite w0 module=m0 position=17 kind=Parameter
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = NewClosure function=f1
    v3 = Integer integer=4
    v4 = Call v2, -, v3
    v5 = Call v1, -, v4
    Jump -> next b1()
  b1() handler=b2():
    v6 = String string="4"
    v7 = Call v2, -, v6
    v8 = Call v1, -, v7
    Jump -> next b3()
  b2() role=CatchHandler:
    v9 = CatchValue
    v10 = PropGet v9 key="name"
    v11 = Call v1, -, v10
    Jump -> next b3()
  b3():
    v12 = Undefined
    v13 = Call v2, -, v12
    v14 = Call v1, -, v13
    v15 = Double number=4.5
    v16 = Call v2, -, v15
    v17 = Call v1, -, v16
    Return
}
function f1 name="double" parameters=[{type=undefined|null|number site=w0}] results=[{type=anything}] {
  b0(v0: undefined|null|number):
    v1 = Integer integer=2
    v2 = Mul v0, v1
    Return v2
}
)IR"_s;

// function show(n: number) { return String(n) } try { print(show("4" as any)) } catch (e) { print(e.name) }
static constexpr ASCIILiteral builtinInDeadBody = R"IR(module m0 path="main.ts" displayPath="main.ts" body=f0
entry module=m0
typesite w0 module=m0 position=15 kind=Parameter
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = NewClosure function=f1
    Jump -> next b1()
  b1() handler=b2():
    v3 = String string="4"
    v4 = Call v2, -, v3
    v5 = Call v1, -, v4
    Jump -> next b3()
  b2() role=CatchHandler:
    v6 = CatchValue
    v7 = PropGet v6 key="name"
    v8 = Call v1, -, v7
    Jump -> next b3()
  b3():
    Return
}
function f1 name="show" parameters=[{type=undefined|null|number site=w0}] results=[{type=anything}] {
  b0(v0: undefined|null|number):
    v1 = Builtin builtin=GlobalThis
    v2 = PropGet v1 key="String" [isFreeName]
    v3 = Call v2, -, v0
    Return v3
}
)IR"_s;

// function shout(this: string) { return this + "!" } try { print(shout.call(5)) } catch (e) { print(e.name) }, the call written with its receiver
static constexpr ASCIILiteral falseReceiver = R"IR(module m0 path="main.ts" displayPath="main.ts" body=f0
entry module=m0
typesite w0 module=m0 position=15 kind=This
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = NewClosure function=f1
    Jump -> next b1()
  b1() handler=b2():
    v3 = Integer integer=5
    v4 = Call v2, v3
    v5 = Call v1, -, v4
    Jump -> next b3()
  b2() role=CatchHandler:
    v6 = CatchValue
    v7 = PropGet v6 key="name"
    v8 = Call v1, -, v7
    Jump -> next b3()
  b3():
    Return
}
function f1 name="shout" flags=[HasThis] thisType={type=undefined|null|string site=w0} results=[{type=anything}] {
  b0(v0: undefined|null|string):
    v1 = String string="!"
    v2 = Add v0, v1
    Return v2
}
)IR"_s;

// function fail() { throw "always" } try { print(String(fail())) } catch (e) { print(e) }
static constexpr ASCIILiteral builtinBehindCallThatThrows = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
  b0():
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="print" [isFreeName]
    v2 = NewClosure function=f1
    Jump -> next b1()
  b1() handler=b2():
    v3 = Call v2
    v4 = PropGet v0 key="String" [isFreeName]
    v5 = Call v4, -, v3
    v6 = Call v1, -, v5
    Jump -> next b3()
  b2() role=CatchHandler:
    v7 = CatchValue
    v8 = Call v1, -, v7
    Jump -> next b3()
  b3():
    Return
}
function f1 name="fail" results=[{type=anything}] {
  b0():
    v0 = String string="always"
    Throw v0
}
)IR"_s;

static void checkUnderEveryOption(ASCIILiteral programText, ASCIILiteral expectedOutput)
{
    static constexpr ASCIILiteral words[] = {
        "validate"_s,
        "heat=EverythingIsHot"_s,
        "heat=EverythingIsCold"_s,
        "disable=flow-results"_s,
        "disable=flow:CalleeSets"_s,
        "disable=flow:NarrowedTypes"_s,
        "disable=pass:ComputeEffectSummaries"_s,
        "disable=pass:ResolveCalls"_s,
        "disable=pass:InlineCalls"_s,
        "disable=pass:EliminateDeadCode"_s,
        "disable=pass:RemoveUnusedParameters"_s,
    };
    checkOutputOfProgramInShell(programText, expectedOutput, { });
    for (ASCIILiteral word : words)
        checkOutputOfProgramInShell(programText, expectedOutput, { word });
}

static void testOnlyFalseArgument()
{
    checkUnderEveryOption(onlyFalseArgument, "8\nafter\n"_s);
}

static void testResultUnused()
{
    checkUnderEveryOption(resultUnused, "no error\n"_s);
}

static void testCalleeFromUnknownCode()
{
    checkUnderEveryOption(calleeFromUnknownCode, "8\n"_s);
}

static void testFittingAndFalseArguments()
{
    checkUnderEveryOption(fittingAndFalseArguments, "8\n8\nNaN\n9\n"_s);
}

static void testBuiltinInDeadBody()
{
    checkUnderEveryOption(builtinInDeadBody, "4\n"_s);
}

static void testFalseReceiver()
{
    checkUnderEveryOption(falseReceiver, "5!\n"_s);
}

static void testBuiltinBehindCallThatThrows()
{
    checkUnderEveryOption(builtinBehindCallThatThrows, "always\n"_s);
}

std::span<const NativeTest> falseTypeProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "false-type-programs/only-a-false-argument-enters"_s, testOnlyFalseArgument },
        { "false-type-programs/result-of-the-call-unused"_s, testResultUnused },
        { "false-type-programs/callee-from-unknown-code"_s, testCalleeFromUnknownCode },
        { "false-type-programs/fitting-and-false-arguments"_s, testFittingAndFalseArguments },
        { "false-type-programs/built-in-in-a-body-nothing-enters"_s, testBuiltinInDeadBody },
        { "false-type-programs/false-receiver"_s, testFalseReceiver },
        { "false-type-programs/built-in-behind-a-call-that-throws"_s, testBuiltinBehindCallThatThrows },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
