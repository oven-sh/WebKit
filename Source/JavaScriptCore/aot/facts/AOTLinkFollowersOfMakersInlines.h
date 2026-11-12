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
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> initialNamesNoted;
    auto bitsOfInitialName = [](PropertyKeyID key) {
        return key.bits();
    };
    auto hashOfInitialNames = [](std::span<const PropertyKeyID> keys, uint64_t seed) {
        constexpr uint64_t multiplier = 0x9e3779b97f4a7c15u;
        uint64_t hash = seed;
        for (PropertyKeyID key : keys)
            hash = (hash ^ hash >> 32) * multiplier + key.bits();
        return hash * multiplier >> 1;
    };
    auto noteInitialNames = [&](LayoutID layout, std::span<const PropertyKeyID> keys, uint64_t storedTogether) {
        LinkedInitialNames names { layout, { } };
        for (size_t index = 0; index < std::min<size_t>(keys.size(), 64); ++index) {
            if (storedTogether >> index & 1 && m_pairsNotListed.contains(static_cast<uint64_t>(layout.index()) << 32 | keys[index].bits()))
                names.keys.append(keys[index]);
        }
        if (names.keys.isEmpty())
            return;
        std::ranges::sort(names.keys, { }, bitsOfInitialName);
        uint64_t hash = hashOfInitialNames(names.keys, layout.index());
        if (initialNamesNoted.add(hash).isNewEntry)
            m_tables.initialNames.append(WTF::move(names));
    };
    struct LostInitialNames {
        std::span<const PropertyKeyID> keys;
        uint64_t storedTogether { 0 };
        Vector<PropertyKeyID, 8> keysOfMakers;
    };
    Vector<LostInitialNames> lostInitialNames;
    auto noteLostInitialNames = [&](LostInitialNames&& names) {
        if (!names.storedTogether || names.keysOfMakers.isEmpty())
            return;
        std::ranges::sort(names.keysOfMakers, { }, bitsOfInitialName);
        names.keysOfMakers.shrink(names.keysOfMakers.size() - std::ranges::unique(names.keysOfMakers).size());
        uint64_t hash = hashOfInitialNames(names.keysOfMakers, hashOfInitialNames(names.keys, names.storedTogether));
        if (initialNamesNoted.add(hash).isNewEntry)
            lostInitialNames.append(WTF::move(names));
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
        uint64_t keysStoredTogether = run.keysStoredTogether & ~run.keysOnlyGivenUndefined;
        for (LayoutID layout : layouts) {
            for (size_t index = 0; index < run.keys.size(); ++index) {
                if (index >= 64 || !(run.keysOnlyGivenUndefined >> index & 1))
                    note(layout, run.keys[index]);
            }
            noteInitialNames(layout, run.keys.span(), keysStoredTogether);
        }
        if (layouts.isEmpty() && run.isOnObjectThatLinkHasLost) {
            runsOnObjectsThatLinkHasLost.append({ run.keys.span(), run.keysOnlyGivenUndefined });
            LostInitialNames names { run.keys.span(), keysStoredTogether, { } };
            size_t numberOfMakersLeft = m_runsOfMakers.size();
            for (const RunOfMaker* maker = &run; !maker->isMadeHere && maker->givenBy && !m_runsOnWhatFunctionsReturn.isEmpty() && m_runsOnWhatFunctionsReturn[maker->givenBy.index()] && numberOfMakersLeft--;) {
                maker = &m_runsOfMakers[m_runsOnWhatFunctionsReturn[maker->givenBy.index()] - 1];
                names.keysOfMakers.appendVector(maker->keys);
            }
            noteLostInitialNames(WTF::move(names));
        }
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
            noteInitialNames(layout, run.keys.span(), run.keysStoredTogether);
        }
        if (run.layouts.isEmpty() && run.isOnObjectThatLinkHasLost) {
            runsOnObjectsThatLinkHasLost.append({ run.keys.span(), 0 });
            LostInitialNames names { run.keys.span(), run.keysStoredTogether, { } };
            FunctionID maker = run.givenBy;
            for (size_t numberOfMakersLeft = m_runsOnObjectsInFields.size(); maker && numberOfMakersLeft--;) {
                auto found = m_runsOnFieldsOfWhatFunctionsReturn.find(static_cast<uint64_t>(maker.index() + 1) << 32 | run.keyInHolder.bits());
                if (found == m_runsOnFieldsOfWhatFunctionsReturn.end())
                    break;
                const RunOnObjectInField& inFront = m_runsOnObjectsInFields[found->value - 1];
                names.keysOfMakers.appendVector(inFront.keys);
                maker = inFront.givenBy;
            }
            noteLostInitialNames(WTF::move(names));
        }
    }
    if (runsOnObjectsThatLinkHasLost.isEmpty()) {
        m_tables.sortInitialNames();
        return;
    }

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
    for (const LostInitialNames& names : lostInitialNames) {
        const Vector<LayoutID, 2>* candidates = nullptr;
        for (uint64_t left = names.storedTogether; left; left &= left - 1) {
            auto stored = layoutsStoredUnderKey.find(names.keys[std::countr_zero(left)].bits());
            if (stored != layoutsStoredUnderKey.end() && (!candidates || stored->value.size() < candidates->size()))
                candidates = &stored->value;
        }
        if (!candidates)
            continue;
        for (LayoutID layout : *candidates) {
            auto hasName = [&](PropertyKeyID key) {
                return m_pairsNotListed.contains(static_cast<uint64_t>(layout.index()) << 32 | key.bits()) || findField(m_program, layout, key);
            };
            if (std::ranges::all_of(names.keys, hasName) && std::ranges::all_of(names.keysOfMakers, hasName))
                noteInitialNames(layout, names.keys, names.storedTogether);
        }
    }
    m_tables.sortInitialNames();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
