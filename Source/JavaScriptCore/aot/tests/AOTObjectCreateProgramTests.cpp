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
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = NewClosure function=f4
    v6 = Call v1, -, v2, v3, v4, v5
    Return
}
function f1 name="withoutPrototype" results=[{type=anything}] {
    v0 = Builtin builtin=ObjectCreate
    v1 = Null
    v2 = Call v0, -, v1
    Return v2
}
function f2 name="filledWithoutPrototype" parameters=[{type=anything}, {type=anything}, {type=anything}] results=[{type=anything}] {
    v3 = Builtin builtin=ObjectCreate
    v4 = Null
    v5 = Call v3, -, v4
    PropSet v5, v0 key="first" mode=Strict
    PropSet v5, v1 key="second" mode=Strict
    PropSet v5, v2 key="third" mode=Strict
    Return v5
}
function f3 name="underObjectOrNull" parameters=[{type=null|object}] results=[{type=anything}] {
    v1 = Builtin builtin=ObjectCreate
    v2 = Call v1, -, v0
    Return v2
}
function f4 name="underAnything" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=ObjectCreate
    v2 = Call v1, -, v0
    Return v2
}
)IR"_s;

#define WITH_PUBLISHED_FUNCTIONS(statements) \
    "globalThis.describe = object => (Object.getPrototypeOf(object) === null) + ' ' + Object.keys(object).join() + ' ' + JSON.stringify(object);" \
    "globalThis.publish = function (withoutPrototype, filledWithoutPrototype, underObjectOrNull, underAnything) { " statements " };"_s

static constexpr ASCIILiteral optionWords[] = {
    "validate"_s,
    "disable=flow-results"_s,
    "references=narrow"_s,
    "heat=EverythingIsHot"_s,
    "heat=EverythingIsCold"_s,
    "disable=rule:PropertyRun"_s,
    "disable=pass:SinkAllocations"_s,
};

static void checkUnderEveryOption(ASCIILiteral statementsBeforeProgram, ASCIILiteral expectedOutput)
{
    checkOutputOfProgramInShell(programText, expectedOutput, { }, statementsBeforeProgram);
    for (ASCIILiteral word : optionWords)
        checkOutputOfProgramInShell(programText, expectedOutput, { word }, statementsBeforeProgram);
}

static void testNullGivesObjectWithoutPrototype()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("let object = withoutPrototype(); print(describe(object)); print('toString' in object); print(object === withoutPrototype()); object.late = 1; print(describe(object)); print(describe(withoutPrototype()));"),
        "true  {}\nfalse\nfalse\ntrue late {\"late\":1}\ntrue  {}\n"_s);
}

static void testStoresFollowTheCreation()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("print(describe(filledWithoutPrototype(1, 'two', null))); print(describe(filledWithoutPrototype(0.5, [1], { a: 1 }))); let object = filledWithoutPrototype(1, 2, 3); object.fourth = 4; delete object.second; print(describe(object)); print(describe(filledWithoutPrototype(7, 8, 9)));"),
        "true first,second,third {\"first\":1,\"second\":\"two\",\"third\":null}\ntrue first,second,third {\"first\":0.5,\"second\":[1],\"third\":{\"a\":1}}\ntrue first,third,fourth {\"first\":1,\"third\":3,\"fourth\":4}\ntrue first,second,third {\"first\":7,\"second\":8,\"third\":9}\n"_s);
}

static void testObjectAndNullAlternate()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("let base = { inherited: 'yes' }; for (let create of [underObjectOrNull, underAnything]) { for (let prototype of [null, base, null, Array.prototype, base, null]) { let object = create(prototype); print((Object.getPrototypeOf(object) === prototype) + ' ' + object.inherited + ' ' + Object.keys(object).length); } }"),
        "true undefined 0\ntrue yes 0\ntrue undefined 0\ntrue undefined 0\ntrue yes 0\ntrue undefined 0\ntrue undefined 0\ntrue yes 0\ntrue undefined 0\ntrue undefined 0\ntrue yes 0\ntrue undefined 0\n"_s);
}

static void testOtherValuesThrow()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("for (let wrong of [undefined, 1, 'text', true, Symbol.iterator]) { try { underAnything(wrong); print('no error'); } catch (error) { print(error instanceof TypeError); } }"),
        "true\ntrue\ntrue\ntrue\ntrue\n"_s);
}

// const members = { get size() { return this.items } }   function g() { return tick() }   function make(proto) { const made = Object.create(proto); made.items = g(); return made }
// report(make(members).size)   try { make(1); report("no error") } catch (error) { report(error.message) }
// Nobody outside sees make(): what it creates is an object of a layout, and the allocation lists what g() answers.
static constexpr ASCIILiteral closedProgramText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 fields=[{key="size" declared={type=anything} flags=[IsRequired]}]
variable g0 name="g" module=m0 declared={type=anything} flags=[IsImmutable]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = NewClosure function=f1
    PutModuleVariable v0 variable=g0
    v2 = NewClosure function=f2
    v3 = NewObject layout=L0
    v4 = NewClosure function=f3
    DefineGetter v3, v4 key="size"
    v6 = FinishObject v3
    v7 = Builtin builtin=GlobalThis
    v8 = PropGet v7 key="report" [isFreeName]
    v9 = Call v2, -, v6
    v10 = PropGet v9 key="size"
    v11 = Call v8, -, v10
    Jump -> next b1()
  b1() handler=b3():
    v12 = Integer integer=1
    v13 = Call v2, -, v12
    v14 = String string="no error"
    v15 = Call v8, -, v14
    Jump -> next b2()
  b2():
    Return
  b3() role=CatchHandler:
    v16 = CatchValue
    v17 = PropGet v16 key="message"
    v18 = Call v8, -, v17
    Jump -> next b2()
}
function f1 name="g" results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="tick" [isFreeName]
    v2 = Call v1, -
    Return v2
}
function f2 name="make" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=Object
    v2 = Builtin builtin=ObjectCreate
    v3 = Call v2, v1, v0
    v4 = GetModuleVariable variable=g0
    v5 = Call v4, -
    PropSet v3, v5 key="items"
    Return v3
}
function f3 name="size" kind=Getter flags=[HasThis] thisType={type=anything} results=[{type=anything}] {
    v1 = PropGet v0 key="items"
    Return v1
}
)IR"_s;

// The built-in refuses its argument before it makes anything, so nothing that the text has behind the call has run when the error is thrown.
static void testOtherValuesThrowBeforeWhatFollowsTheCreation()
{
    static constexpr ASCIILiteral wordsOfPasses[] = {
        "disable=pass:InlineCalls"_s,
        "disable=pass:SimplifyCasts"_s,
        "disable=pass:EliminateRedundantReads"_s,
    };
    ASCIILiteral statements = "globalThis.ticks = 0; globalThis.tick = () => ++ticks; globalThis.report = text => print(text, ticks);"_s;
    ASCIILiteral expectedOutput = "1 1\nObject prototype may only be an Object or null. 1\n"_s;
    checkOutputOfProgramInShell(closedProgramText, expectedOutput, { }, statements);
    for (ASCIILiteral word : optionWords)
        checkOutputOfProgramInShell(closedProgramText, expectedOutput, { word }, statements);
    for (ASCIILiteral word : wordsOfPasses)
        checkOutputOfProgramInShell(closedProgramText, expectedOutput, { word }, statements);
}

static void testObjectsSurviveCollections()
{
    checkUnderEveryOption(WITH_PUBLISHED_FUNCTIONS("let kept = []; let wrong = 0; for (let index = 0; index < 60000; ++index) { let object = filledWithoutPrototype(index, kept, index + 0.5); if (!(index % 10000)) { kept[kept.length] = object; gc(); } if (Object.getPrototypeOf(object) !== null || Object.getPrototypeOf(withoutPrototype()) !== null || object.third !== index + 0.5) ++wrong; } print(wrong); print(kept.length); for (let index = 0; index < kept.length; ++index) print(kept[index].first + ' ' + (kept[index].second === kept) + ' ' + Object.keys(kept[index]).join());"),
        "0\n6\n0 true first,second,third\n10000 true first,second,third\n20000 true first,second,third\n30000 true first,second,third\n40000 true first,second,third\n50000 true first,second,third\n"_s);
}

std::span<const NativeTest> objectCreateProgramTests()
{
    static constexpr NativeTest tests[] = {
        { "object-create/null-gives-object-without-prototype"_s, testNullGivesObjectWithoutPrototype },
        { "object-create/stores-follow-the-creation"_s, testStoresFollowTheCreation },
        { "object-create/object-and-null-alternate"_s, testObjectAndNullAlternate },
        { "object-create/other-values-throw"_s, testOtherValuesThrow },
        { "object-create/other-values-throw-before-what-follows-the-creation"_s, testOtherValuesThrowBeforeWhatFollowsTheCreation },
        { "object-create/objects-survive-collections"_s, testObjectsSurviveCollections },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
