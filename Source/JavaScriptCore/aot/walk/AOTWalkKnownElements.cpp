/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkKnown.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

static Reference recordNotedInKnownElements(std::optional<uint64_t> noted)
{
    return noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
}

static bool isTakenToLieInsideInKnownElements(Walker& walker, Reference position)
{
    TypeID type = position && walker.takesPositionsToLieInside() ? walker.typeOf(position) : TypeID();
    return type && walker.types().at(type).isInteger() && walker.types().at(type).integers.minimum >= 0;
}

std::optional<PassedTest> Walker::findWhatHoldsPositionInsideList(Reference list, Reference position)
{
    if (isTakenToLieInsideInKnownElements(*this, position))
        return PassedTest();
    if (!position || !has(Mechanism::TypedOperations) || m_state.answers().isEmpty())
        return std::nullopt;
    Reference length = recordNotedInKnownElements(findKnown(*this, KnownNote::LengthOfList, list));
    std::optional<PassedTest> fromZeroUp = length ? findWhatHoldsFromZeroUp(position) : std::nullopt;
    std::optional<PassedTest> below = fromZeroUp ? findWhatHoldsBelow(position, length) : std::nullopt;
    return below ? std::optional(fromZeroUp->with(*below)) : std::nullopt;
}

bool isKnownToLieInsideList(Walker& walker, Reference list, Reference position)
{
    return !!walker.findWhatHoldsPositionInsideList(list, position);
}

std::optional<Effects> effectsOfAppendToListAsMade(Walker& walker, const Draft& append)
{
    static constexpr OpcodeSet appendsThatRunNoCode { Opcode::AppendElements, Opcode::AppendMembers };
    if (!appendsThatRunNoCode.contains(append.opcode) || !walker.has(Mechanism::TypedOperations) || !isFreshListAsMade(walker, append.arrayOperand()))
        return std::nullopt;
    Effects effects = declaredEffectsOfOperation(walker.program(), append.opcode, append);
    if (effects.mayRunOtherCode())
        return std::nullopt;
    effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    return effects;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
