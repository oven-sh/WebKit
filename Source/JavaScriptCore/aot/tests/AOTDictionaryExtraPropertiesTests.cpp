/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTShellProgramTestSupport.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertyNameArray.h"
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

std::span<const NativeTest> dictionaryExtraPropertiesTests();

static constexpr ASCIILiteral programText = R"IR(layout L0 name="Counter" fields=[{key="total" declared={type=anything} flags=[IsRequired]}]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = Call v1, -, v2, v3
    Return
}
function f1 name="makeCounter" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = FinishObject v1
    Return v2
}
function f2 name="count" parameters=[{type=anything}, {type=string}, {type=anything}] results=[{type=anything}] {
    ElemSet v0, v1, v2
    v4 = ElemDelete v0, v1
    Return
}
)IR"_s;

namespace {

}

static constexpr ASCIILiteral programOfReadsByName = R"IR(layout L0 name="Counter" fields=[{key="total" declared={type=anything} flags=[IsRequired]}]
layout L1 name="Labeled" fields=[{key="label" declared={type=anything} flags=[IsRequired]}]
module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
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
    v10 = NewClosure function=f9
    v11 = Call v1, -, v2, v3, v4, v5, v6, v7, v8, v9, v10
    Return
}
function f1 name="makeCounter" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = NewObject -, v0 layout=L0 fields=[0]
    v2 = FinishObject v1
    Return v2
}
function f2 name="makeLabeled" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = NewObject -, v0 layout=L1 fields=[0]
    v2 = FinishObject v1
    Return v2
}
function f3 name="readLabel" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="label"
    Return v1
}
function f4 name="readLoose" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="loose"
    Return v1
}
function f5 name="labelIsTruthy" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="label"
    v2 = ToBoolean v1
    Branch v2 -> taken b1(), notTaken b2()
  b1():
    v3 = Integer integer=1
    Return v3
  b2():
    v4 = Integer
    Return v4
}
function f6 name="looseIsTruthy" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="loose"
    v2 = ToBoolean v1
    Branch v2 -> taken b1(), notTaken b2()
  b1():
    v3 = Integer integer=1
    Return v3
  b2():
    v4 = Integer
    Return v4
}
function f7 name="readLabelOfLoose" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="loose"
    v2 = PropGet v1 key="label"
    Return v2
}
function f8 name="readBoth" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="label"
    v2 = PropGet v0 key="loose"
    v3 = NewArray v1, v2 contents=anything
    Return v3
}
function f9 name="readTotal" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = PropGet v0 key="total"
    Return v1
}
)IR"_s;

#define STATEMENTS_CHECKING_ANSWERS R"JS(
const check = (what, answer, expected) => {
    let wrong = 0;
    let last;
    for (let i = 0; i < 400; i++) {
        last = answer();
        if (last !== expected) wrong++;
    }
    print(what + ": " + (wrong ? "WRONG " + wrong + " times, at last " + last : "right"));
};
)JS"

static constexpr ASCIILiteral statementsChangingWhatReadsFind = STATEMENTS_CHECKING_ANSWERS R"JS(
globalThis.publish = (makeCounter, makeLabeled, readLabel, readLoose, labelIsTruthy, looseIsTruthy) => {
    for (const [name, read, isTruthy] of [["label", readLabel, labelIsTruthy], ["loose", readLoose, looseIsTruthy]]) {
        const expect = (what, object, expected) => check(name + ", " + what, () => read(object) + " " + isTruthy(object), expected);

        const counter = makeCounter(1);
        expect("nothing has it", counter, "undefined 0");
        counter[name] = 5;
        expect("added", counter, "5 1");
        counter[name] = 0;
        expect("replaced", counter, "0 0");
        delete counter[name];
        expect("deleted", counter, "undefined 0");
        counter[name] = 6;
        expect("added again", counter, "6 1");
        Object.defineProperty(counter, name, { get() { return this === counter ? 17 : "another receiver"; }, configurable: true });
        expect("a getter", counter, "17 1");
        Object.defineProperty(counter, name, { value: 7, writable: true, enumerable: true, configurable: true });
        expect("a value again", counter, "7 1");
        Object.freeze(counter);
        counter[name] = 8;
        delete counter[name];
        expect("frozen", counter, "7 1");

        const below = makeCounter(2);
        const holder = { [name]: 9 };
        Object.setPrototypeOf(below, holder);
        expect("a holder above", below, "9 1");
        holder[name] = 0;
        expect("replaced on the holder", below, "0 0");
        delete holder[name];
        expect("deleted on the holder", below, "undefined 0");
        holder[name] = 10;
        expect("added on the holder", below, "10 1");
        Object.defineProperty(holder, name, { get() { return this === below ? 11 : "another receiver"; }, configurable: true });
        expect("a getter on the holder", below, "11 1");
        Object.setPrototypeOf(below, { [name]: 12 });
        expect("another holder", below, "12 1");
        below[name] = 13;
        expect("its own in front of the holder's", below, "13 1");
        delete below[name];
        expect("the holder's again", below, "12 1");
        Object.setPrototypeOf(below, null);
        expect("no prototype", below, "undefined 0");

        const wide = { };
        for (let i = 0; i < 20; i++) wide["filler" + i] = i;
        wide[name] = 14;
        const belowWide = makeCounter(3);
        Object.setPrototypeOf(belowWide, wide);
        expect("out of line in the holder", belowWide, "14 1");

        const full = makeCounter(4);
        for (let i = 0; i < 20; i++) full["filler" + i] = i;
        full[name] = 15;
        expect("out of line", full, "15 1");
        delete full.filler3;
        expect("behind the deletion of another name", full, "15 1");

        const top = { };
        const deep = makeCounter(5);
        Object.setPrototypeOf(deep, Object.create(top));
        expect("nothing two objects above", deep, "undefined 0");
        top[name] = 16;
        expect("added two objects above", deep, "16 1");
        delete top[name];
        expect("deleted two objects above", deep, "undefined 0");

        const first = makeCounter(6);
        const second = makeCounter(7);
        first[name] = 1;
        second[name] = 2;
        check(name + ", two objects by turns", () => read(first) + " " + read(second), "1 2");
        gc();
        check(name + ", behind a collection", () => read(first) + " " + read(second), "1 2");
    }
};
)JS"_s;

#define LINES_OF_CHANGES(name) \
    name ", nothing has it: right\n" \
    name ", added: right\n" \
    name ", replaced: right\n" \
    name ", deleted: right\n" \
    name ", added again: right\n" \
    name ", a getter: right\n" \
    name ", a value again: right\n" \
    name ", frozen: right\n" \
    name ", a holder above: right\n" \
    name ", replaced on the holder: right\n" \
    name ", deleted on the holder: right\n" \
    name ", added on the holder: right\n" \
    name ", a getter on the holder: right\n" \
    name ", another holder: right\n" \
    name ", its own in front of the holder's: right\n" \
    name ", the holder's again: right\n" \
    name ", no prototype: right\n" \
    name ", out of line in the holder: right\n" \
    name ", out of line: right\n" \
    name ", behind the deletion of another name: right\n" \
    name ", nothing two objects above: right\n" \
    name ", added two objects above: right\n" \
    name ", deleted two objects above: right\n" \
    name ", two objects by turns: right\n" \
    name ", behind a collection: right\n"

static void testReadsByNameFollowEveryChange()
{
    checkOutputOfProgramInShell(programOfReadsByName, LINES_OF_CHANGES("label") LINES_OF_CHANGES("loose") ""_s, { }, statementsChangingWhatReadsFind);
}

static constexpr ASCIILiteral statementsWithTwoReads = STATEMENTS_CHECKING_ANSWERS R"JS(
globalThis.publish = (makeCounter, makeLabeled, readLabel, readLoose, labelIsTruthy, looseIsTruthy, readLabelOfLoose, readBoth, readTotal) => {
    const outer = makeCounter(1);
    const inner = makeCounter(2);
    inner.label = 21;
    outer.loose = inner;
    check("one behind the other, two extra properties", () => readLabelOfLoose(outer), 21);
    delete inner.label;
    check("one behind the other, the second deleted", () => readLabelOfLoose(outer), undefined);
    outer.loose = makeLabeled(22);
    check("one behind the other, a field", () => readLabelOfLoose(outer), 22);
    outer.loose = { label: 23 };
    check("one behind the other, an ordinary object", () => readLabelOfLoose(outer), 23);
    Object.defineProperty(outer, "loose", { get() { return inner; }, configurable: true });
    inner.label = 24;
    check("one behind the other, a getter first", () => readLabelOfLoose(outer), 24);
    delete outer.loose;
    Object.setPrototypeOf(outer, { loose: makeLabeled(25) });
    check("one behind the other, a holder first", () => readLabelOfLoose(outer), 25);

    const both = makeCounter(3);
    check("side by side, neither", () => readBoth(both).join(), ",");
    both.label = 1;
    both.loose = 2;
    check("side by side, two extra properties", () => readBoth(both).join(), "1,2");
    delete both.label;
    check("side by side, the first deleted", () => readBoth(both).join(), ",2");
    Object.setPrototypeOf(both, { label: 3 });
    check("side by side, a holder and an extra property", () => readBoth(both).join(), "3,2");
    check("side by side, a field and nothing", () => readBoth(makeLabeled(4)).join(), "4,");

    const counter = makeCounter(40);
    check("a field in its slot", () => readTotal(counter), 40);
    Object.defineProperty(counter, "total", { get() { return this === counter ? 30 : "another receiver"; }, configurable: true });
    check("a field that became a getter", () => readTotal(counter), 30);
    Object.defineProperty(counter, "total", { value: 31, writable: false, enumerable: false, configurable: true });
    check("a field that became read only", () => readTotal(counter), 31);
    delete counter.total;
    check("a field that was deleted", () => readTotal(counter), undefined);
    counter.total = 32;
    check("a field that was added again", () => readTotal(counter), 32);
    check("the field of another object of the layout", () => readTotal(makeCounter(41)), 41);

    const labeled = makeLabeled(50);
    check("where most layouts have the name", () => readLabel(labeled), 50);
    delete labeled.label;
    check("its slot is empty", () => readLabel(labeled), undefined);
    Object.setPrototypeOf(labeled, { label: 51 });
    check("its slot is empty below a holder", () => readLabel(labeled), 51);
    labeled.label = 52;
    check("its slot is filled again", () => readLabel(labeled), 52);
    check("an ordinary object", () => readLabel({ label: 53 }), 53);
    check("an ordinary object without the name", () => readLabel({ other: 1 }), undefined);
    check("a text", () => readLabel("text"), undefined);
    check("a number", () => readLabel(5), undefined);
};
)JS"_s;

static void testTwoReadsByNameFollowEveryChange()
{
    checkOutputOfProgramInShell(programOfReadsByName,
        "one behind the other, two extra properties: right\n"
        "one behind the other, the second deleted: right\n"
        "one behind the other, a field: right\n"
        "one behind the other, an ordinary object: right\n"
        "one behind the other, a getter first: right\n"
        "one behind the other, a holder first: right\n"
        "side by side, neither: right\n"
        "side by side, two extra properties: right\n"
        "side by side, the first deleted: right\n"
        "side by side, a holder and an extra property: right\n"
        "side by side, a field and nothing: right\n"
        "a field in its slot: right\n"
        "a field that became a getter: right\n"
        "a field that became read only: right\n"
        "a field that was deleted: right\n"
        "a field that was added again: right\n"
        "the field of another object of the layout: right\n"
        "where most layouts have the name: right\n"
        "its slot is empty: right\n"
        "its slot is empty below a holder: right\n"
        "its slot is filled again: right\n"
        "an ordinary object: right\n"
        "an ordinary object without the name: right\n"
        "a text: right\n"
        "a number: right\n"_s,
        { }, statementsWithTwoReads);
}

template<LoadedProgramTest body>
static constexpr void (*withCounter)() = runNativeTestWithLoadedProgram<dictionaryExtraPropertiesTests, programText, body>;

std::span<const NativeTest> dictionaryExtraPropertiesTests()
{
    static constexpr NativeTest tests[] = {
        { "extra-properties/programs/reads-by-name-follow-every-change"_s, testReadsByNameFollowEveryChange },
        { "extra-properties/programs/two-reads-by-name-follow-every-change"_s, testTwoReadsByNameFollowEveryChange },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
