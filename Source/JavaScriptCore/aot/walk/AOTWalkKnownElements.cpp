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

bool isKnownToLieInsideList(Walker& walker, Reference list, Reference position)
{
    if (isTakenToLieInsideInKnownElements(walker, position))
        return true;
    if (!position || !walker.has(Mechanism::TypedOperations) || walker.state().answers().isEmpty())
        return false;
    Reference length = recordNotedInKnownElements(findKnown(walker, KnownNote::LengthOfList, list));
    return length && isKnownToLieInside(walker, position, length);
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
