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

bool buildMatchInPartsByCodeOfRegExp(Walker&, const BodyValue& call, BuiltinOverloadID, const RegExpWrittenForCall&, Reference objectOfCall, Reference string, Origin);
bool buildSplitInParts(Walker&, const CallByOneRow&);
Reference partOfListThatIsOnlyTakenApart(Walker&, Reference name, Reference key);

Reference propertyOfResultTakenInParts(Walker&, Reference name, PropertyKeyID);
std::optional<Vector<Reference, 4>> partsThatPatternTakes(Walker&, Reference iterable, uint32_t numberOfSteps);
bool boundsAreOfOnePart(const Walker&, Reference start, Reference end);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
