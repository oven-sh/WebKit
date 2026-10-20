/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "AOTSlotAccess.h"
#include "JSCell.h"
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
static constexpr size_t numberOfExtensionReferenceCounts = 2;
static constexpr size_t numberOfExtensionHeaderWords = 1 + numberOfExtensionReferenceCounts;

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
    const uint32_t* header = inlineTable.data() + inlineTable.size();
    return { inlineTable.data(), inlineTable.size() + numberOfExtensionHeaderWords + header[1] + header[2] };
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
ALWAYS_INLINE void forEachReference(const JSCell* cell, std::span<const uint32_t> references, const Functor& functor)
{
    if (references.empty())
        return;
    auto visitValue = [&](const uint8_t* slot) ALWAYS_INLINE_LAMBDA {
        functor(Reference { Reference::Kind::Value, *reinterpret_cast<const uint64_t*>(slot) });
    };
    auto visitCell32 = [&](const uint8_t* slot) ALWAYS_INLINE_LAMBDA {
        if (uint32_t word = *reinterpret_cast<const uint32_t*>(slot); word >= minCell32)
            functor(Reference { Reference::Kind::Cell32, word });
    };
    const uint8_t* fields = reinterpret_cast<const uint8_t*>(cell);
    const uint32_t* counts = references.data();
    std::span wide { counts + numberOfInlineReferenceCounts, static_cast<size_t>(counts[0]) + counts[1] };
    for (uint32_t offset : wide)
        visitValue(fields + offset);
    std::span narrow { wide.data() + wide.size(), counts[2] };
    for (uint32_t offset : narrow)
        visitCell32(fields + offset);
    const uint32_t* header = narrow.data() + narrow.size();
    if (header == references.data() + references.size())
        return;
    const uint8_t* extension = extensionStorageOrNull(cell, SlotData::decode(header[0]));
    if (!extension)
        return;
    functor(Reference { Reference::Kind::Auxiliary, std::bit_cast<uintptr_t>(extension) });
    // Read once: the mutator may set bits of granules that come after these, and those hold nothing that this visit could miss.
    uint64_t granuleBits = *reinterpret_cast<const uint64_t*>(extension + offsetOfExtensionGranuleBits);
    auto slotAt = [&](uint32_t offset) ALWAYS_INLINE_LAMBDA -> const uint8_t* {
        uint64_t bit = granuleBitOf(offset);
        if (!(granuleBits & bit))
            return nullptr;
        return extension + offsetOfFirstExtensionSlot + std::popcount(granuleBits & (bit - 1)) * extensionGranuleSize + offsetInGranule(offset);
    };
    std::span wideOutOfLine { header + numberOfExtensionHeaderWords, header[1] };
    for (uint32_t offset : wideOutOfLine) {
        if (const uint8_t* slot = slotAt(offset))
            visitValue(slot);
    }
    for (std::span narrowOutOfLine { wideOutOfLine.data() + wideOutOfLine.size(), header[2] }; uint32_t offset : narrowOutOfLine) {
        if (const uint8_t* slot = slotAt(offset))
            visitCell32(slot);
    }
}

template<typename Visitor>
ALWAYS_INLINE void visitReferences(const JSCell* cell, std::span<const uint32_t> references, Visitor& visitor)
{
    forEachReference(cell, references, [&](Reference reference) ALWAYS_INLINE_LAMBDA {
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
