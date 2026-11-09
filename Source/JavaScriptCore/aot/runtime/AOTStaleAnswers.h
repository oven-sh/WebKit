/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSCJSValue.h"
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC { namespace AOT {

struct Instance;

enum class KindOfAnswer : uint8_t { Value, Absent, Accessor, Present, AbsentFromObjectItself };

enum class StubThatAnswers : uint8_t { CallMethodAtSite, FindsNoExtraPropertyAtSite, ReadByName, ReadByNameUnlessAsMade, TruthByName, PresenceByName, PresenceOfOwnName, GetterOfPendingRead, CachedGetterByName, ProofThatLookupFindsNothing, TypedReadByName, TypedPresenceOfOwnName, StateOfWalk, FunctionThatNameFinds, StoreIntoSlot, DoorOfPlainInstance, HolderWithoutAddedNames, ChainAbovePrototype, MemberAsBuilt };

constexpr uint32_t kindAndStubOfAnswer(KindOfAnswer kind, StubThatAnswers stub) { return static_cast<uint32_t>(kind) | static_cast<uint32_t>(stub) << 8; }
constexpr uint32_t answerMayComeFromSlowPath = 1 << 16;
constexpr uint32_t emptySlotAnswersNothingByItself = 1 << 17;
constexpr uint32_t accessorIsNoAnswerOfWalk = 1 << 18;
constexpr uint32_t answerIsYesOfQuestionOfLibrary = 1 << 19;

void stopAtStaleAnswer(Instance&, JSValue receiver, UniquedStringImpl* name, KindOfAnswer, JSValue value, ASCIILiteral whoAnswered);
void stopAtStaleGetterOfPendingRead(Instance&, JSValue receiver, JSValue accessorOrGetter);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
