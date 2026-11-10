/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTTypeArena.h"
#include <optional>
#include <span>

#include "AOTWalkKnown.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t flatStringAmongNotesOfFlattening = 0;
static constexpr uint32_t readerHasBeenHereAmongNotesOfFlattening = 1;

static Reference flatStringNotedForFlattening(Walker& walker, Reference string)
{
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::FlatString, string, { }, flatStringAmongNotesOfFlattening);
    return noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
}

static void noteFlatStringForFlattening(Walker& walker, Reference string, Reference flat)
{
    noteKnown(walker, KnownNote::FlatString, string, { }, flatStringAmongNotesOfFlattening, { }, flat.bits(), true);
}

static bool readerHasBeenHereForFlattening(Walker& walker, Reference string)
{
    if (findKnown(walker, KnownNote::FlatString, string, { }, readerHasBeenHereAmongNotesOfFlattening))
        return true;
    noteKnown(walker, KnownNote::FlatString, string, { }, readerHasBeenHereAmongNotesOfFlattening, { }, 1);
    return false;
}

static Draft draftOfFlatStringForFlattening(Reference string, Origin origin)
{
    origin.isTailCall = false;
    Draft flatten(Opcode::FlattenString, origin);
    flatten.setStringOperand(string);
    return flatten;
}

static Reference flattenInFrontOfLoopForFlattening(Walker& walker, Reference string, Origin origin)
{
    const auto& here = walker.code().at(walker.currentBlock());
    if (!walker.has(Mechanism::LoopInvariants) || here.frequency == BlockFrequency::Rare || here.handler)
        return { };
    std::optional<uint32_t> loop = walker.outermostLoopToMakeTextInFrontOf(singleElementSpan(string), declaredBoundOf(Opcode::FlattenString));
    if (!loop)
        return { };
    Draft flatten = draftOfFlatStringForFlattening(string, origin);
    if (Reference begins = walker.buildLoopEntryCondition(*loop))
        flatten.setIsReadOperand(begins);
    return walker.buildInFrontOfOpenLoop(*loop, WTF::move(flatten));
}

Reference flatStringInSight(Walker& walker, Reference string)
{
    if (!string.isValue())
        return { };
    if (walker.types().at(walker.typeOf(string)).isOpaque(OpaqueKind::FlatString))
        return string;
    Reference flat = flatStringNotedForFlattening(walker, string);
    return flat && walker.isInSightWhereWalkBuilds(flat) ? flat : Reference();
}

Reference flatStringMadeWhereWalkStands(Walker& walker, Reference string, Origin origin)
{
    if (walker.isUnreachable() || !string.isValue() || !isProvenString(walker, walker.typeOf(string)))
        return { };
    if (Reference flat = flatStringInSight(walker, string))
        return flat;
    Reference flat = walker.build(draftOfFlatStringForFlattening(string, origin));
    if (flat)
        noteFlatStringForFlattening(walker, string, flat);
    return flat;
}

Reference flatStringForReaderOfCharacters(Walker& walker, Reference string, Origin origin)
{
    if (walker.isUnreachable() || !string.isValue())
        return { };
    if (Reference flat = flatStringInSight(walker, string))
        return flat;
    if (!isProvenString(walker, walker.typeOf(string)))
        return { };

    Reference flat = flattenInFrontOfLoopForFlattening(walker, string, origin);
    if (!flat && readerHasBeenHereForFlattening(walker, string))
        flat = walker.build(draftOfFlatStringForFlattening(string, origin));
    if (flat)
        noteFlatStringForFlattening(walker, string, flat);
    return flat;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
