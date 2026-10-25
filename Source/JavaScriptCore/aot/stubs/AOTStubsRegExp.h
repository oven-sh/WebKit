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
// The result of operationAOTRegExpExecPrefix and of operationAOTStringMatchPrefix when nothing matched.
constexpr uint32_t noMatchPrefixLength = UINT32_MAX;
// The third argument of operationAOTRegExpMatchLiteral when the RegExp is of unknown origin.
constexpr uint32_t noRegExpRecord = UINT32_MAX;

constexpr StubKey regExpMatch(RegExpMatchResult result) { return StubKey(StubFamily::RegExpMatch).withForm(static_cast<uint8_t>(result)); }
// Takes the RegExpObject and the subject, which is no rope, of a match whose offsets are in VMState::regExpMatchOutput. Answers what exec answers and leaves behind what exec leaves behind.
constexpr StubKey finishRegExpExec(FinishRegExpExecForm form) { return StubKey(StubFamily::FinishRegExpExec).withForm(static_cast<uint8_t>(form)); }
constexpr StubKey stringMatchRegExp() { return StubKey(StubFamily::StringMatchRegExp); }
constexpr StubKey stringReplaceRegExpMayNotMatch() { return StubKey(StubFamily::StringReplaceRegExpMayNotMatch); }
constexpr StubKey newRegExpLiteral() { return StubKey(StubFamily::NewRegExpLiteral); }
// The immediate: RegExpRecordID::index(). The catalog binds every one. A record serves the literal with and without the flag g, so the site says which it has.
constexpr StubKey regExpMatchLiteral(Typed::RegExpMatchForm form, bool isGlobal) { return StubKey(StubFamily::RegExpMatchLiteral).withForm(static_cast<uint8_t>(static_cast<unsigned>(form) + (isGlobal ? numberOfEnumerators<Typed::RegExpMatchForm> : 0))); }
// Takes the string alone: for a literal without the flags g, y and d in a program that never reads what the last match left in RegExp.
constexpr StubKey regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm form) { return StubKey(StubFamily::RegExpMatchLiteral).withForm(static_cast<uint8_t>(static_cast<unsigned>(form) + 2 * numberOfEnumerators<Typed::RegExpMatchForm>)); }
constexpr Typed::RegExpMatchForm matchFormOf(StubKey key) { return static_cast<Typed::RegExpMatchForm>(key.form() % numberOfEnumerators<Typed::RegExpMatchForm>); }
constexpr bool matchesGlobalLiteral(StubKey key) { return key.form() / numberOfEnumerators<Typed::RegExpMatchForm> == 1; }
constexpr bool takesRegExpObject(StubKey key) { return key.form() < 2 * numberOfEnumerators<Typed::RegExpMatchForm>; }

static_assert(exists(regExpMatch(RegExpMatchResult::Array)) && exists(regExpMatch(RegExpMatchResult::BoxedBoolean)));
static_assert(!exists(regExpMatch(RegExpMatchResult::BoxedBoolean).withForm(static_cast<uint8_t>(RegExpMatchResult::BoxedBoolean) + 1)));
static_assert(exists(finishRegExpExec(FinishRegExpExecForm::WithObject)) && exists(finishRegExpExec(FinishRegExpExecForm::WithoutObject)) && exists(stringMatchRegExp()) && exists(stringReplaceRegExpMayNotMatch()) && exists(newRegExpLiteral()));
static_assert(descriptorOf(StubFamily::RegExpMatchLiteral).numberOfForms == 3 * numberOfEnumerators<Typed::RegExpMatchForm>);
static_assert(exists(regExpMatchLiteral(Typed::RegExpMatchForm::Test, false)) && exists(regExpMatchLiteral(Typed::RegExpMatchForm::Match, true).withImmediate(0)));
static_assert(matchFormOf(regExpMatchLiteral(Typed::RegExpMatchForm::Match, true)) == Typed::RegExpMatchForm::Match && matchesGlobalLiteral(regExpMatchLiteral(Typed::RegExpMatchForm::Test, true)) && !matchesGlobalLiteral(regExpMatchLiteral(Typed::RegExpMatchForm::Match, false)));
static_assert(exists(regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Match).withImmediate(0)) && matchFormOf(regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Search)) == Typed::RegExpMatchForm::Search);
static_assert(!takesRegExpObject(regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Test)) && !matchesGlobalLiteral(regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Test)) && takesRegExpObject(regExpMatchLiteral(Typed::RegExpMatchForm::Match, true)));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
