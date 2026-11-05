/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCaseConversions.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include <wtf/text/StringHasherInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

CaseConversions& CaseConversions::ensure(State& state)
{
    if (CaseConversions* conversions = state.instance.realm.caseConversions) [[likely]]
        return *conversions;
    CaseConversions* conversions = state.tableArena.allocate<CaseConversions>(1);
    conversions->cageBase = state.instance.vm->heap.cage().base();
    state.caches->add(CacheKind::WeakByKey, "CaseConversions"_s, *conversions);
    state.instance.realm.caseConversions = conversions;
    return *conversions;
}

void CaseConversions::remember(const JSString* source, LetterCase letterCase, const JSString* converted)
{
    if (std::bit_cast<uintptr_t>(source) - cageBase >= MarkedBlockCage::maxSize || std::bit_cast<uintptr_t>(converted) - cageBase >= MarkedBlockCage::maxSize)
        return;
    uint32_t encoded = MarkedBlockCage::encode(cageBase, source);
    size_t way = 0;
    while (way < ways - 1 && entries[indexOf(encoded, letterCase, way)])
        ++way;
    for (; way; --way)
        entries[indexOf(encoded, letterCase, way)] = entries[indexOf(encoded, letterCase, way - 1)];
    entries[indexOf(encoded, letterCase)] = static_cast<uint64_t>(encoded) << 32 | MarkedBlockCage::encode(cageBase, converted);
    size_t set = indexOf(encoded, letterCase) >> setShift;
    setsWithEntries[set / 64] |= uint64_t { 1 } << set % 64;
}

JSString* CaseConversions::convertedString(VM& vm, std::span<const Latin1Character> characters)
{
    ASSERT(characters.size() >= 2);
    unsigned hash = StringHasher::computeHashAndMaskTop8Bits<Latin1Character>(characters);
    uint64_t& entry = convertedStrings[hash % numberOfConvertedStrings];
    if (entry && entry >> 32 == hash) {
        JSString* candidate = uncheckedDowncast<JSString>(MarkedBlockCage::decode(cageBase, static_cast<uint32_t>(entry)));
        if (equal(candidate->tryGetValueImpl(), characters))
            return candidate;
    }
    JSString* made = jsNontrivialString(vm, String(characters));
    if (std::bit_cast<uintptr_t>(made) - cageBase < MarkedBlockCage::maxSize)
        entry = static_cast<uint64_t>(hash) << 32 | MarkedBlockCage::encode(cageBase, made);
    return made;
}

size_t CaseConversions::removeDeadEntries(VM& vm, CollectionScope)
{
    size_t removedCount = 0;
    for (uint64_t& entry : convertedStrings) {
        if (entry && !vm.heap.isMarked(MarkedBlockCage::decode(cageBase, static_cast<uint32_t>(entry)))) {
            entry = 0;
            ++removedCount;
        }
    }
    for (size_t set = 0; set < numberOfSets; ++set) {
        uint64_t bit = uint64_t { 1 } << set % 64;
        if (!(setsWithEntries[set / 64] & bit))
            continue;
        bool hasEntries = false;
        for (size_t index = set << setShift; index < (set + 1) << setShift; ++index) {
            uint64_t entry = entries[index];
            if (!entry)
                continue;
            if (vm.heap.isMarked(MarkedBlockCage::decode(cageBase, static_cast<uint32_t>(entry >> 32))) && vm.heap.isMarked(MarkedBlockCage::decode(cageBase, static_cast<uint32_t>(entry)))) {
                hasEntries = true;
                continue;
            }
            entries[index] = 0;
            ++removedCount;
        }
        if (!hasEntries)
            setsWithEntries[set / 64] &= ~bit;
    }
    return removedCount;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
