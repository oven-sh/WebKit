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
    auto as = [&]<typename T>(T) {
        return *static_cast<const T*>(slot);
    };
    auto decode = [&](uint32_t word) {
        return JSValue(MarkedBlockCage::decode(strings.instance().cellCageBase, word));
    };
    switch (representation) {
    case SlotRepresentation::Boolean:
        return jsBoolean(as(uint8_t()));
    case SlotRepresentation::Uint8:
        return jsNumber(as(uint8_t()));
    case SlotRepresentation::Int8:
        return jsNumber(as(int8_t()));
    case SlotRepresentation::Uint16:
        return jsNumber(as(uint16_t()));
    case SlotRepresentation::Int16:
        return jsNumber(as(int16_t()));
    case SlotRepresentation::Int32:
        return jsNumber(as(int32_t()));
    case SlotRepresentation::Uint32:
        return jsNumber(as(uint32_t()));
    case SlotRepresentation::Int64:
        return jsNumber(static_cast<double>(as(int64_t())));
    case SlotRepresentation::Float:
        return jsNumber(withoutPayload(as(float())));
    case SlotRepresentation::Double:
        return jsNumber(withoutPayload(as(double())));
    case SlotRepresentation::Cell:
    case SlotRepresentation::JSValue:
        return JSValue::decode(as(EncodedJSValue()));
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32: {
        uint32_t word = as(uint32_t());
        return word <= undefinedCell32 ? JSValue::decode(word) : decode(word);
    }
    case SlotRepresentation::OptionalBoolean:
        return JSValue::decode(as(uint8_t()));
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32: {
        uint64_t word = representation == SlotRepresentation::LiteralString ? as(uint64_t()) : as(uint32_t());
        if (isLiteralStringWord(word))
            return strings.stringForLiteralStringWord(word);
        return representation == SlotRepresentation::LiteralString || word <= undefinedCell32 ? JSValue::decode(word) : decode(static_cast<uint32_t>(word));
    }
    case SlotRepresentation::OptionalInt64:
        switch (int64_t integer = as(int64_t())) {
        case undefinedInteger:
            return jsUndefined();
        case nullInteger:
            return jsNull();
        case absentInteger:
            return { };
        default:
            return jsNumber(static_cast<double>(integer));
        }
    case SlotRepresentation::OptionalDouble:
        switch (as(uint64_t())) {
        case undefinedDouble:
            return jsUndefined();
        case nullDouble:
            return jsNull();
        case absentDouble:
            return { };
        default:
            return jsNumber(withoutPayload(as(double())));
        }
    case SlotRepresentation::OptionalInt32:
        switch (int32_t word = as(int32_t())) {
        case undefinedInt32:
            return jsUndefined();
        case nullInt32:
            return jsNull();
        case absentInt32:
            return { };
        default:
            RELEASE_ASSERT(word >= minInlineInt32);
            return jsNumber(word);
        }
    case SlotRepresentation::DeferredClosure: {
        // Only the object can make the closure that its slot still defers. Undefined and null are the words that any narrow reference has for them.
        uint32_t word = as(uint32_t());
        if (word == absentDeferredClosure)
            return { };
        if (word == undefinedCell32 || word == nullCell32)
            return JSValue::decode(word);
        RELEASE_ASSERT(word >= minCell32);
        return decode(word);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename T>
static bool storeInSlot(void* slot, T bits)
{
    *static_cast<T*>(slot) = bits;
    return true;
}

static bool tryWriteInteger(SlotRepresentation representation, void* slot, int64_t integer)
{
    auto writeIfInRange = [&]<typename T>(int64_t minimum, int64_t maximum, T) {
        return integer >= minimum && integer <= maximum && storeInSlot(slot, static_cast<T>(integer));
    };
    switch (representation) {
    case SlotRepresentation::Uint8:
        return writeIfInRange(0, UINT8_MAX, uint8_t());
    case SlotRepresentation::Int8:
        return writeIfInRange(INT8_MIN, INT8_MAX, int8_t());
    case SlotRepresentation::Uint16:
        return writeIfInRange(0, UINT16_MAX, uint16_t());
    case SlotRepresentation::Int16:
        return writeIfInRange(INT16_MIN, INT16_MAX, int16_t());
    case SlotRepresentation::Int32:
        return writeIfInRange(INT32_MIN, INT32_MAX, int32_t());
    case SlotRepresentation::Uint32:
        return writeIfInRange(0, UINT32_MAX, uint32_t());
    case SlotRepresentation::Int64:
    case SlotRepresentation::OptionalInt64:
        return writeIfInRange(-maxSlotInteger, maxSlotInteger, int64_t());
    case SlotRepresentation::OptionalInt32:
        return writeIfInRange(minInlineInt32, INT32_MAX, int32_t());
    case SlotRepresentation::Float:
    case SlotRepresentation::Double:
    case SlotRepresentation::OptionalDouble:
    case SlotRepresentation::JSValue:
    case SlotRepresentation::Cell:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
    case SlotRepresentation::Boolean:
    case SlotRepresentation::OptionalBoolean:
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
    case SlotRepresentation::DeferredClosure:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool tryWriteFloat(void* slot, double number)
{
    if (number == number && static_cast<double>(static_cast<float>(number)) != number)
        return false;
    return storeInSlot(slot, static_cast<float>(number));
}

bool tryWriteSlot(ProgramStrings& strings, SlotRepresentation representation, void* slot, JSValue value)
{
    auto number = [&] {
        return value.isNumber() && tryWriteDouble(representation, slot, value.asNumber());
    };
    auto marker = [&](auto undefinedMarker, auto nullMarker) {
        return value.isUndefinedOrNull() && storeInSlot(slot, value.isUndefined() ? undefinedMarker : nullMarker);
    };
    auto cell32 = [&] {
        return value && value.isCell() && storeInSlot(slot, MarkedBlockCage::encode(strings.instance().cellCageBase, value.asCell()));
    };
    switch (representation) {
    case SlotRepresentation::Boolean:
        return value.isBoolean() && storeInSlot(slot, static_cast<uint8_t>(value.asBoolean()));
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::Int32:
    case SlotRepresentation::Uint32:
    case SlotRepresentation::Int64:
    case SlotRepresentation::Float:
    case SlotRepresentation::Double:
        return number();
    case SlotRepresentation::OptionalInt64:
        return marker(undefinedInteger, nullInteger) || number();
    case SlotRepresentation::OptionalDouble:
        return marker(undefinedDouble, nullDouble) || number();
    case SlotRepresentation::OptionalInt32:
        return marker(undefinedInt32, nullInt32) || number();
    case SlotRepresentation::Cell:
        if (!value || !value.isCell())
            return false;
        [[fallthrough]];
    case SlotRepresentation::JSValue:
        return storeInSlot(slot, JSValue::encode(value));
    case SlotRepresentation::OptionalCell32:
        if (!value || value.isUndefinedOrNull())
            return storeInSlot(slot, static_cast<uint32_t>(JSValue::encode(value)));
        [[fallthrough]];
    case SlotRepresentation::Cell32:
        return cell32();
    case SlotRepresentation::DeferredClosure:
        if (value && value.isUndefinedOrNull())
            return storeInSlot(slot, static_cast<uint32_t>(JSValue::encode(value)));
        return cell32();
    case SlotRepresentation::OptionalBoolean:
        return (!value || value.isUndefinedOrNull() || value.isBoolean()) && storeInSlot(slot, static_cast<uint8_t>(JSValue::encode(value)));
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32: {
        if (value && !value.isUndefinedOrNull() && !value.isString())
            return false;
        uint64_t word = value && value.isString() ? strings.literalStringWordFor(asString(value)) : static_cast<uint64_t>(JSValue::encode(value));
        if (representation == SlotRepresentation::LiteralString)
            return storeInSlot(slot, word);
        return word < minCell32 ? storeInSlot(slot, static_cast<uint32_t>(word)) : cell32();
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool tryWriteInt32(SlotRepresentation representation, void* slot, int32_t number)
{
    switch (representation) {
    case SlotRepresentation::Int32:
        return storeInSlot(slot, number);
    case SlotRepresentation::Int64:
    case SlotRepresentation::OptionalInt64:
        return storeInSlot(slot, static_cast<int64_t>(number));
    case SlotRepresentation::Double:
    case SlotRepresentation::OptionalDouble:
        return storeInSlot(slot, static_cast<double>(number));
    case SlotRepresentation::JSValue:
        return storeInSlot(slot, JSValue::encode(jsNumber(number)));
    case SlotRepresentation::Float:
        return tryWriteFloat(slot, number);
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::Uint32:
    case SlotRepresentation::OptionalInt32:
        return tryWriteInteger(representation, slot, number);
    case SlotRepresentation::Cell:
    case SlotRepresentation::Boolean:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
    case SlotRepresentation::OptionalBoolean:
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
    case SlotRepresentation::DeferredClosure:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool tryWriteDouble(SlotRepresentation representation, void* slot, double number)
{
    switch (representation) {
    case SlotRepresentation::Double:
    case SlotRepresentation::OptionalDouble:
        return storeInSlot(slot, withoutPayload(number));
    case SlotRepresentation::JSValue:
        return storeInSlot(slot, JSValue::encode(jsNumber(withoutPayload(number))));
    case SlotRepresentation::Float:
        return tryWriteFloat(slot, withoutPayload(number));
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::Int32:
    case SlotRepresentation::Uint32:
    case SlotRepresentation::Int64:
    case SlotRepresentation::OptionalInt64:
    case SlotRepresentation::OptionalInt32:
        return Typed::isTrackedInteger(number) && tryWriteInteger(representation, slot, static_cast<int64_t>(number));
    case SlotRepresentation::Cell:
    case SlotRepresentation::Boolean:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
    case SlotRepresentation::OptionalBoolean:
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
    case SlotRepresentation::DeferredClosure:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool tryWriteBoolean(SlotRepresentation representation, void* slot, bool value)
{
    switch (representation) {
    case SlotRepresentation::Boolean:
        return storeInSlot(slot, static_cast<uint8_t>(value));
    case SlotRepresentation::OptionalBoolean:
        return storeInSlot(slot, static_cast<uint8_t>(JSValue::encode(jsBoolean(value))));
    case SlotRepresentation::JSValue:
        return storeInSlot(slot, JSValue::encode(jsBoolean(value)));
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::Int32:
    case SlotRepresentation::Uint32:
    case SlotRepresentation::Int64:
    case SlotRepresentation::Float:
    case SlotRepresentation::Double:
    case SlotRepresentation::Cell:
    case SlotRepresentation::OptionalInt64:
    case SlotRepresentation::OptionalDouble:
    case SlotRepresentation::OptionalInt32:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
    case SlotRepresentation::DeferredClosure:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

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
