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
#include <wtf/UnalignedAccess.h>

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
    while (way < ways && entries[indexOf(encoded, letterCase, way)])
        ++way;
    if (way == ways)
        way = encoded * multiplier >> (shift - 2) & (ways - 1);
    entries[indexOf(encoded, letterCase, way)] = static_cast<uint64_t>(encoded) << 32 | MarkedBlockCage::encode(cageBase, converted);
    size_t set = indexOf(encoded, letterCase) >> setShift;
    setsWithEntries[set / 64] |= uint64_t { 1 } << set % 64;
}

template<typename Functor>
static ALWAYS_INLINE void forEachWord(const Latin1Character* characters, size_t length, const Functor& functor)
{
    if (length >= sizeof(uint64_t)) {
        for (size_t offset = 0; offset < length - sizeof(uint64_t); offset += sizeof(uint64_t))
            functor(WTF::unalignedLoad<uint64_t>(characters + offset), offset);
        functor(WTF::unalignedLoad<uint64_t>(characters + length - sizeof(uint64_t)), length - sizeof(uint64_t));
        return;
    }
    if (length >= sizeof(uint32_t)) {
        functor(WTF::unalignedLoad<uint32_t>(characters), 0);
        functor(WTF::unalignedLoad<uint32_t>(characters + length - sizeof(uint32_t)), length - sizeof(uint32_t));
        return;
    }
    functor(WTF::unalignedLoad<uint16_t>(characters), 0);
    functor(WTF::unalignedLoad<uint16_t>(characters + length - sizeof(uint16_t)), length - sizeof(uint16_t));
}

static ALWAYS_INLINE uint32_t hashShortString(std::span<const Latin1Character> characters)
{
    constexpr uint64_t multiplier = 0x9e3779b97f4a7c15ull;
    uint64_t hash = characters.size();
    forEachWord(characters.data(), characters.size(), [&](auto word, size_t) ALWAYS_INLINE_LAMBDA {
        hash = (hash ^ word) * multiplier;
        hash ^= hash >> 32;
    });
    return static_cast<uint32_t>(hash);
}

static ALWAYS_INLINE bool equalShortString(const StringImpl* string, std::span<const Latin1Character> characters)
{
    if (!string || !string->is8Bit() || string->length() != characters.size())
        return false;
    const Latin1Character* other = string->span8().data();
    uint64_t difference = 0;
    forEachWord(characters.data(), characters.size(), [&](auto word, size_t offset) ALWAYS_INLINE_LAMBDA {
        difference |= word ^ WTF::unalignedLoad<decltype(word)>(other + offset);
    });
    return !difference;
}

JSString* CaseConversions::convertedString(VM& vm, std::span<const Latin1Character> characters)
{
    ASSERT(characters.size() >= 2);
    uint32_t hash = hashShortString(characters);
    uint64_t& entry = convertedStrings[hash % numberOfConvertedStrings];
    if (entry && entry >> 32 == hash) {
        JSString* candidate = uncheckedDowncast<JSString>(MarkedBlockCage::decode(cageBase, static_cast<uint32_t>(entry)));
        if (equalShortString(candidate->tryGetValueImpl(), characters))
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
