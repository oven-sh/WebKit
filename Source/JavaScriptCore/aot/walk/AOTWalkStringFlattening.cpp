/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Strings: FlattenString. A reader of characters in a loop takes the flat string that is made once in front of the loop, and the second reader on one way makes it for himself and all that follow.
// A flat string is no value of JavaScript and no saved frame keeps it: in front of a loop that suspends nothing can be built, and the note ends at a suspension. It throws only for lack of memory, so under a handler it stays out of the block in front.
// It calls what loops-3 (Walker::innermostOpenLoop() and its neighbors), loops-4 (outermostLoopToMakeTextInFrontOf()) and known-state-and-effects-2 (AOTWalkKnown.h) add, and asks the compiler whether they are there: without them every reader takes the string.
// Withheld (LoopInvariants, Folding): the same.

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTTypeArena.h"
#include <optional>
#include <span>

#if __has_include("AOTWalkKnown.h")
#include "AOTWalkKnown.h"
#define AOT_STRINGS_HAVE_NOTES_IN_STATE 1
#else
#define AOT_STRINGS_HAVE_NOTES_IN_STATE 0
#endif

namespace JSC { namespace AOT { namespace Typed {

#if AOT_STRINGS_HAVE_NOTES_IN_STATE

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

// True from the second reader on one way on.
static bool readerHasBeenHereForFlattening(Walker& walker, Reference string)
{
    if (findKnown(walker, KnownNote::FlatString, string, { }, readerHasBeenHereAmongNotesOfFlattening))
        return true;
    noteKnown(walker, KnownNote::FlatString, string, { }, readerHasBeenHereAmongNotesOfFlattening, { }, 1);
    return false;
}

#else

static Reference flatStringNotedForFlattening(Walker&, Reference) { return { }; }
static void noteFlatStringForFlattening(Walker&, Reference, Reference) { }
static bool readerHasBeenHereForFlattening(Walker&, Reference) { return false; }

#endif

static Draft draftOfFlatStringForFlattening(Reference string, Origin origin)
{
    origin.isTailCall = false;
    Draft flatten(Opcode::FlattenString, origin);
    flatten.setStringOperand(string);
    return flatten;
}

// What stands in front of the loop runs whenever the loop is entered, and a rare block mostly not at all.
template<typename WalkerWithLoops>
static Reference flattenInFrontOfLoopForFlattening(WalkerWithLoops& walker, Reference string, Origin origin)
{
    if constexpr (requires(Draft draft, Effects effects) { walker.innermostOpenLoop(); walker.standsInFrontOfOpenLoop(0u, string); walker.buildInFrontOfOpenLoop(0u, WTF::move(draft)); walker.outermostLoopToMakeTextInFrontOf(std::span<const Reference>(), effects); }) {
        const auto& here = walker.code().at(walker.currentBlock());
        if (!walker.has(Mechanism::LoopInvariants) || here.frequency == BlockFrequency::Rare || here.handler)
            return { };
        // Only a member of Walker sees the handler of the frame, whether the loop collects, and what the state has learned of the string in the loop.
        std::optional<uint32_t> loop = walker.outermostLoopToMakeTextInFrontOf(singleElementSpan(string), declaredBoundOf(Opcode::FlattenString));
        if (!loop)
            return { };
        return walker.buildInFrontOfOpenLoop(*loop, draftOfFlatStringForFlattening(string, origin));
    } else
        return { };
}

// The emitter keeps the raw pointer to the characters from the FlattenString to the last reader. None of them stands behind a suspension, where the text may have become a key and its old characters be freed:
// a loop that may suspend is never open, the note ends at a suspension, and the verifier of Code holds a flat string to the rule of the cells of a frame.
Reference flatStringInSight(Walker& walker, Reference string)
{
    if (!string.isValue())
        return { };
    if (walker.types().at(walker.typeOf(string)).isOpaque(OpaqueKind::FlatString))
        return string;
    return flatStringNotedForFlattening(walker, string);
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

#undef AOT_STRINGS_HAVE_NOTES_IN_STATE

#endif // ENABLE(AOT)
