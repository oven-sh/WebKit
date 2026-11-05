/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkKnownEffects.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include <algorithm>
#include <bit>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static uint64_t hashForDistinctEffects(const Effects& effects)
{
    uint64_t hash = EffectTraits(effects).bits() | static_cast<uint64_t>(effects.mayThrowOtherThanStackOverflow()) << EffectTraits::numberOfBits;
    for (const LocationMasks* masks : { &effects.reads, &effects.writes }) {
        hash = (std::rotr(hash, 32) ^ masks->heaps().toRaw()) * 0x9e3779b97f4a7c15ull;
        for (AbstractHeap heap : indexedHeapsInOrder)
            hash = (std::rotr(hash, 32) ^ masks->maskOf(heap)) * 0x9e3779b97f4a7c15ull;
    }
    return hash;
}

const Effects*& DistinctEffects::placeOf(const Effects& effects)
{
    size_t mask = m_places.size() - 1;
    size_t place = (hashForDistinctEffects(effects) >> 32) & mask;
    while (m_places[place] && *m_places[place] != effects)
        place = (place + 1) & mask;
    return m_places[place];
}

void DistinctEffects::makeRoomInPlaces()
{
    Vector<const Effects*> outgrown = std::exchange(m_places, { });
    m_places.fill(nullptr, std::max(fewestPlaces, outgrown.size() * 2));
    for (const Effects* kept : outgrown) {
        if (kept)
            placeOf(*kept) = kept;
    }
}

const Effects* DistinctEffects::keep(const Effects& effects)
{
    if (effects == effectsOfUnknownCode)
        return &effectsOfUnknownCode;
    RELEASE_ASSERT(effects.reads.masksAgreeWithHeaps() && effects.writes.masksAgreeWithHeaps());
    if ((m_numberKept + 1) * 2 > m_places.size())
        makeRoomInPlaces();
    const Effects*& place = placeOf(effects);
    if (place)
        return place;
    if (m_blocks.isEmpty() || m_blocks.last().size() == effectsInBlock) {
        m_blocks.append(Vector<Effects>());
        m_blocks.last().reserveInitialCapacity(effectsInBlock);
    }
    m_blocks.last().append(effects);
    place = &m_blocks.last().last();
    ++m_numberKept;
    return place;
}

Vector<Vector<Effects>> DistinctEffects::takeBlocks()
{
    m_places = { };
    m_numberKept = 0;
    return std::exchange(m_blocks, { });
}

uint64_t bitOfLayoutInKnownEffects(const Program& program, LayoutID layout)
{
    size_t stepsLeft = program.layouts.size();
    LayoutID top = layout;
    for (LayoutID above = writtenBaseOf(program.at(top)); above; above = writtenBaseOf(program.at(top))) {
        if (!stepsLeft--)
            return objectsOfAnyLayout;
        top = above;
    }
    std::optional<uint8_t> bit = program.locationBitOf(top);
    return bit ? uint64_t { 1 } << (*bit & 63) : objectsOfAnyLayout;
}

Vector<uint64_t> bitsOfLayoutsInKnownEffects(const Program& program)
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    Vector<uint64_t> bits;
    bits.fill(0, numberOfLayouts);
    BitVector isOnTheWay(numberOfLayouts);
    Vector<uint32_t, 32> row;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        uint64_t found = 0;
        for (LayoutID current(index); !found;) {
            LayoutID above = writtenBaseOf(program.at(current));
            if (bits[current.index()])
                found = bits[current.index()];
            else if (isOnTheWay.quickSet(current.index()))
                found = objectsOfAnyLayout;
            else {
                row.append(current.index());
                if (above)
                    current = above;
                else
                    found = bitOfLayoutInKnownEffects(program, current);
            }
        }
        for (uint32_t layout : row)
            bits[layout] = found;
        row.shrink(0);
    }
    return bits;
}

uint64_t LinkKnownEffects::layoutsWrittenBy(FunctionID function) const
{
    return function.index() < layoutsWrittenByFunction.size() ? layoutsWrittenByFunction[function.index()] : objectsOfAnyLayout;
}

const LinkedEffectsGivenArguments* LinkKnownEffects::findEffectsGivenArguments(FunctionID function) const
{
    auto found = std::ranges::lower_bound(givenArguments, function, { }, &LinkedEffectsGivenArguments::function);
    return found != givenArguments.end() && found->function == function ? &*found : nullptr;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
