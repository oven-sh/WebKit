/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinFamilyState.h"
#include "AOTBuiltinLocationWatch.h"
#include "AOTBuiltinTable.h"
#include "AOTLoadedProgramTestSupport.h"
#include "AOTShellProgramTestSupport.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

std::span<const NativeTest> builtinLocationWatchTests();

static constexpr ASCIILiteral usesOfMathSign = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = Builtin builtin=MathSign
    v5 = NewClosure function=f3
    v6 = Call v1, -, v2, v3, v4, v5
    Return
}
function f1 name="callSign" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=Math
    v2 = PropGet v1 key="sign"
    v3 = Call v2, v1, v0 calleeKey="sign"
    Return v3
}
function f2 name="readSign" results=[{type=anything}] {
    v0 = Builtin builtin=Math
    v1 = PropGet v0 key="sign"
    Return v1
}
function f3 name="storeSign" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=Math
    PropSet v1, v0 key="sign" mode=Sloppy
    Return
}
)IR"_s;

// Intl makes the constructor when somebody first looks for it. The program stores to the member itself, so no proof says what a read finds.
static constexpr ASCIILiteral usesOfIntlCollator = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = Call v1, -, v2, v3
    Return
}
function f1 name="readCollator" results=[{type=anything}] {
    v0 = Builtin builtin=Intl
    v1 = PropGet v0 key="Collator"
    Return v1
}
function f2 name="storeCollator" parameters=[{type=anything}] results=[{type=anything}] {
    v1 = Builtin builtin=Intl
    PropSet v1, v0 key="Collator" mode=Sloppy
    Return
}
)IR"_s;

// Code that nobody compiled calls both functions, so each expects an error and tests for it.
static constexpr ASCIILiteral readsOfErrors = R"IR(module m0 path="/main.js" displayPath="main.js" body=f0
entry module=m0
function f0 name="main" kind=Module results=[{type=anything}] {
    v0 = Builtin builtin=GlobalThis
    v1 = PropGet v0 key="publish" [isFreeName]
    v2 = NewClosure function=f1
    v3 = NewClosure function=f2
    v4 = Call v1, -, v2, v3
    Return
}
function f1 name="readMessage" parameters=[{type=Error flags=[IsExpected]}] results=[{type=anything}] {
    v1 = PropGet v0 key="message"
    Return v1
}
function f2 name="readCause" parameters=[{type=Error flags=[IsExpected]}] results=[{type=anything}] {
    v1 = PropGet v0 key="cause"
    Return v1
}
)IR"_s;

#define HELPERS R"JS(
const arms = () => Number(aotOperationCount("operationAOTArmBuiltinLocation"));
const repeat = action => {
    let result;
    for (let i = 0; i < 100; i++) result = action();
    return result;
};
const describeError = (what, action) => {
    try {
        action();
        print(what + ": no error");
    } catch (error) {
        print(what + ": " + error.constructor.name);
    }
};
)JS"

static void checkInEveryBuild(ASCIILiteral programText, ASCIILiteral statements, ASCIILiteral expectedOutput)
{
    checkOutputOfProgramInShell(programText, expectedOutput, { }, statements);
    for (ASCIILiteral word : { "validate"_s, "heat=EverythingIsHot"_s, "heat=EverythingIsCold"_s, "disable=flow-results"_s, "disable=pass:PlaceTypeTests"_s })
        checkOutputOfProgramInShell(programText, expectedOutput, { word }, statements);
}

static void testReplacedMethodIsCalled()
{
    checkInEveryBuild(usesOfMathSign, HELPERS R"JS(
globalThis.publish = (callSign, readSign, original) => {
    print("at first: " + repeat(() => callSign(-3)));
    print("read at first: " + (repeat(readSign) === original));
    Math.unrelated = 1;
    print("after another property was added: " + repeat(() => callSign(-3)));
    Math.sign = () => "replaced";
    print("after the store: " + repeat(() => callSign(-3)));
    print("read after the store: " + (repeat(readSign) === Math.sign));
    Math.sign = original;
    print("with the original back: " + repeat(() => callSign(-3)));
    Object.defineProperty(Math, "sign", { get() { return () => "from a getter"; }, configurable: true });
    print("after a getter was defined: " + repeat(() => callSign(-3)));
};
)JS"_s,
        "at first: -1\n"
        "read at first: true\n"
        "after another property was added: -1\n"
        "after the store: replaced\n"
        "read after the store: true\n"
        "with the original back: -1\n"
        "after a getter was defined: from a getter\n"_s);
}

static void testMethodReplacedBeforeFirstUseIsCalled()
{
    checkInEveryBuild(usesOfMathSign, HELPERS R"JS(
globalThis.publish = (callSign, readSign, original) => {
    Math.sign = () => "replaced";
    print("called: " + repeat(() => callSign(-3)));
    print("read: " + (repeat(readSign) === Math.sign));
    print("the original is another function: " + (original !== Math.sign && original(-3) === -1));
};
)JS"_s,
        "called: replaced\n"
        "read: true\n"
        "the original is another function: true\n"_s);
}

static void testDeletedMethodThrows()
{
    checkInEveryBuild(usesOfMathSign, HELPERS R"JS(
globalThis.publish = (callSign, readSign) => {
    print("at first: " + repeat(() => callSign(-3)));
    delete Math.sign;
    describeError("called after the delete", () => callSign(-3));
    print("read after the delete: " + repeat(readSign));
    Math.sign = () => "defined again";
    print("defined again: " + repeat(() => callSign(-3)));
};
)JS"_s,
        "at first: -1\n"
        "called after the delete: TypeError\n"
        "read after the delete: undefined\n"
        "defined again: defined again\n"_s);
}

// One call of the run time starts the watch. From then on the byte answers, also once it says that the member has changed.
static void testArmedOnFirstUseOnly()
{
    checkOutputOfProgramInShell(usesOfMathSign,
        "questions before the first use: 0\n"
        "questions of the first hundred uses: 1\n"
        "questions of the next hundred uses: 0\n"
        "questions of another site: 0\n"
        "questions after the store: 0\n"_s,
        { "heat=EverythingIsHot"_s }, HELPERS R"JS(
globalThis.publish = (callSign, readSign) => {
    let before = arms();
    const questions = what => {
        print("questions " + what + ": " + (arms() - before));
        before = arms();
    };
    print("questions before the first use: " + before);
    repeat(() => callSign(-3));
    questions("of the first hundred uses");
    repeat(() => callSign(-3));
    questions("of the next hundred uses");
    repeat(readSign);
    questions("of another site");
    Math.sign = () => 0;
    repeat(() => callSign(-3));
    repeat(readSign);
    questions("after the store");
};
)JS"_s,
        { "--aotCountsPath=/dev/null"_s });
}

static void testMemberThatHostMakesIsReadAsByName()
{
    checkInEveryBuild(usesOfIntlCollator, HELPERS R"JS(
globalThis.publish = (readCollator, storeCollator) => {
    const made = repeat(readCollator);
    print("made by the compiled read: " + typeof made + " " + (made === Intl.Collator));
    Intl.unrelated = 1;
    print("after another property was added: " + (repeat(readCollator) === made));
    Intl.Collator = "replaced";
    print("after the store: " + repeat(readCollator));
    storeCollator(made);
    print("with the original back: " + (repeat(readCollator) === made));
    Object.defineProperty(Intl, "Collator", { value: "defined", writable: true, configurable: true });
    print("after a definition: " + repeat(readCollator));
    delete Intl.Collator;
    print("after the delete: " + repeat(readCollator));
};
)JS"_s,
        "made by the compiled read: function true\n"
        "after another property was added: true\n"
        "after the store: replaced\n"
        "with the original back: true\n"
        "after a definition: defined\n"
        "after the delete: undefined\n"_s);
}

static void testMemberReplacedBeforeHostMakesItIsRead()
{
    checkInEveryBuild(usesOfIntlCollator, HELPERS R"JS(
globalThis.publish = readCollator => {
    Intl.Collator = "replaced";
    print("read: " + repeat(readCollator));
};
)JS"_s,
        "read: replaced\n"_s);
}

// Start-up makes nothing. The first question finds a member that nobody has made and makes none, the read by name behind it makes the member, and the second question starts the watch.
static void testMemberThatHostMakesIsArmedOnceItIsMade()
{
    checkOutputOfProgramInShell(usesOfIntlCollator,
        "questions of the first hundred reads: 2\n"
        "questions of the next hundred reads: 0\n"
        "questions after the store: 0\n"_s,
        { "heat=EverythingIsHot"_s }, HELPERS R"JS(
globalThis.publish = readCollator => {
    let before = arms();
    const questions = what => {
        print("questions " + what + ": " + (arms() - before));
        before = arms();
    };
    repeat(readCollator);
    questions("of the first hundred reads");
    repeat(readCollator);
    questions("of the next hundred reads");
    Intl.Collator = 0;
    repeat(readCollator);
    questions("after the store");
};
)JS"_s,
        { "--aotCountsPath=/dev/null"_s });
    checkOutputOfProgramInShell(usesOfIntlCollator,
        "questions of the first hundred reads: 1\n"
        "the same object: true\n"_s,
        { "heat=EverythingIsHot"_s }, HELPERS R"JS(
globalThis.publish = readCollator => {
    const made = Intl["Coll" + "ator"];
    const before = arms();
    const read = repeat(readCollator);
    print("questions of the first hundred reads: " + (arms() - before));
    print("the same object: " + (read === made));
};
)JS"_s,
        { "--aotCountsPath=/dev/null"_s });
}

static void testOwnSlotsOfErrorsAreReadAsByName()
{
    checkInEveryBuild(readsOfErrors, HELPERS R"JS(
globalThis.publish = (readMessage, readCause) => {
    class Derived extends Error { }
    const withBoth = new Error("both", { cause: "why" });
    const stored = new Error("first");
    stored.message = 5;
    const deleted = new Error("gone");
    delete deleted.message;
    const accessor = new Error("hidden");
    Object.defineProperty(accessor, "message", { get() { return "from a getter"; } });
    const withStack = new Error("has a stack");
    withStack.stack;
    print("message: " + repeat(() => readMessage(new Error("plain"))));
    print("message beside a cause: " + repeat(() => readMessage(withBoth)));
    print("cause: " + repeat(() => readCause(withBoth)));
    print("cause without a message: " + repeat(() => readCause(new Error(undefined, { cause: "alone" }))));
    print("no cause: " + repeat(() => readCause(new Error("plain"))));
    print("no message: " + JSON.stringify(repeat(() => readMessage(new Error))));
    print("stored: " + typeof repeat(() => readMessage(stored)));
    print("deleted: " + JSON.stringify(repeat(() => readMessage(deleted))));
    print("accessor: " + repeat(() => readMessage(accessor)));
    print("after the stack was made: " + repeat(() => readMessage(withStack)));
    print("class below: " + repeat(() => readMessage(new TypeError("type"))));
    print("class of the program: " + repeat(() => readMessage(new Derived("derived"))));
    Error.prototype.message = "inherited";
    print("inherited: " + repeat(() => readMessage(new Error)));
};
)JS"_s,
        "message: plain\n"
        "message beside a cause: both\n"
        "cause: why\n"
        "cause without a message: alone\n"
        "no cause: undefined\n"
        "no message: \"\"\n"
        "stored: number\n"
        "deleted: \"\"\n"
        "accessor: from a getter\n"
        "after the stack was made: has a stack\n"
        "class below: type\n"
        "class of the program: derived\n"
        "inherited: inherited\n"_s);
}

static BuiltinLocationWatch& watchOf(LoadedProgram& program)
{
    return *program.state().builtinFamilies->locationWatch;
}

static void evaluate(LoadedProgram& program, ASCIILiteral source)
{
    program.realms().evaluate(program.realm(), source);
}

static void testNothingIsWatchedBeforeTheFirstQuestion(LoadedProgram& program)
{
    BuiltinLocationWatch& watch = watchOf(program);
    AOT_TEST_CHECK(!watch.numberOfWatches());
    AOT_TEST_CHECK(watch.stateOf(Builtins::MathSign) == WatchedLocationState::NotArmed);

    AOT_TEST_CHECK(watch.arm(program.vm(), Builtins::MathSign));
    AOT_TEST_CHECK(watch.stateOf(Builtins::MathSign) == WatchedLocationState::Unchanged);
    AOT_TEST_CHECK(watch.numberOfWatches() == 1);

    AOT_TEST_CHECK(watch.arm(program.vm(), Builtins::MathSign));
    AOT_TEST_CHECK(watch.numberOfWatches() == 1);
}

static void testStoreEndsTheWatchForGood(LoadedProgram& program)
{
    BuiltinLocationWatch& watch = watchOf(program);
    evaluate(program, "globalThis.original = Math.sign"_s);
    AOT_TEST_CHECK(watch.arm(program.vm(), Builtins::MathSign));

    evaluate(program, "Math.unrelated = 1; Math.sinh = 2"_s);
    AOT_TEST_CHECK(watch.stateOf(Builtins::MathSign) == WatchedLocationState::Unchanged);

    evaluate(program, "Math.sign = () => 0"_s);
    AOT_TEST_CHECK(watch.stateOf(Builtins::MathSign) == WatchedLocationState::Changed);
    AOT_TEST_CHECK(!watch.arm(program.vm(), Builtins::MathSign));

    evaluate(program, "Math.sign = original"_s);
    AOT_TEST_CHECK(!watch.arm(program.vm(), Builtins::MathSign));
}

static void testMemberReplacedBeforeTheFirstQuestionIsChanged(LoadedProgram& program)
{
    BuiltinLocationWatch& watch = watchOf(program);
    evaluate(program, "Math.sign = () => 0"_s);
    AOT_TEST_CHECK(!watch.arm(program.vm(), Builtins::MathSign));
    AOT_TEST_CHECK(watch.stateOf(Builtins::MathSign) == WatchedLocationState::Changed);
    AOT_TEST_CHECK(!watch.numberOfWatches());
}

static void testDeleteEndsTheWatch(LoadedProgram& program)
{
    BuiltinLocationWatch& watch = watchOf(program);
    AOT_TEST_CHECK(watch.arm(program.vm(), Builtins::MathSign));
    evaluate(program, "delete Math.sign"_s);
    AOT_TEST_CHECK(watch.stateOf(Builtins::MathSign) == WatchedLocationState::Changed);
}

// Nobody kept what the realm was created with under a name that the program does not use, and a cached store to a free name fires no watchpoint.
static void testMembersThatCannotBeComparedAreChanged(LoadedProgram& program)
{
    BuiltinLocationWatch& watch = watchOf(program);
    AOT_TEST_CHECK(!watch.arm(program.vm(), Builtins::MathCbrt));
    AOT_TEST_CHECK(!watch.arm(program.vm(), Builtins::Date));
    AOT_TEST_CHECK(!watch.numberOfWatches());
}

// The program has no cell for this member of Intl, so start-up has not looked for it.
static void testQuestionMakesNoMember(LoadedProgram& program)
{
    BuiltinLocationWatch& watch = watchOf(program);
    JSObject* holder = program.realms().object(program.realm(), "Intl"_s);
    Identifier name = Identifier::fromString(program.vm(), "Segmenter"_s);

    for (unsigned question = 0; question < 2; ++question) {
        AOT_TEST_CHECK(!watch.arm(program.vm(), Builtins::IntlSegmenter));
        AOT_TEST_CHECK(watch.stateOf(Builtins::IntlSegmenter) == WatchedLocationState::NotArmed);
        AOT_TEST_CHECK(!isValidOffset(holder->getDirectOffset(program.vm(), name)));
        AOT_TEST_CHECK(!watch.numberOfWatches());
    }

    evaluate(program, "Intl.Segmenter"_s);
    AOT_TEST_CHECK(watch.arm(program.vm(), Builtins::IntlSegmenter));
    AOT_TEST_CHECK(watch.stateOf(Builtins::IntlSegmenter) == WatchedLocationState::Unchanged);

    evaluate(program, "Intl.Segmenter = 1"_s);
    AOT_TEST_CHECK(watch.stateOf(Builtins::IntlSegmenter) == WatchedLocationState::Changed);
}

static void testMemberMadeBeforeTheFirstQuestionIsWatched(LoadedProgram& program)
{
    BuiltinLocationWatch& watch = watchOf(program);
    evaluate(program, "Intl.Collator"_s);
    AOT_TEST_CHECK(watch.arm(program.vm(), Builtins::IntlCollator));
    AOT_TEST_CHECK(watch.stateOf(Builtins::IntlCollator) == WatchedLocationState::Unchanged);
}

static void testMemberStoredBeforeHostMakesItIsChanged(LoadedProgram& program)
{
    BuiltinLocationWatch& watch = watchOf(program);
    evaluate(program, "Intl.Segmenter = 1"_s);
    AOT_TEST_CHECK(!watch.arm(program.vm(), Builtins::IntlSegmenter));
    AOT_TEST_CHECK(watch.stateOf(Builtins::IntlSegmenter) == WatchedLocationState::Changed);
}

static void testMemberMadeAndThenReplacedIsChanged(LoadedProgram& program)
{
    BuiltinLocationWatch& watch = watchOf(program);
    evaluate(program, "Intl.Collator; Intl.Collator = function () { }"_s);
    AOT_TEST_CHECK(!watch.arm(program.vm(), Builtins::IntlCollator));
    AOT_TEST_CHECK(watch.stateOf(Builtins::IntlCollator) == WatchedLocationState::Changed);
}

static BuiltinOverloadID ownSlotOverloadOf(BuiltinID member)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    for (BuiltinOverloadID overload : table.overloadIDsOf(member)) {
        if (table.at(overload).code.implementation.kind == BuiltinImplementationKind::OwnSlot)
            return overload;
    }
    failNativeTest(__FILE__, __LINE__, "the member is an own slot");
}

static uint32_t publishedStructureIDOf(LoadedProgram& program, BuiltinID member)
{
    BuiltinOverloadID overload = ownSlotOverloadOf(member);
    publishStructureOfOwnSlot(program.instance(), overload);
    return program.instance().builtins.ownSlotStructureIDs[BuiltinTable::singleton().at(overload).code.implementation.ownSlot.structure.index()];
}

static uint32_t structureIDOf(LoadedProgram& program, ASCIILiteral expression)
{
    return program.realms().object(program.realm(), expression)->structureID().bits();
}

static void testOwnSlotHasTheStructureOfNewError(LoadedProgram& program)
{
    for (uint32_t word : program.instance().builtins.ownSlotStructureIDs)
        AOT_TEST_CHECK(!word);

    uint32_t withMessage = publishedStructureIDOf(program, Builtins::ErrorPrototypeMessage);
    uint32_t withMessageAndCause = publishedStructureIDOf(program, Builtins::ErrorPrototypeCause);
    AOT_TEST_CHECK(withMessage && withMessage != structureIDOfNoInstance);
    AOT_TEST_CHECK(withMessageAndCause && withMessageAndCause != structureIDOfNoInstance && withMessageAndCause != withMessage);

    AOT_TEST_CHECK(structureIDOf(program, "new Error('text')"_s) == withMessage);
    AOT_TEST_CHECK(structureIDOf(program, "new Error('text', { cause: 1 })"_s) == withMessageAndCause);
    AOT_TEST_CHECK(structureIDOf(program, "new Error"_s) != withMessage);
    AOT_TEST_CHECK(structureIDOf(program, "new Error(undefined, { cause: 1 })"_s) != withMessageAndCause);
    AOT_TEST_CHECK(structureIDOf(program, "new TypeError('text')"_s) != withMessage);

    JSObject* error = program.realms().object(program.realm(), "new Error('text', { cause: 1 })"_s);
    AOT_TEST_CHECK(error->getDirectOffset(program.vm(), program.vm().propertyNames->message) == propertyOffsetOfOwnSlot(ErrorInstanceType, 0));
    AOT_TEST_CHECK(error->getDirectOffset(program.vm(), program.vm().propertyNames->cause) == propertyOffsetOfOwnSlot(ErrorInstanceType, 1));
}

template<LoadedProgramTest body>
static constexpr void (*withUsesOfMathSign)() = runNativeTestWithLoadedProgram<builtinLocationWatchTests, usesOfMathSign, body>;

template<LoadedProgramTest body>
static constexpr void (*withUsesOfIntlCollator)() = runNativeTestWithLoadedProgram<builtinLocationWatchTests, usesOfIntlCollator, body>;

template<LoadedProgramTest body>
static constexpr void (*withReadsOfErrors)() = runNativeTestWithLoadedProgram<builtinLocationWatchTests, readsOfErrors, body>;

std::span<const NativeTest> builtinLocationWatchTests()
{
    static constexpr NativeTest tests[] = {
        { "builtin-location-watch/programs/replaced-method-is-called"_s, testReplacedMethodIsCalled },
        { "builtin-location-watch/programs/method-replaced-before-first-use-is-called"_s, testMethodReplacedBeforeFirstUseIsCalled },
        { "builtin-location-watch/programs/deleted-method-throws"_s, testDeletedMethodThrows },
        { "builtin-location-watch/programs/armed-on-first-use-only"_s, testArmedOnFirstUseOnly },
        { "builtin-location-watch/programs/member-that-host-makes-is-read-as-by-name"_s, testMemberThatHostMakesIsReadAsByName },
        { "builtin-location-watch/programs/member-replaced-before-host-makes-it-is-read"_s, testMemberReplacedBeforeHostMakesItIsRead },
        { "builtin-location-watch/programs/member-that-host-makes-is-armed-once-it-is-made"_s, testMemberThatHostMakesIsArmedOnceItIsMade },
        { "builtin-location-watch/programs/own-slots-of-errors-are-read-as-by-name"_s, testOwnSlotsOfErrorsAreReadAsByName },
        { "builtin-location-watch/nothing-is-watched-before-the-first-question"_s, withUsesOfMathSign<testNothingIsWatchedBeforeTheFirstQuestion> },
        { "builtin-location-watch/store-ends-the-watch-for-good"_s, withUsesOfMathSign<testStoreEndsTheWatchForGood> },
        { "builtin-location-watch/member-replaced-before-the-first-question-is-changed"_s, withUsesOfMathSign<testMemberReplacedBeforeTheFirstQuestionIsChanged> },
        { "builtin-location-watch/delete-ends-the-watch"_s, withUsesOfMathSign<testDeleteEndsTheWatch> },
        { "builtin-location-watch/members-that-cannot-be-compared-are-changed"_s, withUsesOfMathSign<testMembersThatCannotBeComparedAreChanged> },
        { "builtin-location-watch/question-makes-no-member"_s, withUsesOfIntlCollator<testQuestionMakesNoMember> },
        { "builtin-location-watch/member-made-before-the-first-question-is-watched"_s, withUsesOfIntlCollator<testMemberMadeBeforeTheFirstQuestionIsWatched> },
        { "builtin-location-watch/member-stored-before-host-makes-it-is-changed"_s, withUsesOfIntlCollator<testMemberStoredBeforeHostMakesItIsChanged> },
        { "builtin-location-watch/member-made-and-then-replaced-is-changed"_s, withUsesOfIntlCollator<testMemberMadeAndThenReplacedIsChanged> },
        { "builtin-location-watch/own-slot-has-the-structure-of-new-error"_s, withReadsOfErrors<testOwnSlotHasTheStructureOfNewError> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
