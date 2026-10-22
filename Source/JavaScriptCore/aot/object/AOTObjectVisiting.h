/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "AOTSlotAccess.h"
#include "HeapCellInlines.h"
#include "JSCell.h"
#include "MarkedBlock.h"
#include <bit>
#include <span>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct Reference {
    enum class Kind : uint8_t { Value, Cell32, Auxiliary };
    Kind kind;
    uint64_t bits;
};
static_assert(sizeof(Reference) == 16);

static constexpr size_t numberOfInlineReferenceCounts = 3;
static constexpr size_t numberOfExtensionGranuleSets = 3;
static constexpr size_t numberOfExtensionHeaderWords = 1 + numberOfExtensionGranuleSets * (sizeof(uint64_t) / sizeof(uint32_t));
static_assert(maxExtensionGranules <= 64 && extensionGranuleSize == 2 * sizeof(uint32_t));

struct ExtensionReferences {
    uint64_t wide;
    uint64_t narrowInFirstHalf;
    uint64_t narrowInSecondHalf;
};

ALWAYS_INLINE ExtensionReferences extensionReferencesAt(const uint32_t* header)
{
    auto setAt = [&](size_t index) ALWAYS_INLINE_LAMBDA {
        return header[1 + 2 * index] | static_cast<uint64_t>(header[2 + 2 * index]) << 32;
    };
    return { setAt(0), setAt(1), setAt(2) };
}

template<typename Record>
inline std::span<const uint32_t> inlineReferenceTableOf(const Record& record)
{
    if (!record.referencesOffset)
        return { };
    const uint32_t* words = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(&record) + record.referencesOffset);
    return { words, numberOfInlineReferenceCounts + words[0] + words[1] + words[2] };
}

inline std::span<const uint32_t> referenceTableOf(const LayoutData& layout)
{
    ASSERT(!layout.hasExtension() || layout.referencesOffset);
    std::span inlineTable = inlineReferenceTableOf(layout);
    if (!layout.hasExtension() || inlineTable.empty())
        return inlineTable;
    return { inlineTable.data(), inlineTable.size() + numberOfExtensionHeaderWords };
}

inline std::span<const uint32_t> referenceTableOf(const EnvironmentData& environment)
{
    return inlineReferenceTableOf(environment);
}

ALWAYS_INLINE const uint8_t* extensionStorageOrNull(const JSCell* cell, SlotData extensionSlot)
{
    const uint8_t* slot = reinterpret_cast<const uint8_t*>(cell) + extensionSlot.offset;
    if (extensionSlot.representation() == SlotRepresentation::Cell)
        return *reinterpret_cast<const uint8_t* const*>(slot);
    ASSERT(extensionSlot.representation() == SlotRepresentation::Cell32);
    uint32_t word = *reinterpret_cast<const uint32_t*>(slot);
    return word ? reinterpret_cast<const uint8_t*>(decodeCell32(cell, word)) + offsetOfEmptyExtensionInCage : nullptr;
}

template<typename Functor>
ALWAYS_INLINE void forEachReference(const JSCell* cell, const uint32_t* counts, bool hasExtension, const Functor& functor)
{
    auto visitValue = [&](const uint8_t* slot) ALWAYS_INLINE_LAMBDA {
        functor(Reference { Reference::Kind::Value, *reinterpret_cast<const uint64_t*>(slot) });
    };
    auto visitCell32 = [&](const uint8_t* slot) ALWAYS_INLINE_LAMBDA {
        if (uint32_t word = *reinterpret_cast<const uint32_t*>(slot); word >= minCell32)
            functor(Reference { Reference::Kind::Cell32, word });
    };
    const uint8_t* fields = reinterpret_cast<const uint8_t*>(cell);
    const uint32_t* next = counts + numberOfInlineReferenceCounts;
    for (const uint32_t* end = next + counts[0] + counts[1]; next != end; ++next)
        visitValue(fields + *next);
    for (const uint32_t* end = next + counts[2]; next != end; ++next)
        visitCell32(fields + *next);
    if (!hasExtension)
        return;
    const uint32_t* header = next;
    const uint8_t* extension = extensionStorageOrNull(cell, SlotData::decode(header[0]));
    if (!extension)
        return;
    functor(Reference { Reference::Kind::Auxiliary, std::bit_cast<uintptr_t>(extension) });
    // Read once: the mutator may set bits of granules that come after these, and those hold nothing that this visit could miss.
    uint64_t granuleBits = *reinterpret_cast<const uint64_t*>(extension + offsetOfExtensionGranuleBits);
    ExtensionReferences sets = extensionReferencesAt(header);
    for (uint64_t remaining = granuleBits & (sets.wide | sets.narrowInFirstHalf | sets.narrowInSecondHalf); remaining; remaining &= remaining - 1) {
        uint64_t bit = remaining & -remaining;
        const uint8_t* slot = extension + offsetOfFirstExtensionSlot + std::popcount(granuleBits & (bit - 1)) * extensionGranuleSize;
        if (sets.wide & bit) {
            visitValue(slot);
            continue;
        }
        if (sets.narrowInFirstHalf & bit)
            visitCell32(slot);
        if (sets.narrowInSecondHalf & bit)
            visitCell32(slot + sizeof(uint32_t));
    }
}

template<typename Functor>
ALWAYS_INLINE void forEachReference(const JSCell* cell, std::span<const uint32_t> references, const Functor& functor)
{
    if (references.empty())
        return;
    const uint32_t* counts = references.data();
    forEachReference(cell, counts, references.size() != numberOfInlineReferenceCounts + counts[0] + counts[1] + counts[2], functor);
}

template<typename Record, typename Visitor>
ALWAYS_INLINE void visitReferences(const JSCell* cell, const Record& record, bool hasExtension, Visitor& visitor)
{
    if (!record.referencesOffset)
        return;
    const uint32_t* counts = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(&record) + record.referencesOffset);
    forEachReference(cell, counts, hasExtension, [&](Reference reference) ALWAYS_INLINE_LAMBDA {
        switch (reference.kind) {
        case Reference::Kind::Value:
            visitor.appendUnbarriered(JSValue::decode(static_cast<EncodedJSValue>(reference.bits)));
            return;
        case Reference::Kind::Cell32:
            visitNarrowReference(visitor, cell, static_cast<uint32_t>(reference.bits));
            return;
        case Reference::Kind::Auxiliary:
            visitor.markAuxiliary(std::bit_cast<const void*>(static_cast<uintptr_t>(reference.bits)));
            return;
        }
    });
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
