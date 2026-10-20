/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRuntimeRecords.h"
#include <array>
#include <optional>
#include <type_traits>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

struct alignas(32) HashedSlot {
    static constexpr uintptr_t accessMask = 3;
    static constexpr unsigned ways = 4;
    static constexpr unsigned sizeShift = 5;
    static constexpr uint32_t structureMultiplier = 0x9e3779b1;
    static constexpr unsigned structureHashShift = 16 - sizeShift;
    static constexpr unsigned uidAlignmentShift = 4;
    static constexpr unsigned uidHashShift = sizeShift - uidAlignmentShift;

    static uintptr_t keyFor(const UniquedStringImpl* uid, SlotAccess access) { return std::bit_cast<uintptr_t>(uid) | static_cast<uintptr_t>(access); }
    static constexpr uint64_t offsetMaskFor(uint32_t numberOfEntries) { return static_cast<uint64_t>(numberOfEntries - 1) << sizeShift; }
    static uint32_t offsetOfFirstWay(StructureID structureID, const UniquedStringImpl* uid, uint64_t offsetMask)
    {
        uint32_t structureHash = (structureID.bits() * structureMultiplier) >> structureHashShift;
        uint32_t uidHash = static_cast<uint32_t>(std::bit_cast<uintptr_t>(uid)) << uidHashShift;
        return static_cast<uint32_t>((structureHash + uidHash) & offsetMask);
    }
    static constexpr uint32_t offsetOfWay(uint32_t offsetOfFirstWay, unsigned way) { return offsetOfFirstWay ^ (way << sizeShift); }

    bool isEmpty() const { return !key; }
    bool matches(StructureID structureID, uintptr_t expectedKey) const { return key == expectedKey && slot.structureID == structureID; }
    UniquedStringImpl* uid() const { return std::bit_cast<UniquedStringImpl*>(key & ~accessMask); }
    SlotAccess access() const { return static_cast<SlotAccess>(key & accessMask); }

    static constexpr ptrdiff_t offsetOfSlot() { return 0; }
    static constexpr ptrdiff_t offsetOfKey() { return 16; }

    Slot slot;
    uintptr_t key;
};

enum class SlotForm : uint8_t { Slot, PackedSlot };
enum class SlotOrigin : uint8_t { ById, ByIdInStubFrame, ByVal, FreeName, FreeNameForTypeOf };
enum class SlotOperation : uint8_t { Read, StoreStrict, StoreSloppy, Define };

constexpr SlotAccess accessOf(SlotOperation operation)
{
    switch (operation) {
    case SlotOperation::Read:
        return SlotAccess::Read;
    case SlotOperation::StoreStrict:
    case SlotOperation::StoreSloppy:
        return SlotAccess::Store;
    case SlotOperation::Define:
        return SlotAccess::Define;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr SlotOperation operationFor(PutMode mode)
{
    switch (mode) {
    case PutMode::Strict:
        return SlotOperation::StoreStrict;
    case PutMode::Sloppy:
        return SlotOperation::StoreSloppy;
    case PutMode::Define:
        return SlotOperation::Define;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

#define FOR_EACH_AOT_SLOT_HANDLER_TABLE(v) \
    v(ReadById, Read, ById, Slot) \
    v(ReadByIdPacked, Read, ById, PackedSlot) \
    v(ReadByIdInStubFrame, Read, ByIdInStubFrame, Slot) \
    v(ReadByIdPackedInStubFrame, Read, ByIdInStubFrame, PackedSlot) \
    v(ReadByVal, Read, ByVal, Slot) \
    v(ReadFreeName, Read, FreeName, Slot) \
    v(ReadFreeNameForTypeOf, Read, FreeNameForTypeOf, Slot) \
    v(StoreStrictById, StoreStrict, ById, Slot) \
    v(StoreStrictByIdPacked, StoreStrict, ById, PackedSlot) \
    v(StoreStrictByVal, StoreStrict, ByVal, Slot) \
    v(StoreStrictFreeName, StoreStrict, FreeName, Slot) \
    v(StoreSloppyById, StoreSloppy, ById, Slot) \
    v(StoreSloppyByIdPacked, StoreSloppy, ById, PackedSlot) \
    v(StoreSloppyByVal, StoreSloppy, ByVal, Slot) \
    v(StoreSloppyFreeName, StoreSloppy, FreeName, Slot) \
    v(DefineById, Define, ById, Slot) \
    v(DefineByIdPacked, Define, ById, PackedSlot) \
    v(DefineByVal, Define, ByVal, Slot)

enum class SlotHandlerTable : uint8_t {
#define AOT_DECLARE_SLOT_HANDLER_TABLE(name, operation, origin, form) name,
    FOR_EACH_AOT_SLOT_HANDLER_TABLE(AOT_DECLARE_SLOT_HANDLER_TABLE)
#undef AOT_DECLARE_SLOT_HANDLER_TABLE
};

struct SlotHandlerTableTraits {
    SlotOperation operation;
    SlotOrigin origin;
    SlotForm form;

    friend constexpr bool operator==(SlotHandlerTableTraits, SlotHandlerTableTraits) = default;
};

inline constexpr std::array allSlotHandlerTables {
#define AOT_LIST_SLOT_HANDLER_TABLE(name, operation, origin, form) SlotHandlerTable::name,
    FOR_EACH_AOT_SLOT_HANDLER_TABLE(AOT_LIST_SLOT_HANDLER_TABLE)
#undef AOT_LIST_SLOT_HANDLER_TABLE
};
inline constexpr unsigned numberOfSlotHandlerTables = allSlotHandlerTables.size();

constexpr SlotHandlerTableTraits traitsOf(SlotHandlerTable table)
{
    switch (table) {
#define AOT_SLOT_HANDLER_TABLE_TRAITS(name, operation, origin, form) \
    case SlotHandlerTable::name: \
        return { SlotOperation::operation, SlotOrigin::origin, SlotForm::form };
    FOR_EACH_AOT_SLOT_HANDLER_TABLE(AOT_SLOT_HANDLER_TABLE_TRAITS)
#undef AOT_SLOT_HANDLER_TABLE_TRAITS
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr std::optional<SlotHandlerTable> tryFindSlotHandlerTable(SlotHandlerTableTraits traits)
{
    for (SlotHandlerTable table : allSlotHandlerTables) {
        if (traitsOf(table) == traits)
            return table;
    }
    return std::nullopt;
}

constexpr SlotHandlerTable slotHandlerTable(SlotOperation operation, SlotOrigin origin, SlotForm form)
{
    std::optional<SlotHandlerTable> table = tryFindSlotHandlerTable({ operation, origin, form });
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(table);
    return *table;
}

struct StructureRowWords {
    static constexpr unsigned granuleShift = 6;
    static constexpr unsigned wordShift = 2;
};

struct FrontWords {
    const uint32_t* rowsForStructureIDZero;
    const PackedSlot* plane;
};

struct PropertyCacheWords {
    FrontWords front[numberOfSlotAccesses];
    const HashedSlot* hashedSlots;
    uint64_t hashedSlotOffsetMask;
    uintptr_t siteCacheStateBias;
    const CustomAccessorRecord* customAccessors;
    uint32_t numberStructureID;
    uint32_t booleanStructureID;
    UniquedStringImpl* hasInstanceUID;
    UniquedStringImpl* prototypeUID;
    JSCell* defaultHasInstanceFunction;
    void* handlers[numberOfSlotHandlerTables][numberOfSlotKinds];

    static constexpr ptrdiff_t offsetOfFront(SlotAccess access) { return OBJECT_OFFSETOF(PropertyCacheWords, front) + static_cast<ptrdiff_t>(static_cast<unsigned>(access) * sizeof(FrontWords)); }
    static constexpr ptrdiff_t offsetOfHandlers(SlotHandlerTable table) { return OBJECT_OFFSETOF(PropertyCacheWords, handlers) + static_cast<ptrdiff_t>(static_cast<unsigned>(table) * numberOfSlotKinds * sizeof(void*)); }
};

static_assert(sizeof(HashedSlot) == 1u << HashedSlot::sizeShift && alignof(HashedSlot) == sizeof(HashedSlot));
static_assert(OBJECT_OFFSETOF(HashedSlot, slot) == HashedSlot::offsetOfSlot() && OBJECT_OFFSETOF(HashedSlot, key) == HashedSlot::offsetOfKey());
static_assert(std::is_trivially_copyable_v<HashedSlot> && std::is_aggregate_v<HashedSlot>);
static_assert(numberOfSlotAccesses <= HashedSlot::accessMask + 1 && alignof(UniquedStringImpl) > HashedSlot::accessMask);
static_assert(hasOneBitSet(HashedSlot::ways));
static_assert(sizeof(FrontWords) == 2 * sizeof(void*) && OBJECT_OFFSETOF(FrontWords, plane) == sizeof(void*));
static_assert(1u << StructureRowWords::wordShift == sizeof(uint32_t));
static_assert(std::is_trivially_default_constructible_v<PropertyCacheWords>);
static_assert(OBJECT_OFFSETOF(PropertyCacheWords, hashedSlotOffsetMask) == OBJECT_OFFSETOF(PropertyCacheWords, hashedSlots) + sizeof(void*));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
