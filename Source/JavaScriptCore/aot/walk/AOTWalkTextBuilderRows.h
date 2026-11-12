/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

bool joinOfWhatIsInSight(Walker&, const BodyValue& call, BuiltinID called);
bool takePartsOfConvertedList(Walker&, Reference list, Vector<Reference, 8>& parts);

bool holdsPartsTakenOutOfTexts(const Walker&, const BodyValue& join, std::span<const Reference> parts);
inline constexpr OpcodeSet opcodesThatGuessThatOperandBesideStringHasTextTakes { Opcode::Add };
AOT_LINK_OF_WALK(guessThatOperandBesideStringHasText, opcodesThatGuessThatOperandBesideStringHasTextTakes)
inline constexpr OpcodeSet opcodesThatGuessThatConvertedPartOfTextInBuilderHasTextTakes { Opcode::ToString };
AOT_LINK_OF_WALK(guessThatConvertedPartOfTextInBuilderHasText, opcodesThatGuessThatConvertedPartOfTextInBuilderHasTextTakes)

bool isReadByNoRecordSoFar(const Walker&, Reference made);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
