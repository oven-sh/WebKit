/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"
#include "AOTWalkStrings.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet callsThatGiveBoundsOfParts { Opcode::BoundsOfRegExpMatch, Opcode::BoundsOfStringSplit };
inline constexpr uint32_t firstBoundOfParts = 0;

bool buildHeldRegExpMatchInParts(Walker&, const BodyValue& call, BuiltinOverloadID, const HeldRegExp&, Reference regExpObject, Reference string, Origin);
Reference findLastIndexOfMatch(Walker&, Reference regExpObject, PropertyKeyID);
void noteMatchStoresLastIndex(Walker&, const HeldRegExp&, Reference regExpObject);
bool buildLastIndexOfMatch(Walker&, const BodyValue& read);
bool buildMatchIntoFrame(Walker&, const BodyValue& call, BuiltinOverloadID, const HeldRegExp*, Reference regExpObject, Reference string, Origin);
ValueID findLengthReadAfterReplace(const Walker&, const BodyValue& call);
bool buildLengthWithoutMatches(Walker&, const BodyValue& call, BuiltinOverloadID, Reference string, Reference regExpObject, Origin);
bool buildFrameMatchElement(Walker&, const BodyValue& read, const InputsOfTypedOperation&);
bool buildMatchInPartsByCodeOfRegExp(Walker&, const BodyValue& call, BuiltinOverloadID, const RegExpWrittenForCall&, Reference objectOfCall, Reference string, Origin);
Reference buildPlacesReadOfMatch(Walker&, const BodyValue& call, BuiltinOverloadID, const RegExpWrittenForCall&, Reference objectOfCall, Reference string, Origin);
bool buildSplitInParts(Walker&, const CallByOneRow&);
bool openMatchIteration(Walker&, const CallByOneRow& matchAll);
bool walkMatchIteration(Walker&, const BodyValue&);
bool namesMatchAll(Walker&, PropertyKeyID);
bool matchesAreSteppedByExec(Walker&, StringID pattern, OptionSet<RegExpFlags>);
bool loopTakesMatchesApart(Walker&, ValueID step);
void stepOverMatchesOfString(Walker&, Reference string, Reference regExpObject, Origin, const ScopedLambda<void()>& finish, const ScopedLambda<void(Reference)>& give);
void stepOverPiecesOfString(Walker&, Reference string, Reference separator, Reference position, Origin, const ScopedLambda<void()>& finish, const ScopedLambda<void(Reference, Reference)>& give);
Reference partOfListThatIsOnlyTakenApart(Walker&, Reference name, Reference key);

Reference propertyOfResultTakenInParts(Walker&, Reference name, PropertyKeyID);
std::optional<Vector<Reference, 4>> partsThatPatternTakes(Walker&, Reference iterable, uint32_t numberOfSteps);
bool boundsAreOfOnePart(const Walker&, Reference start, Reference end);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
