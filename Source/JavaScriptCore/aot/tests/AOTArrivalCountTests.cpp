/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTArrivalSiteNumbers.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTRealmTestSupport.h"
#include "AOTRuntimeCounters.h"
#include "AOTShellProgramTestSupport.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> arrivalCountTests();

static constexpr ASCIILiteral programText = R"IR(module m0 path="main.js" displayPath="main.js" body=f0
entry module=m0
layout L0 name="Point" fields=[{key="x" declared={type=anything} flags=[IsRequired]}, {key="y" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Size" fields=[{key="w" declared={type=anything} flags=[IsRequired]}, {key="h" declared={type=anything} flags=[IsRequired]}]
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = NewClosure function=f3
    v5 = Call v1, -, v2, v3, v4
    Return
}
function f1 name="makePoint" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = NewObject -, v0, v0 layout=L0 fields=[0, 1]
    v2 = FinishObject v1
    Return v2
}
function f2 name="makeSize" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = NewObject -, v0, v0 layout=L1 fields=[0, 1]
    v2 = FinishObject v1
    Return v2
}
function f3 name="readX" parameters=[{type=L0 flags=[IsExpected]}] results=[{type=anything}] {
  b0(v0: anything ~ L0):
    v1 = PropGet v0 key="x"
    Return v1
}
)IR"_s;

// The versions of a function and the copies that inlining makes have sites of their own, so the counts are added up by what arrived.
#define SHOW_ARRIVALS R"JS(
const arrivalLines = () => aotRuntimeCounters().split("\n").map(line => line.split("\t")).filter(([, name]) => name && name.startsWith("arrival:"));
const show = (...arrivals) => {
    const counts = new Map();
    for (const [count, , arrival] of arrivalLines()) {
        const what = arrival.replace(/^function:\d+$/, "function:compiled");
        counts.set(what, (counts.get(what) || 0) + Number(count));
    }
    for (const what of arrivals) print(what + " " + (counts.get(what) || 0));
};
)JS"

// With no test placed, every read of the program goes by name.
static constexpr ASCIILiteral withoutTypeTests = "disable=pass:PlaceTypeTests"_s;

static void testCountsLayoutThatWasNotExpected()
{
    checkOutputOfProgramInShell(programText, "layout:1 5\n"_s, { "count-arrivals"_s, "heat=EverythingIsHot"_s }, SHOW_ARRIVALS R"JS(
globalThis.publish = (makePoint, makeSize, readX) => {
    for (let i = 0; i < 7; i++) readX(makePoint(i));
    for (let i = 0; i < 5; i++) readX(makeSize(i));
    show("layout:1");
};
)JS" ""_s);
}

static void testCountsObjectThatLeftItsShape()
{
    checkOutputOfProgramInShell(programText,
        "layout:0 1\n"
        "layout:0+changed 5\n"_s,
        { "count-arrivals"_s, withoutTypeTests }, SHOW_ARRIVALS R"JS(
globalThis.publish = (makePoint, makeSize, readX) => {
    readX(makePoint(0));
    const withExtraProperty = makePoint(1);
    withExtraProperty.extra = 2;
    for (let i = 0; i < 3; i++) readX(withExtraProperty);
    readX(Object.freeze(makePoint(2)));
    readX(Object.setPrototypeOf(makePoint(3), null));
    show("layout:0", "layout:0+changed");
};
)JS" ""_s);
}

static void testNamesWhatChangedAnObject()
{
    checkOutputOfProgramInShell(programText,
        "layout:0 extra-properties 3\n"
        "layout:0 frozen,sealed,not-extensible 1\n"
        "layout:0 other-prototype 1\n"
        "layout:0 cell-marked,field-outside-slot,extra-properties 2\n"
        "rows 4\n"_s,
        { "count-arrivals"_s, withoutTypeTests }, R"JS(
globalThis.publish = (makePoint, makeSize, readX) => {
    readX(makePoint(0));
    const withExtraProperty = makePoint(1);
    withExtraProperty.extra = 2;
    for (let i = 0; i < 3; i++) readX(withExtraProperty);
    readX(Object.freeze(makePoint(2)));
    readX(Object.setPrototypeOf(makePoint(3), null));
    const withGetter = Object.defineProperty(makePoint(4), "y", { get() { return 5; }, configurable: true, enumerable: true });
    for (let i = 0; i < 2; i++) readX(withGetter);
    const counts = new Map();
    for (const [count, name, state] of aotRuntimeCounters().split("\n").map(line => line.split("\t"))) {
        if (name && name.startsWith("state:"))
            counts.set(state, (counts.get(state) || 0) + Number(count));
    }
    for (const state of ["layout:0 extra-properties", "layout:0 frozen,sealed,not-extensible", "layout:0 other-prototype", "layout:0 cell-marked,field-outside-slot,extra-properties"]) print(state + " " + (counts.get(state) || 0));
    print("rows " + counts.size);
};
)JS" ""_s);
}

static void testCountsForeignObjectWithItsNames()
{
    checkOutputOfProgramInShell(programText,
        "object{x,b} 2\n"
        "object{b,x} 1\n"
        "object{p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,..} 1\n"
        "object{} 1\n"
        "object{x}dict 1\n"
        "class:Array 1\n"
        "function:compiled 1\n"
        "function:native 2\n"
        "undefined 1\n"
        "null 1\n"
        "boolean 1\n"
        "number 2\n"
        "string 1\n"
        "symbol 1\n"
        "bigint 1\n"_s,
        { "count-arrivals"_s, withoutTypeTests }, SHOW_ARRIVALS R"JS(
globalThis.publish = (makePoint, makeSize, readX) => {
    for (let i = 0; i < 2; i++) readX({ x: i, b: 2 });
    readX(JSON.parse('{"b":1,"x":2}'));
    const withManyNames = {};
    for (let i = 0; i < 14; i++) withManyNames["p" + i] = i;
    readX(withManyNames);
    readX({});
    const dictionary = { x: 1 };
    $vm.toUncacheableDictionary(dictionary);
    readX(dictionary);
    readX([1]);
    readX(readX);
    readX(print);
    for (const nullish of [undefined, null]) {
        try { readX(nullish); } catch { }
    }
    for (const primitive of [true, 1, 1.5, "text", Symbol(), 10n]) readX(primitive);
    show("object{x,b}", "object{b,x}", "object{p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p11,..}", "object{}", "object{x}dict", "class:Array", "function:compiled", "function:native",
        "undefined", "null", "boolean", "number", "string", "symbol", "bigint");
};
)JS" ""_s, { "--useDollarVM=1"_s });
}

static void testCountsNothingWithoutTheOption()
{
    checkOutputOfProgramInShell(programText, "0\n"_s, { withoutTypeTests }, SHOW_ARRIVALS R"JS(
globalThis.publish = (makePoint, makeSize, readX) => {
    readX(makeSize(1));
    readX({ x: 1 });
    print(arrivalLines().length);
};
)JS" ""_s);
}

static String dumped(const ArrivalCounts& counts)
{
    StringPrintStream out;
    counts.dump(out);
    return out.toString();
}

static void testLinesAreSortedBySiteThenByCount()
{
    Realms realms;
    ArrivalCounts counts;
    AOT_TEST_CHECK(dumped(counts).isEmpty());

    counts.record(jsNumber(1), 9);
    counts.record(jsUndefined(), 4);
    counts.record(jsBoolean(true), 4);
    for (unsigned i = 0; i < 2; ++i)
        counts.record(jsNull(), 4);
    for (unsigned i = 0; i < 3; ++i)
        counts.record(realms.evaluate(realms.plain(), "({ a: 1, b: 2 })"_s), 4);
    counts.record(jsNull(), 0);

    AOT_TEST_CHECK(dumped(counts) ==
        "1\tarrival:0\tnull\n"
        "3\tarrival:4\tobject{a,b}\n"
        "2\tarrival:4\tnull\n"
        "1\tarrival:4\tboolean\n"
        "1\tarrival:4\tundefined\n"
        "1\tarrival:9\tnumber\n"_s);
}

static void testFirstOperationBehindArrivalIsWhereItsSiteLeft()
{
    ArrivalCounts counts;
    counts.didCallOperation(Entry::operationAOTPutByIdAndCache);
    AOT_TEST_CHECK(dumped(counts).isEmpty());

    for (unsigned i = 0; i < 2; ++i) {
        counts.didCallOperation(Entry::operationAOTRecordArrival);
        counts.record(jsNull(), 7);
        counts.didCallOperation(Entry::operationAOTPutByIdAndCache);
        counts.didCallOperation(Entry::operationAOTGetById);
    }
    counts.record(jsNull(), 7);
    counts.record(jsNull(), 3);
    counts.didCallOperation(Entry::operationAOTGetById);
    counts.record(jsNull(), 7);
    counts.didCallOperation(Entry::operationAOTGetById);

    AOT_TEST_CHECK(dumped(counts) ==
        "1\tarrival:3\tnull\n"
        "4\tarrival:7\tnull\n"
        "1\tleft:3\toperationAOTGetById\n"
        "2\tleft:7\toperationAOTPutByIdAndCache\n"
        "1\tleft:7\toperationAOTGetById\n"_s);
}

static void testObjectsWithTheSameNamesAreOneArrival()
{
    Realms realms;
    JSGlobalObject* realm = realms.plain();
    ArrivalCounts counts;
    counts.record(realms.evaluate(realm, "({ a: 1, b: 2 })"_s), 0);
    counts.record(realms.evaluate(realm, "(() => { const object = Object.create(null); object.a = 1; object.b = 2; return object; })()"_s), 0);
    counts.record(realms.evaluate(realm, "({ b: 1, a: 2 })"_s), 0);
    counts.record(realms.evaluate(realm, "new Map"_s), 0);
    counts.record(realms.evaluate(realm, "new Map"_s), 0);
    counts.record(realms.evaluate(realm, "new Map"_s), 0);

    AOT_TEST_CHECK(dumped(counts) ==
        "3\tarrival:0\tclass:Map\n"
        "2\tarrival:0\tobject{a,b}\n"
        "1\tarrival:0\tobject{b,a}\n"_s);
}

static constexpr ASCIILiteral describedProgramText = R"IR(module m0 path="main.js" displayPath="main.js"
layout L0 name="Point" fields=[{key="x" declared={type=number} flags=[IsRequired]}, {key="label" declared={type=string}}]
layout L1 kind=Environment fields=[{key="captured" declared={type=anything}}]
layout L2 fields=[{key="x" declared={type=number} flags=[IsRequired]}]
function f0 name="readX" module=m0 parameters=[{type=L0|L2 flags=[IsExpected]}, {type=anything}] results=[{type=anything}] {
  b0(v0: anything, v1: anything):
    v2 = PropGet v0 key="x" receivers=[L0, L2, ..] @7
    PropSet v1, v2 key="x" mode=Strict @9
    Return v2
}
)IR"_s;

static void testAccessByKeySaysWhatTheKeyWas()
{
    Realms realms;
    JSGlobalObject* realm = realms.plain();
    ArrivalCounts counts;
    uint32_t site = 0;
    auto record = [&](ASCIILiteral receiver, ASCIILiteral key) {
        counts.record(realm, realms.evaluate(realm, receiver), realms.evaluate(realm, key), site++);
    };
    record("[1, 2]"_s, "1"_s);
    record("[1.5]"_s, "3"_s);
    record("[{}]"_s, "0.5"_s);
    record("[1, 2]"_s, "'1'"_s);
    record("[1, 2]"_s, "-1"_s);
    record("'ab'"_s, "1"_s);
    record("new Uint8Array(2)"_s, "2"_s);
    record("({ ab: 1 })"_s, "'ab'"_s);
    record("({ ab: 1 })"_s, "String.fromCharCode(97, 98)"_s);
    record("Object.create({ ab: 1 })"_s, "'ab'"_s);
    record("({ ab: 1 })"_s, "String.fromCharCode(122, 113, 120, 119, 106)"_s);
    record("({ ab: 1 })"_s, "(() => { const left = String.fromCharCode(97, 98, 99); return left + String.fromCharCode(100, 101, 102); })()"_s);
    record("({ ab: 1 })"_s, "Symbol()"_s);
    record("({ ab: 1 })"_s, "null"_s);
    record("[1, 2]"_s, "'length'"_s);
    record("new Proxy({ ab: 1 }, {})"_s, "'ab'"_s);

    StringBuilder withoutMarks;
    for (StringView line : StringView(dumped(counts)).split('\n'))
        withoutMarks.append(line.left(line.reverseFind("; #"_s)), '\n');
    AOT_TEST_CHECK(withoutMarks.toString() ==
        "1\tarrival:0\tclass:Array; int32; key:index; own\n"
        "1\tarrival:1\tclass:Array; double; key:index; past the end\n"
        "1\tarrival:2\tclass:Array; contiguous; key:number that is no index; ?\n"
        "1\tarrival:3\tclass:Array; int32; key:text of an index; own\n"
        "1\tarrival:4\tclass:Array; int32; key:negative integer; ?\n"
        "1\tarrival:5\tstring; -; key:index; own\n"
        "1\tarrival:6\tclass:Uint8Array; none; key:index; absent\n"
        "1\tarrival:7\tobject{ab}; none; key:name; own\n"
        "1\tarrival:8\tobject{ab}; none; key:text made anew; own\n"
        "1\tarrival:9\tobject{}; none; key:name; inherited\n"
        "1\tarrival:10\tobject{ab}; none; key:text made anew; absent\n"
        "1\tarrival:11\tobject{ab}; none; key:rope; ?\n"
        "1\tarrival:12\tobject{ab}; none; key:symbol; absent\n"
        "1\tarrival:13\tobject{ab}; none; key:other; ?\n"
        "1\tarrival:14\tclass:Array; int32; key:name; own\n"
        "1\tarrival:15\tclass:ProxyObject; none; key:name; ?\n"_s);
}

static void testKeyIsLeftAsItWasAndEqualKeysAreOneArrival()
{
    Realms realms;
    JSGlobalObject* realm = realms.plain();
    ArrivalCounts counts;
    JSValue object = realms.evaluate(realm, "({ ab: 1 })"_s);
    JSValue anew = realms.evaluate(realm, "String.fromCharCode(97, 98)"_s);
    JSValue rope = realms.evaluate(realm, "(() => { const left = String.fromCharCode(97, 98, 99); return left + String.fromCharCode(100, 101, 102); })()"_s);
    for (unsigned i = 0; i < 3; ++i) {
        counts.record(realm, object, anew, 0);
        counts.record(realm, object, realms.evaluate(realm, "String.fromCharCode(97, 98)"_s), 0);
        counts.record(realm, object, rope, 1);
    }
    AOT_TEST_CHECK(!asString(anew)->tryGetValueImpl()->isAtom());
    AOT_TEST_CHECK(!asString(anew)->tryGetValueImpl()->hasHash());
    AOT_TEST_CHECK(asString(rope)->isRope());
    String lines = dumped(counts);
    AOT_TEST_CHECK(lines.startsWith("6\tarrival:0\tobject{ab}; none; key:text made anew; own; #"_s));
    AOT_TEST_CHECK(lines.contains("\n3\tarrival:1\tobject{ab}; none; key:rope; ?; #0000\n"_s));
}

static void testReportListsSitesAndLayouts()
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    AOT_TEST_CHECK(!applyOptionWord(options, "count-arrivals"_s));
    BuildContext build(options);
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(describedProgramText, Typed::IRForm::Selected, build);
    AOT_TEST_CHECK(program);

    Typed::ArrivalSiteNumbers numbers = Typed::numberArrivalSites(*program, build);
    AOT_TEST_CHECK(numbers.isCounting());
    Vector<String> lines = String::fromUTF8(Typed::describeArrivalSites({ *program }, numbers, build).span()).split('\n');

    AOT_TEST_CHECK(lines.size() >= 4);
    AOT_TEST_CHECK(lines[0].startsWith("S\t0\tPropGet\tRead\tUnknown\tUnknown\tOpen\tmain.js\t"_s) && lines[0].endsWith("\treadX\t0,2\tf0"_s));
    AOT_TEST_CHECK(lines[1].startsWith("S\t1\tPropSet\tStore\tUnknown\tUnknown\tNone\tmain.js\t"_s) && lines[1].endsWith("\treadX\t\tf0"_s));
    // An environment is no receiver. The layouts that the engine adds to every program come after those of the text.
    AOT_TEST_CHECK(lines[2] == "L\t0\tRecord\tPoint\t\tx,label?"_s);
    AOT_TEST_CHECK(lines[3] == "L\t2\tRecord\t\t\tx"_s);
}

static void testReportIsEmptyWithoutTheOption()
{
    BuildContext build(makeDefaultCompileOptions());
    std::unique_ptr<Typed::Program> program = Typed::parseProgram(describedProgramText, Typed::IRForm::Selected, build);
    AOT_TEST_CHECK(program);

    Typed::ArrivalSiteNumbers numbers = Typed::numberArrivalSites(*program, build);
    AOT_TEST_CHECK(!numbers.isCounting());
    AOT_TEST_CHECK(!numbers.numberOf(Typed::FunctionID(0u), Typed::ValueID(2u), 0));
    AOT_TEST_CHECK(Typed::describeArrivalSites({ *program }, numbers, build).isNull());
}

std::span<const NativeTest> arrivalCountTests()
{
    static constexpr NativeTest tests[] = {
        { "arrival-counts/programs/counts-layout-that-was-not-expected"_s, testCountsLayoutThatWasNotExpected },
        { "arrival-counts/programs/counts-object-that-left-its-shape"_s, testCountsObjectThatLeftItsShape },
        { "arrival-counts/programs/names-what-changed-an-object"_s, testNamesWhatChangedAnObject },
        { "arrival-counts/programs/counts-foreign-object-with-its-names"_s, testCountsForeignObjectWithItsNames },
        { "arrival-counts/programs/counts-nothing-without-the-option"_s, testCountsNothingWithoutTheOption },
        { "arrival-counts/lines-are-sorted-by-site-then-by-count"_s, testLinesAreSortedBySiteThenByCount },
        { "arrival-counts/first-operation-behind-an-arrival-is-where-its-site-left"_s, testFirstOperationBehindArrivalIsWhereItsSiteLeft },
        { "arrival-counts/objects-with-the-same-names-are-one-arrival"_s, testObjectsWithTheSameNamesAreOneArrival },
        { "arrival-counts/an-access-by-key-says-what-the-key-was"_s, testAccessByKeySaysWhatTheKeyWas },
        { "arrival-counts/the-key-is-left-as-it-was-and-equal-keys-are-one-arrival"_s, testKeyIsLeftAsItWasAndEqualKeysAreOneArrival },
        { "arrival-counts/report-lists-sites-and-layouts"_s, testReportListsSitesAndLayouts },
        { "arrival-counts/report-is-empty-without-the-option"_s, testReportIsEmptyWithoutTheOption },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
