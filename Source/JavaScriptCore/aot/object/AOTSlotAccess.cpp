/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotAccess.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTPrimitives.h"
#include "AOTProgramStrings.h"
#include "JSCJSValueInlines.h"
#include "JSString.h"
#include "PureNaN.h"
#include <algorithm>
#include <bit>

namespace JSC { namespace AOT {

static_assert(std::max({ JSValue::ValueEmpty, JSValue::ValueNull, JSValue::ValueFalse, JSValue::ValueTrue, JSValue::ValueUndefined }) <= UINT8_MAX);

static constexpr auto allRepresentationsSatisfy = [](auto predicate) consteval {
    return std::ranges::all_of(allMembersOf<SlotRepresentation>, [&](SlotRepresentation representation) {
        return predicate(traitsOf(representation));
    });
};

static_assert(allRepresentationsSatisfy([](SlotTraits traits) {
    return traits.width == 1 || traits.width == 2 || traits.width == 4 || traits.width == 8;
}));
static_assert(allRepresentationsSatisfy([](SlotTraits traits) {
    return !traits.holdsCell || traits.width == 4 || traits.width == 8;
}));
static_assert(allRepresentationsSatisfy([](SlotTraits traits) {
    return traits.isNarrowReference == (traits.holdsCell && traits.width == 4);
}));
static_assert(allRepresentationsSatisfy([](SlotTraits traits) {
    return !((traits.absentBits | traits.undefinedBits | traits.nullBits) & ~(UINT64_MAX >> (64 - 8 * traits.width)));
}));
static_assert(allRepresentationsSatisfy([](SlotTraits traits) {
    return (traits.hasAbsent || !traits.absentBits) && (traits.hasUndefined || !traits.undefinedBits) && (traits.hasNull || !traits.nullBits);
}));
static_assert(allRepresentationsSatisfy([](SlotTraits traits) {
    return !(traits.hasAbsent && traits.hasUndefined && traits.absentBits == traits.undefinedBits)
        && !(traits.hasAbsent && traits.hasNull && traits.absentBits == traits.nullBits)
        && !(traits.hasUndefined && traits.hasNull && traits.undefinedBits == traits.nullBits);
}));
static_assert(allRepresentationsSatisfy([](SlotTraits traits) {
    return !traits.isNarrowReference || std::max({ traits.absentBits, traits.undefinedBits, traits.nullBits }) < minCell32;
}));
static_assert(allRepresentationsSatisfy([](SlotTraits traits) {
    return traits.valueKind != SlotValueKind::DeferredClosure || (traits.isNarrowReference && traits.hasAbsent && traits.absentBits == absentDeferredClosure && !traits.hasUndefined && !traits.hasNull);
}));

static_assert(traitsOf(SlotRepresentation::OptionalCell32).undefinedBits == JSValue::ValueUndefined);
static_assert(traitsOf(SlotRepresentation::OptionalInt32).absentBits == 0x80000001);
static_assert(std::max({ absentInt32, undefinedInt32, nullInt32 }) < minInlineInt32);
static_assert(traitsOf(SlotRepresentation::DeferredClosure).valueKind == SlotValueKind::DeferredClosure);
static_assert(!traitsOf(SlotRepresentation::Cell32).hasUndefined);
static_assert(traitsOf(SlotRepresentation::Cell32).valueKind != SlotValueKind::DeferredClosure);
static_assert(!traitsOf(SlotRepresentation::Double).hasAbsent);

void writeSlotBits(SlotRepresentation representation, void* slot, uint64_t bits)
{
    switch (widthOf(representation)) {
    case 1:
        *static_cast<uint8_t*>(slot) = static_cast<uint8_t>(bits);
        return;
    case 2:
        *static_cast<uint16_t*>(slot) = static_cast<uint16_t>(bits);
        return;
    case 4:
        *static_cast<uint32_t*>(slot) = static_cast<uint32_t>(bits);
        return;
    case 8:
        *static_cast<uint64_t*>(slot) = bits;
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void clearSlot(SlotRepresentation representation, void* slot)
{
    SlotTraits traits = traitsOf(representation);
    writeSlotBits(representation, slot, traits.hasAbsent ? traits.absentBits : 0);
}

JSValue readSlot(SlotRepresentation representation, const void* slot, const ProgramStrings& strings)
{
    const SlotTraits& traits = traitsOf(representation);
    uint64_t bits = readSlotBits(representation, slot);
    if (traits.hasAbsent && bits == traits.absentBits)
        return { };
    if (traits.hasUndefined && bits == traits.undefinedBits)
        return jsUndefined();
    if (traits.hasNull && bits == traits.nullBits)
        return jsNull();
    auto reference = [&] {
        return traits.isNarrowReference ? JSValue(MarkedBlockCage::decode(strings.instance().cellCageBase, static_cast<uint32_t>(bits))) : JSValue::decode(bits);
    };
    switch (traits.valueKind) {
    case SlotValueKind::SignedInteger: {
        unsigned unusedBits = 64 - 8 * traits.width;
        int64_t integer = static_cast<int64_t>(bits << unusedBits) >> unusedBits;
        RELEASE_ASSERT(traits.width == sizeof(int64_t) || integer >= traits.minInteger);
        return jsNumber(static_cast<double>(integer));
    }
    case SlotValueKind::UnsignedInteger:
        return jsNumber(static_cast<double>(bits));
    case SlotValueKind::Float:
        return jsNumber(purifyNaN(std::bit_cast<float>(static_cast<uint32_t>(bits))));
    case SlotValueKind::Double:
        return jsNumber(purifyNaN(std::bit_cast<double>(bits)));
    case SlotValueKind::Boolean:
        return traits.hasAbsent ? JSValue::decode(bits) : jsBoolean(bits);
    case SlotValueKind::Boxed:
        return JSValue::decode(bits);
    case SlotValueKind::LiteralString:
        if (isLiteralStringWord(bits))
            return strings.stringForLiteralStringWord(bits);
        return reference();
    case SlotValueKind::DeferredClosure:
        // Only the object can make the closure that its slot still defers. Undefined and null are the words that any narrow reference has for them.
        if (bits == undefinedCell32 || bits == nullCell32)
            return JSValue::decode(bits);
        RELEASE_ASSERT(bits >= minCell32);
        return reference();
    case SlotValueKind::Cell:
        return reference();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<uint64_t> encodeFloatForSlot(double number)
{
    if (number == number && static_cast<double>(static_cast<float>(number)) != number)
        return std::nullopt;
    return std::bit_cast<uint32_t>(static_cast<float>(number));
}

static std::optional<uint64_t> encodeIntegerForSlot(const SlotTraits& traits, int64_t integer)
{
    switch (traits.valueKind) {
    case SlotValueKind::SignedInteger:
    case SlotValueKind::UnsignedInteger:
        if (integer < traits.minInteger || integer > traits.maxInteger)
            return std::nullopt;
        return static_cast<uint64_t>(integer);
    case SlotValueKind::Double:
        return std::bit_cast<uint64_t>(static_cast<double>(integer));
    case SlotValueKind::Float:
        return encodeFloatForSlot(static_cast<double>(integer));
    case SlotValueKind::Boxed:
        return JSValue::encode(jsNumber(static_cast<double>(integer)));
    default:
        return std::nullopt;
    }
}

static std::optional<uint64_t> encodeNumberForSlot(const SlotTraits& traits, double number)
{
    switch (traits.valueKind) {
    case SlotValueKind::SignedInteger:
    case SlotValueKind::UnsignedInteger:
        return Typed::isTrackedInteger(number) ? encodeIntegerForSlot(traits, static_cast<int64_t>(number)) : std::nullopt;
    case SlotValueKind::Double:
        return std::bit_cast<uint64_t>(purifyNaN(number));
    case SlotValueKind::Float:
        return encodeFloatForSlot(purifyNaN(number));
    case SlotValueKind::Boxed:
        return JSValue::encode(jsNumber(purifyNaN(number)));
    default:
        return std::nullopt;
    }
}

// The strings are asked only for a cell.
static NEVER_INLINE std::optional<uint64_t> encodeForSlot(ProgramStrings* strings, const SlotTraits& traits, JSValue value)
{
    // The empty value goes where the markers are the encoded values.
    if (!value)
        return traits.hasUndefined && traits.undefinedBits == JSValue::ValueUndefined ? std::optional<uint64_t>(traits.absentBits) : std::nullopt;
    if (traits.hasUndefined && value.isUndefined())
        return traits.undefinedBits;
    if (traits.hasNull && value.isNull())
        return traits.nullBits;
    auto reference = [&]() -> uint64_t {
        return traits.isNarrowReference ? MarkedBlockCage::encode(strings->instance().cellCageBase, value.asCell()) : JSValue::encode(value);
    };
    switch (traits.valueKind) {
    case SlotValueKind::SignedInteger:
    case SlotValueKind::UnsignedInteger:
    case SlotValueKind::Float:
    case SlotValueKind::Double:
        return value.isNumber() ? encodeNumberForSlot(traits, value.asNumber()) : std::nullopt;
    case SlotValueKind::Boolean:
        if (!value.isBoolean())
            return std::nullopt;
        return traits.hasAbsent ? JSValue::encode(value) : value.asBoolean();
    case SlotValueKind::Boxed:
        return JSValue::encode(value);
    case SlotValueKind::LiteralString: {
        if (!value.isString())
            return std::nullopt;
        uint64_t word = strings->literalStringWordFor(asString(value));
        return !traits.isNarrowReference || word < minCell32 ? word : reference();
    }
    case SlotValueKind::DeferredClosure:
        if (value.isUndefinedOrNull())
            return JSValue::encode(value);
        [[fallthrough]];
    case SlotValueKind::Cell:
        return value.isCell() ? std::optional<uint64_t>(reference()) : std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool writeSlotIfEncoded(SlotRepresentation representation, void* slot, std::optional<uint64_t> bits)
{
    if (bits)
        writeSlotBits(representation, slot, *bits);
    return !!bits;
}

bool tryWriteSlot(ProgramStrings& strings, SlotRepresentation representation, void* slot, JSValue value) { return writeSlotIfEncoded(representation, slot, encodeForSlot(&strings, traitsOf(representation), value)); }
bool tryWriteInt32(SlotRepresentation representation, void* slot, int32_t number) { return writeSlotIfEncoded(representation, slot, encodeIntegerForSlot(traitsOf(representation), number)); }
bool tryWriteDouble(SlotRepresentation representation, void* slot, double number) { return writeSlotIfEncoded(representation, slot, encodeNumberForSlot(traitsOf(representation), number)); }
bool tryWriteBoolean(SlotRepresentation representation, void* slot, bool value) { return writeSlotIfEncoded(representation, slot, encodeForSlot(nullptr, traitsOf(representation), jsBoolean(value))); }

JSCell* cellInSlot(SlotRepresentation representation, const void* slot, uintptr_t cageBase)
{
    SlotTraits traits = traitsOf(representation);
    if (!traits.holdsCell)
        return nullptr;
    if (!traits.isNarrowReference) {
        uint64_t bits = *static_cast<const uint64_t*>(slot);
        if (isLiteralStringWord(bits))
            return nullptr;
        JSValue value = JSValue::decode(bits);
        return value.isCell() ? value.asCell() : nullptr;
    }
    uint32_t word = *static_cast<const uint32_t*>(slot);
    return word < minCell32 ? nullptr : MarkedBlockCage::decode(cageBase, word);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
