/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

uint32_t WriterOfLinkLayouts::findMakingInFrontOfRun(uint32_t place)
{
    static constexpr uint32_t notAsked = 0;
    Vector<uint32_t, 8> runsGoneThrough;
    uint32_t making = noMakingInFrontOfRun;
    for (uint32_t current = place;;) {
        uint32_t& known = m_makingsInFrontOfRuns[current];
        if (known != notAsked) {
            making = known;
            break;
        }
        known = noMakingInFrontOfRun;
        runsGoneThrough.append(current);
        const RunOfMaker& run = m_runsOfMakers[current];
        if (run.isMadeHere) {
            making = current + 1;
            break;
        }
        uint32_t inFront = run.givenBy && !m_runsOnWhatFunctionsReturn.isEmpty() ? m_runsOnWhatFunctionsReturn[run.givenBy.index()] : 0;
        if (!inFront)
            break;
        current = inFront - 1;
    }
    for (uint32_t passed : runsGoneThrough)
        m_makingsInFrontOfRuns[passed] = making;
    return making;
}

void WriterOfLinkLayouts::noteNamesThatFollowersOfMakersStore()
{
    m_makingsInFrontOfRuns.fill(0, m_runsOfMakers.size());
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numbersOfLayoutsNotedUnderKey;
    auto note = [&](LayoutID layout, PropertyKeyID key) {
        auto found = m_pairsNotListed.find(static_cast<uint64_t>(layout.index()) << 32 | key.bits());
        if (found != m_pairsNotListed.end() && !std::exchange(m_namesNotListed[found->value].isStoredByFollowerOfMaker, true))
            ++numbersOfLayoutsNotedUnderKey.add(key.bits(), 0).iterator->value;
    };
    struct KeysStoredIntoObjectThatLinkHasLost {
        std::span<const PropertyKeyID> keys;
        uint64_t keysOnlyGivenUndefined { 0 };
    };
    Vector<KeysStoredIntoObjectThatLinkHasLost> runsOnObjectsThatLinkHasLost;
    for (uint32_t place = 0; place < m_runsOfMakers.size(); ++place) {
        const RunOfMaker& run = m_runsOfMakers[place];
        if (run.keys.isEmpty())
            continue;
        uint32_t making = findMakingInFrontOfRun(place);
        if (making == noMakingInFrontOfRun)
            continue;
        const LayoutsOfRun& layouts = run.layouts.isEmpty() ? m_runsOfMakers[making - 1].layouts : run.layouts;
        for (LayoutID layout : layouts) {
            for (size_t index = 0; index < run.keys.size(); ++index) {
                if (index >= 64 || !(run.keysOnlyGivenUndefined >> index & 1))
                    note(layout, run.keys[index]);
            }
        }
        if (layouts.isEmpty() && run.isOnObjectThatLinkHasLost)
            runsOnObjectsThatLinkHasLost.append({ run.keys.span(), run.keysOnlyGivenUndefined });
    }
    HashMap<uint64_t, bool, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> runsBehindMakerThatFillsField;
    Vector<uint64_t, 8> runsGoneThrough;
    for (const RunOnObjectInField& run : m_runsOnObjectsInFields) {
        if (run.keys.isEmpty())
            continue;
        uint32_t onHolder = run.runOnHolder;
        if (!onHolder && run.givenBy && !m_runsOnWhatFunctionsReturn.isEmpty())
            onHolder = m_runsOnWhatFunctionsReturn[run.givenBy.index()];
        if (!onHolder || findMakingInFrontOfRun(onHolder - 1) == noMakingInFrontOfRun)
            continue;
        bool holdsObjectMadeForIt = false;
        runsGoneThrough.shrink(0);
        for (uint32_t current = onHolder; current && !holdsObjectMadeForIt;) {
            uint64_t asked = static_cast<uint64_t>(current) << 32 | run.keyInHolder.bits();
            if (auto known = runsBehindMakerThatFillsField.find(asked); known != runsBehindMakerThatFillsField.end()) {
                holdsObjectMadeForIt = known->value;
                break;
            }
            runsGoneThrough.append(asked);
            const RunOfMaker& inFront = m_runsOfMakers[current - 1];
            if (size_t index = inFront.keysGivenObjectsMadeHere ? placeAmongFirstKeysOfRun(inFront, run.keyInHolder) : 64; index < 64)
                holdsObjectMadeForIt = inFront.keysGivenObjectsMadeHere >> index & 1;
            current = inFront.isMadeHere ? 0 : m_runsOnWhatFunctionsReturn[inFront.givenBy.index()];
        }
        for (uint64_t asked : runsGoneThrough)
            runsBehindMakerThatFillsField.set(asked, holdsObjectMadeForIt);
        if (!holdsObjectMadeForIt)
            continue;
        for (LayoutID layout : run.layouts) {
            for (PropertyKeyID key : run.keys)
                note(layout, key);
        }
        if (run.layouts.isEmpty() && run.isOnObjectThatLinkHasLost)
            runsOnObjectsThatLinkHasLost.append({ run.keys.span(), 0 });
    }
    if (runsOnObjectsThatLinkHasLost.isEmpty())
        return;

    auto bitsOf = [](PropertyKeyID key) {
        return key.bits();
    };
    std::ranges::sort(runsOnObjectsThatLinkHasLost, [&](const auto& first, const auto& second) {
        if (std::ranges::equal(first.keys, second.keys))
            return first.keysOnlyGivenUndefined < second.keysOnlyGivenUndefined;
        return std::ranges::lexicographical_compare(first.keys, second.keys, { }, bitsOf, bitsOf);
    });
    HashMap<uint32_t, Vector<LayoutID, 2>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> layoutsStoredUnderKey;
    for (const NameNotListed& name : m_namesNotListed)
        layoutsStoredUnderKey.add(name.key.bits(), Vector<LayoutID, 2> { }).iterator->value.append(name.layout);
    for (size_t place = 0; place < runsOnObjectsThatLinkHasLost.size(); ++place) {
        const auto& run = runsOnObjectsThatLinkHasLost[place];
        if (place && std::ranges::equal(run.keys, runsOnObjectsThatLinkHasLost[place - 1].keys) && run.keysOnlyGivenUndefined == runsOnObjectsThatLinkHasLost[place - 1].keysOnlyGivenUndefined)
            continue;
        const Vector<LayoutID, 2>* candidates = nullptr;
        bool someLayoutMayHaveEveryName = true;
        bool someNameIsLeftToNote = false;
        for (size_t index = 0; index < run.keys.size() && someLayoutMayHaveEveryName; ++index) {
            PropertyKeyID key = run.keys[index];
            auto stored = layoutsStoredUnderKey.find(key.bits());
            if (stored == layoutsStoredUnderKey.end()) {
                someLayoutMayHaveEveryName = !m_program.layoutsWithName(key).empty();
                continue;
            }
            if (!candidates)
                candidates = &stored->value;
            if (index >= 64 || !(run.keysOnlyGivenUndefined >> index & 1))
                someNameIsLeftToNote |= numbersOfLayoutsNotedUnderKey.get(key.bits()) < stored->value.size();
        }
        if (!candidates || !someLayoutMayHaveEveryName || !someNameIsLeftToNote)
            continue;
        for (LayoutID layout : *candidates) {
            bool hasEveryName = std::ranges::all_of(run.keys, [&](PropertyKeyID key) {
                return m_pairsNotListed.contains(static_cast<uint64_t>(layout.index()) << 32 | key.bits()) || findField(m_program, layout, key);
            });
            for (size_t index = 0; hasEveryName && index < run.keys.size(); ++index) {
                if (index >= 64 || !(run.keysOnlyGivenUndefined >> index & 1))
                    note(layout, run.keys[index]);
            }
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
