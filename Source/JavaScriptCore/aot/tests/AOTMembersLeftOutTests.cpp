/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTMembersLeftOut.h"
#include "AOTRealmTestSupport.h"
#include "Error.h"
#include "JSCInlines.h"
#include "TopExceptionScope.h"

namespace JSC { namespace AOT {

std::span<const NativeTest> membersLeftOutTests();

static constexpr ASCIILiteral sentence = "made was not compiled into this executable"_s;

static void refuse(Realms& realms)
{
    JSGlobalObject* realm = realms.plain();
    auto scope = DECLARE_THROW_SCOPE(realms.vm());
    JSObject* error = createError(realm, String(sentence));
    noteOmittedCodeError(realm, error);
    throwException(realm, scope, error);
}

static void layOnGlobalObject(Realms& realms, JSValue standIn)
{
    AOT_TEST_CHECK(standIn && standIn.isObject());
    realms.plain()->putDirect(realms.vm(), Identifier::fromString(realms.vm(), "made"_s), standIn);
}

static bool says(Realms& realms, ASCIILiteral source, ASCIILiteral expected)
{
    return realms.evaluate(realms.plain(), source).toWTFString(realms.plain()) == expected;
}

#define AOT_TEST_ATTEMPT(expression) "(() => { try { return String(" expression "); } catch (error) { return error.message; } })()"_s

static void checkWhatEveryStandInRefuses(Realms& realms)
{
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("typeof made"), "function"_s));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("made === made"), "true"_s));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("made instanceof Function"), "true"_s));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("typeof made.member"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("'member' in made"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("made[0]"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("made[Symbol.iterator]"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("typeof made.call"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("Object.hasOwn(made, 'member')"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("Object.getOwnPropertyDescriptor(made, 'member')"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("Object.keys(made)"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("Reflect.ownKeys(made)"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("(() => { for (const key in made) return key; })()"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("({ ...made })"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("`${made}`"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("made()"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("made.member = 1"), sentence));

    VM& vm = realms.vm();
    JSObject* standIn = asObject(realms.evaluate(realms.plain(), "made"_s));
    AOT_TEST_CHECK(isPlaceholderForOmittedBuiltin(standIn));
    PropertySlot inquiry(standIn, PropertySlot::InternalMethodType::VMInquiry, &vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    AOT_TEST_CHECK(!standIn->getPropertySlot(realms.plain(), Identifier::fromString(vm, "member"_s), inquiry));
    AOT_TEST_CHECK(!scope.exception());
}

static void testStandInForFunctionKnowsNameAndLengthAlone()
{
    Realms realms;
    refuse(realms);
    layOnGlobalObject(realms, takeOmittedFunctionPlaceholder(realms.plain(), { "made"_s, 2 }, realms.plain()));
    checkWhatEveryStandInRefuses(realms);
    AOT_TEST_CHECK(isOmittedFunctionPlaceholder(asObject(realms.evaluate(realms.plain(), "made"_s))));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("made.name"), "made"_s));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("made.length"), "2"_s));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("'prototype' in made"), "false"_s));
    AOT_TEST_CHECK(realms.throwsTypeError(realms.plain(), "new made()"_s));
}

static void testStandInForConstructorHasPrototype()
{
    Realms realms;
    refuse(realms);
    layOnGlobalObject(realms, takeOmittedFunctionPlaceholder(realms.plain(), { "made"_s, 0, true }, realms.plain()));
    checkWhatEveryStandInRefuses(realms);
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("made.name"), "made"_s));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("typeof made.prototype"), "object"_s));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("new made()"), sentence));
}

static void testStandInForFunctionThatConstructsWithoutPrototype()
{
    Realms realms;
    refuse(realms);
    layOnGlobalObject(realms, takeOmittedFunctionPlaceholder(realms.plain(), { "made"_s, 2, true, false }, realms.plain()));
    checkWhatEveryStandInRefuses(realms);
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("'prototype' in made"), "false"_s));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("new made()"), sentence));
}

static void testStandInForValueRefusesReads()
{
    Realms realms;
    refuse(realms);
    layOnGlobalObject(realms, takeOmittedMemberPlaceholder(realms.plain(), "made"_s));
    checkWhatEveryStandInRefuses(realms);
    AOT_TEST_CHECK(!isOmittedFunctionPlaceholder(asObject(realms.evaluate(realms.plain(), "made"_s))));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("made.name"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("made.length"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("'prototype' in made"), sentence));
    AOT_TEST_CHECK(says(realms, AOT_TEST_ATTEMPT("new made()"), sentence));
}

static void testInnermostMakingSaysWhetherLookOnlyTests()
{
    AOT_TEST_CHECK(!isReifyingForPresenceCheck());
    {
        MemberReificationScope test(true);
        AOT_TEST_CHECK(isReifyingForPresenceCheck());
        {
            MemberReificationScope read(false);
            AOT_TEST_CHECK(!isReifyingForPresenceCheck());
        }
        AOT_TEST_CHECK(isReifyingForPresenceCheck());
    }
    AOT_TEST_CHECK(!isReifyingForPresenceCheck());
}

static void testWhatIsNotedGoesWithItsVM()
{
    const VM* gone;
    {
        Realms realms;
        gone = &realms.vm();
        refuse(realms);
        layOnGlobalObject(realms, takeOmittedMemberPlaceholder(realms.plain(), "made"_s));
        AOT_TEST_CHECK(hasOmittedMemberNotes(gone));
    }
    AOT_TEST_CHECK(!hasOmittedMemberNotes(gone));
}

static void testOrdinaryFunctionStandsInForNothing()
{
    Realms realms;
    AOT_TEST_CHECK(!isPlaceholderForOmittedBuiltin(realms.object(realms.plain(), "(function () {})"_s)));
    AOT_TEST_CHECK(!isPlaceholderForOmittedBuiltin(realms.object(realms.plain(), "Array"_s)));
}

std::span<const NativeTest> membersLeftOutTests()
{
    static constexpr NativeTest tests[] = {
        { "members-left-out/stand-in-for-a-function-knows-its-name-and-length-alone"_s, testStandInForFunctionKnowsNameAndLengthAlone },
        { "members-left-out/stand-in-for-a-constructor-has-a-prototype"_s, testStandInForConstructorHasPrototype },
        { "members-left-out/stand-in-for-a-function-that-constructs-without-a-prototype"_s, testStandInForFunctionThatConstructsWithoutPrototype },
        { "members-left-out/stand-in-for-a-value-refuses-reads"_s, testStandInForValueRefusesReads },
        { "members-left-out/innermost-making-says-whether-the-look-only-tests"_s, testInnermostMakingSaysWhetherLookOnlyTests },
        { "members-left-out/what-is-noted-goes-with-its-vm"_s, testWhatIsNotedGoesWithItsVM },
        { "members-left-out/ordinary-function-stands-in-for-nothing"_s, testOrdinaryFunctionStandsInForNothing },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
