/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"

namespace JSC { namespace AOT {

enum class RegExpMatchResult : uint8_t { Array, Boolean, BoxedBoolean, ArrayForStringMatch, IndexForStringSearch, Parts, Record };
enum class FinishRegExpExecForm : uint8_t { WithObject, WithoutObject, OnSubstringWithObject, OnSubstringWithoutObject };
constexpr bool finishesWithObject(FinishRegExpExecForm form) { return form == FinishRegExpExecForm::WithObject || form == FinishRegExpExecForm::OnSubstringWithObject; }
constexpr bool finishesOnSubstring(FinishRegExpExecForm form) { return form == FinishRegExpExecForm::OnSubstringWithObject || form == FinishRegExpExecForm::OnSubstringWithoutObject; }
enum class ReplacementOfMatches : uint8_t { TextOrFunction, AnyValue };

constexpr uint32_t noSharedRegExpOffset = UINT32_MAX;
constexpr uint32_t noRegExpRecord = UINT32_MAX;

constexpr StubKey regExpMatch(RegExpMatchResult result) { return StubKey(StubFamily::RegExpMatch).withForm(static_cast<uint8_t>(result)); }
constexpr bool takesStringInFrontOfRegExp(RegExpMatchResult result) { return result == RegExpMatchResult::ArrayForStringMatch || result == RegExpMatchResult::IndexForStringSearch; }
constexpr StubKey finishRegExpExec(FinishRegExpExecForm form) { return StubKey(StubFamily::FinishRegExpExec).withForm(static_cast<uint8_t>(form)); }
constexpr StubKey stringReplaceRegExpMayNotMatch(ReplacementOfMatches replacement = ReplacementOfMatches::TextOrFunction) { return StubKey(StubFamily::StringReplaceRegExpMayNotMatch).withForm(static_cast<uint8_t>(replacement)); }
constexpr StubKey newRegExpLiteral() { return StubKey(StubFamily::NewRegExpLiteral); }
constexpr StubKey regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm form) { return StubKey(StubFamily::RegExpMatchLiteral).withForm(static_cast<uint8_t>(form)); }
constexpr StubKey regExpMatchLiteral(Typed::RegExpMatchForm form, bool isGlobal, bool recordsMatch)
{
    unsigned group = 1 + (isGlobal ? 1 : 0) + (recordsMatch ? 2 : 0);
    return StubKey(StubFamily::RegExpMatchLiteral).withForm(static_cast<uint8_t>(static_cast<unsigned>(form) + group * numberOfEnumerators<Typed::RegExpMatchForm>));
}
constexpr Typed::RegExpMatchForm matchFormOf(StubKey key) { return static_cast<Typed::RegExpMatchForm>(key.form() % numberOfEnumerators<Typed::RegExpMatchForm>); }
constexpr bool takesRegExpObject(StubKey key) { return key.form() >= numberOfEnumerators<Typed::RegExpMatchForm>; }
constexpr bool matchesGlobalLiteral(StubKey key) { return takesRegExpObject(key) && ((key.form() / numberOfEnumerators<Typed::RegExpMatchForm> - 1) & 1); }
constexpr bool recordsMatchOfLiteral(StubKey key) { return takesRegExpObject(key) && ((key.form() / numberOfEnumerators<Typed::RegExpMatchForm> - 1) & 2); }

static_assert(exists(regExpMatch(RegExpMatchResult::Array)) && exists(regExpMatch(RegExpMatchResult::IndexForStringSearch)));
static_assert(exists(regExpMatch(RegExpMatchResult::Record)) && !exists(regExpMatch(RegExpMatchResult::Record).withForm(static_cast<uint8_t>(RegExpMatchResult::Record) + 1)));
static_assert(exists(regExpMatchLiteral(Typed::RegExpMatchForm::Parts, true, true)) && matchFormOf(regExpMatchLiteral(Typed::RegExpMatchForm::Parts, true, true)) == Typed::RegExpMatchForm::Parts && !takesRegExpObject(regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Parts)));
static_assert(exists(finishRegExpExec(FinishRegExpExecForm::WithObject)) && exists(finishRegExpExec(FinishRegExpExecForm::WithoutObject)) && exists(finishRegExpExec(FinishRegExpExecForm::OnSubstringWithObject)) && exists(finishRegExpExec(FinishRegExpExecForm::OnSubstringWithoutObject)) && exists(stringReplaceRegExpMayNotMatch()) && exists(stringReplaceRegExpMayNotMatch(ReplacementOfMatches::AnyValue)) && exists(newRegExpLiteral()));
static_assert(descriptorOf(StubFamily::RegExpMatchLiteral).numberOfForms == 5 * numberOfEnumerators<Typed::RegExpMatchForm>);
static_assert(exists(regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Match)) && matchFormOf(regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Search)) == Typed::RegExpMatchForm::Search);
static_assert(!takesRegExpObject(regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Match)) && !matchesGlobalLiteral(regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Match)) && !recordsMatchOfLiteral(regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Match)));
static_assert(exists(regExpMatchLiteral(Typed::RegExpMatchForm::Match, true, true)) && takesRegExpObject(regExpMatchLiteral(Typed::RegExpMatchForm::Test, false, false)) && matchFormOf(regExpMatchLiteral(Typed::RegExpMatchForm::Match, true, true)) == Typed::RegExpMatchForm::Match);
static_assert(matchesGlobalLiteral(regExpMatchLiteral(Typed::RegExpMatchForm::Test, true, false)) && !recordsMatchOfLiteral(regExpMatchLiteral(Typed::RegExpMatchForm::Test, true, false)));
static_assert(!matchesGlobalLiteral(regExpMatchLiteral(Typed::RegExpMatchForm::Match, false, true)) && recordsMatchOfLiteral(regExpMatchLiteral(Typed::RegExpMatchForm::Match, false, true)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
