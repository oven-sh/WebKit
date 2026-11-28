/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <JavaScriptCore/AOTRecordTypes.h>
#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/MarkedBlockCage.h>
#include <optional>
#include <span>
#include <wtf/Assertions.h>

namespace JSC { namespace AOT {

struct SlotData {
    static constexpr unsigned offsetBits = 24;
    static constexpr uint32_t maxOffset = (1u << offsetBits) - 1;

    uint32_t offset : offsetBits;
    uint32_t representationBits : 32 - offsetBits;

    static constexpr SlotData create(uint32_t offset, SlotRepresentation representation)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(offset <= maxOffset);
        return { offset, static_cast<uint32_t>(representation) };
    }
    constexpr SlotRepresentation representation() const { return static_cast<SlotRepresentation>(representationBits); }
    constexpr uint32_t encoded() const { return offset | representationBits << offsetBits; }
    static constexpr SlotData decode(uint32_t word) { return { word & maxOffset, word >> offsetBits }; }
    friend constexpr bool operator==(SlotData a, SlotData b) { return a.encoded() == b.encoded(); }
};
static_assert(sizeof(SlotData) == 4 && isImageRecord<SlotData>);

constexpr bool isObjectLayout(LayoutKind kind) { return isObjectLayoutOf(kind); }

static constexpr int64_t maxSlotInteger = (1ll << 62) - 1;
static constexpr int64_t undefinedInteger = 0x4000ll << 48;
static constexpr int64_t nullInteger = 0x4001ll << 48;
static constexpr int64_t absentInteger = 0x4002ll << 48;
static constexpr uint64_t undefinedDouble = 0x7ffaull << 48;
static constexpr uint64_t nullDouble = 0x7ffbull << 48;
static constexpr uint64_t absentDouble = 0x7ffcull << 48;
static_assert(maxSlotInteger < undefinedInteger);

constexpr uint64_t quietNaNWithSignOf(uint64_t bits) { return (bits >> 51 | 1) << 51; }
ALWAYS_INLINE double withoutPayload(double number)
{
    if (number == number) [[likely]]
        return number;
    return std::bit_cast<double>(quietNaNWithSignOf(std::bit_cast<uint64_t>(number)));
}
static_assert(quietNaNWithSignOf(absentDouble) == PNaNAsBits && quietNaNWithSignOf(UINT64_MAX) == (PNaNAsBits | 1ull << 63));

static constexpr int32_t absentInt32 = INT32_MIN + 1;
static constexpr int32_t undefinedInt32 = INT32_MIN + 2;
static constexpr int32_t nullInt32 = INT32_MIN + 3;
static constexpr int32_t minInlineInt32 = INT32_MIN + 5;

static constexpr int64_t maxIntegerAddedTo = (1ll << 52) - 1;
static constexpr int64_t minIntegerAddedTo = -maxIntegerAddedTo - 1;

static constexpr unsigned cell32Shift = MarkedBlockCage::shift;
static constexpr uint32_t minCell32 = MarkedBlockCage::minIndex;
static constexpr uint32_t absentCell32 = 0;
static constexpr uint32_t nullCell32 = 2;
static constexpr uint32_t undefinedCell32 = 10;
static constexpr int64_t placeOfAbsentKeyInMap = -1;
static constexpr int64_t placeInMapThatIsNotTold = -2;
static constexpr int64_t placeInMapHoldsNarrowValue = 1;
static_assert(nullCell32 == JSValue::ValueNull && undefinedCell32 == JSValue::ValueUndefined && absentCell32 == JSValue::ValueEmpty);

static constexpr uint32_t uncreatedDeferredClosure = 0;
static constexpr uint32_t absentDeferredClosure = 1;
static_assert(absentDeferredClosure != uncreatedDeferredClosure && absentDeferredClosure < minCell32);
static_assert(absentDeferredClosure != nullCell32 && absentDeferredClosure != undefinedCell32);

struct NarrowCharactersOfString {
    static constexpr uint32_t lengthOfNoNarrowText = UINT32_MAX;

    uint32_t length;
    alignas(8) std::array<uint8_t, 128> characters;
};

enum class SlotValueKind : uint8_t {
    SignedInteger,
    UnsignedInteger,
    Float,
    Double,
    Boolean,
    Boxed,
    Cell,
    DeferredClosure,
};

struct SlotMarkers {
    uint64_t absent;
    uint64_t undefined;
    uint64_t null;
};

static constexpr SlotMarkers boxedMarkers { JSValue::ValueEmpty, JSValue::ValueUndefined, JSValue::ValueNull };
static constexpr SlotMarkers integerMarkers { static_cast<uint64_t>(absentInteger), static_cast<uint64_t>(undefinedInteger), static_cast<uint64_t>(nullInteger) };
static constexpr SlotMarkers doubleMarkers { absentDouble, undefinedDouble, nullDouble };
static constexpr SlotMarkers int32Markers { static_cast<uint32_t>(absentInt32), static_cast<uint32_t>(undefinedInt32), static_cast<uint32_t>(nullInt32) };
static constexpr SlotMarkers cell32Markers { absentCell32, undefinedCell32, nullCell32 };

struct SlotTraits {
    static constexpr SlotTraits of(uint8_t width, SlotValueKind valueKind) { return { .width = width, .valueKind = valueKind }; }
    constexpr SlotTraits holdingCell() const
    {
        SlotTraits result = *this;
        result.holdsCell = true;
        return result;
    }
    constexpr SlotTraits holdingNarrowReference() const
    {
        SlotTraits result = holdingCell();
        result.isNarrowReference = true;
        return result;
    }
    constexpr SlotTraits withAbsent(uint64_t bits) const
    {
        SlotTraits result = *this;
        result.hasAbsent = true;
        result.absentBits = bits;
        return result;
    }
    constexpr SlotTraits withMarkers(SlotMarkers markers) const
    {
        SlotTraits result = withAbsent(markers.absent);
        result.hasUndefined = true;
        result.hasNull = true;
        result.undefinedBits = markers.undefined;
        result.nullBits = markers.null;
        return result;
    }

    uint8_t width { 0 };
    SlotValueKind valueKind { SlotValueKind::Boxed };
    bool holdsCell { false };
    bool isNarrowReference { false };
    bool hasAbsent { false };
    bool hasUndefined { false };
    bool hasNull { false };
    uint64_t absentBits { 0 };
    uint64_t undefinedBits { 0 };
    uint64_t nullBits { 0 };
};

constexpr SlotTraits traitsOf(SlotRepresentation representation)
{
    switch (representation) {
    case SlotRepresentation::Uint8:
        return SlotTraits::of(1, SlotValueKind::UnsignedInteger);
    case SlotRepresentation::Int8:
        return SlotTraits::of(1, SlotValueKind::SignedInteger);
    case SlotRepresentation::Boolean:
        return SlotTraits::of(1, SlotValueKind::Boolean);
    case SlotRepresentation::OptionalBoolean:
        return SlotTraits::of(1, SlotValueKind::Boolean).withMarkers(boxedMarkers);
    case SlotRepresentation::Uint16:
        return SlotTraits::of(2, SlotValueKind::UnsignedInteger);
    case SlotRepresentation::Int16:
        return SlotTraits::of(2, SlotValueKind::SignedInteger);
    case SlotRepresentation::Int32:
        return SlotTraits::of(4, SlotValueKind::SignedInteger);
    case SlotRepresentation::Uint32:
        return SlotTraits::of(4, SlotValueKind::UnsignedInteger);
    case SlotRepresentation::Float:
        return SlotTraits::of(4, SlotValueKind::Float);
    case SlotRepresentation::OptionalInt32:
        return SlotTraits::of(4, SlotValueKind::SignedInteger).withMarkers(int32Markers);
    case SlotRepresentation::Cell32:
        return SlotTraits::of(4, SlotValueKind::Cell).holdingNarrowReference().withAbsent(absentCell32);
    case SlotRepresentation::OptionalCell32:
        return SlotTraits::of(4, SlotValueKind::Cell).holdingNarrowReference().withMarkers(cell32Markers);
    case SlotRepresentation::DeferredClosure:
        return SlotTraits::of(4, SlotValueKind::DeferredClosure).holdingNarrowReference().withAbsent(absentDeferredClosure);
    case SlotRepresentation::Int64:
        return SlotTraits::of(8, SlotValueKind::SignedInteger);
    case SlotRepresentation::OptionalInt64:
        return SlotTraits::of(8, SlotValueKind::SignedInteger).withMarkers(integerMarkers);
    case SlotRepresentation::Double:
        return SlotTraits::of(8, SlotValueKind::Double);
    case SlotRepresentation::OptionalDouble:
        return SlotTraits::of(8, SlotValueKind::Double).withMarkers(doubleMarkers);
    case SlotRepresentation::Cell:
        return SlotTraits::of(8, SlotValueKind::Cell).holdingCell().withAbsent(JSValue::ValueEmpty);
    case SlotRepresentation::JSValue:
        return SlotTraits::of(8, SlotValueKind::Boxed).holdingCell().withMarkers(boxedMarkers);
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr unsigned widthOf(SlotRepresentation representation) { return traitsOf(representation).width; }
constexpr bool representsAbsentAsZero(SlotRepresentation representation) { return traitsOf(representation).hasAbsent && !traitsOf(representation).absentBits; }
constexpr bool canLeaveSlot(SlotRepresentation representation) { return traitsOf(representation).hasAbsent; }

constexpr bool isListedOnlyByKeyOrder(SlotRepresentation representation) { return representation == SlotRepresentation::Int32; }
constexpr bool mayBeCreatedByStore(SlotRepresentation representation) { return canLeaveSlot(representation) || isListedOnlyByKeyOrder(representation); }
constexpr uint64_t bitsBeforeCreation(SlotRepresentation representation)
{
    if (isListedOnlyByKeyOrder(representation))
        return static_cast<uint32_t>(absentInt32);
    SlotTraits traits = traitsOf(representation);
    return traits.hasAbsent ? traits.absentBits : 0;
}

constexpr SlotRepresentation hiddenReferenceRepresentation(bool usesWideReferences) { return usesWideReferences ? SlotRepresentation::Cell : SlotRepresentation::Cell32; }

static_assert(widthOf(SlotRepresentation::Cell32) == sizeof(uint32_t) && widthOf(SlotRepresentation::Cell) == sizeof(uint64_t));
static_assert(representsAbsentAsZero(SlotRepresentation::Cell32) && representsAbsentAsZero(SlotRepresentation::Cell));
static_assert(!canLeaveSlot(SlotRepresentation::Int32) && canLeaveSlot(SlotRepresentation::OptionalInt32));
static_assert(canLeaveSlot(SlotRepresentation::DeferredClosure) && !representsAbsentAsZero(SlotRepresentation::DeferredClosure));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
