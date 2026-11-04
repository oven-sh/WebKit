/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubDescriptor.h"

namespace JSC { namespace AOT {

// Found answers like Boolean and leaves the offsets of the match in the RegExp's own vector (RegExp::offsetOfOvector()).
enum class RegExpMatchResult : uint8_t { Array, Boolean, Found, BoxedBoolean };
// WithoutObject is for a literal that is matched without its object, so that the match leaves nothing behind: RegExpRecordID::index() stands in place of the object, and a third operand has the number of groups, none of which has a name.
enum class FinishRegExpExecForm : uint8_t { WithObject, WithoutObject };

// The third operand of newRegExpLiteral and of operationAOTNewRegExpLiteral when the statics have no cell for the literal.
constexpr uint32_t noSharedRegExpOffset = UINT32_MAX;
// The third argument of operationAOTRegExpMatchLiteral when the RegExp is of unknown origin. That operation searches nothing: like finishRegExpExec, it finishes a match whose offsets are in VMState::regExpMatchOutput.
constexpr uint32_t noRegExpRecord = UINT32_MAX;

constexpr StubKey regExpMatch(RegExpMatchResult result) { return StubKey(StubFamily::RegExpMatch).withForm(static_cast<uint8_t>(result)); }
// Takes the RegExpObject and the subject, which is no rope, of a match whose offsets are in VMState::regExpMatchOutput. Answers what exec answers and leaves behind what exec leaves behind.
constexpr StubKey finishRegExpExec(FinishRegExpExecForm form) { return StubKey(StubFamily::FinishRegExpExec).withForm(static_cast<uint8_t>(form)); }
constexpr StubKey stringReplaceRegExpMayNotMatch() { return StubKey(StubFamily::StringReplaceRegExpMayNotMatch); }
// Takes RegExpRecordID::index(), the flags, the offset of the literal's cell in the statics and what that cell holds. Allocates in line once the cell is filled, which operationAOTNewRegExpLiteral does.
constexpr StubKey newRegExpLiteral() { return StubKey(StubFamily::NewRegExpLiteral); }
// The immediate: RegExpRecordID::index(), in its register: one stub serves every literal, and asks the record for the code.
// Takes the string alone and leaves no record of the match: for a literal without the flags g, y and d, and only where Facts::mayReadLegacyRegExpStatics() is false.
constexpr StubKey regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm form) { return StubKey(StubFamily::RegExpMatchLiteral).withForm(static_cast<uint8_t>(form)); }
// Takes the RegExpObject of the literal, which is as created, and the string. A record serves the literal with and without the flag g, so the site says which it has.
// recordsMatch: Facts::mayReadLegacyRegExpStatics(). Where it is false, test and search leave no record of the match.
constexpr StubKey regExpMatchLiteral(Typed::RegExpMatchForm form, bool isGlobal, bool recordsMatch)
{
    unsigned group = 1 + (isGlobal ? 1 : 0) + (recordsMatch ? 2 : 0);
    return StubKey(StubFamily::RegExpMatchLiteral).withForm(static_cast<uint8_t>(static_cast<unsigned>(form) + group * numberOfEnumerators<Typed::RegExpMatchForm>));
}
constexpr Typed::RegExpMatchForm matchFormOf(StubKey key) { return static_cast<Typed::RegExpMatchForm>(key.form() % numberOfEnumerators<Typed::RegExpMatchForm>); }
constexpr bool takesRegExpObject(StubKey key) { return key.form() >= numberOfEnumerators<Typed::RegExpMatchForm>; }
constexpr bool matchesGlobalLiteral(StubKey key) { return takesRegExpObject(key) && ((key.form() / numberOfEnumerators<Typed::RegExpMatchForm> - 1) & 1); }
constexpr bool recordsMatchOfLiteral(StubKey key) { return takesRegExpObject(key) && ((key.form() / numberOfEnumerators<Typed::RegExpMatchForm> - 1) & 2); }

static_assert(exists(regExpMatch(RegExpMatchResult::Array)) && exists(regExpMatch(RegExpMatchResult::BoxedBoolean)));
static_assert(!exists(regExpMatch(RegExpMatchResult::BoxedBoolean).withForm(static_cast<uint8_t>(RegExpMatchResult::BoxedBoolean) + 1)));
static_assert(exists(finishRegExpExec(FinishRegExpExecForm::WithObject)) && exists(finishRegExpExec(FinishRegExpExecForm::WithoutObject)) && exists(stringReplaceRegExpMayNotMatch()) && exists(newRegExpLiteral()));
static_assert(descriptorOf(StubFamily::RegExpMatchLiteral).numberOfForms == 5 * numberOfEnumerators<Typed::RegExpMatchForm>);
static_assert(exists(regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Match)) && matchFormOf(regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Search)) == Typed::RegExpMatchForm::Search);
static_assert(!takesRegExpObject(regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Match)) && !matchesGlobalLiteral(regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Match)) && !recordsMatchOfLiteral(regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Match)));
static_assert(exists(regExpMatchLiteral(Typed::RegExpMatchForm::Match, true, true)) && takesRegExpObject(regExpMatchLiteral(Typed::RegExpMatchForm::Test, false, false)) && matchFormOf(regExpMatchLiteral(Typed::RegExpMatchForm::Match, true, true)) == Typed::RegExpMatchForm::Match);
static_assert(matchesGlobalLiteral(regExpMatchLiteral(Typed::RegExpMatchForm::Test, true, false)) && !recordsMatchOfLiteral(regExpMatchLiteral(Typed::RegExpMatchForm::Test, true, false)));
static_assert(!matchesGlobalLiteral(regExpMatchLiteral(Typed::RegExpMatchForm::Match, false, true)) && recordsMatchOfLiteral(regExpMatchLiteral(Typed::RegExpMatchForm::Match, false, true)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
