/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAssignmentPlans.h"
#include "AOTCellFormat.h"
#include "AOTInstance.h"
#include "AOTSlotAccess.h"
#include "FreeListInlines.h"
#include "LocalAllocator.h"
#include "MarkedBlockCage.h"
#include <array>
#include <bit>
#include <wtf/Atomics.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

// False where the slot holds nothing. The type of what store gets is as wide as the slot of the target. What takes more than loads is left to moveInOtherWay.
template<typename Store, typename Functor>
ALWAYS_INLINE bool tryMove(uintptr_t cellCageBase, const MappedCopy::Move& move, const uint8_t* slot, const Store& store, const Functor& moveInOtherWay, const uint32_t* literalStrings = nullptr)
{
    using Kind = MappedCopy::MoveKind;
    auto load = [&]<typename T>(T) ALWAYS_INLINE_LAMBDA {
        return *reinterpret_cast<const T*>(slot);
    };
    auto storeUnless = [&]<typename T>(T bits, uint64_t absentBits) ALWAYS_INLINE_LAMBDA {
        if (bits == static_cast<T>(absentBits))
            return false;
        store(bits);
        return true;
    };
    auto storeAlways = [&]<typename T>(T bits) ALWAYS_INLINE_LAMBDA {
        store(bits);
        return true;
    };
    switch (move.kind) {
    case Kind::WordUnlessZero:
        return storeUnless(load(uint64_t()), 0);
    case Kind::HalfWordUnlessZero:
        return storeUnless(load(uint32_t()), 0);
    case Kind::ByteUnlessZero:
        return storeUnless(load(uint8_t()), 0);
    case Kind::OptionalInt32:
        return storeUnless(load(uint32_t()), traitsOf(SlotRepresentation::OptionalInt32).absentBits);
    case Kind::OptionalInt64:
        return storeUnless(load(uint64_t()), traitsOf(SlotRepresentation::OptionalInt64).absentBits);
    case Kind::OptionalDouble:
        return storeUnless(load(uint64_t()), traitsOf(SlotRepresentation::OptionalDouble).absentBits);
    case Kind::OptionalInt64UnlessMarker:
    case Kind::OptionalDoubleUnlessMarker: {
        static_assert(undefinedInteger < nullInteger && nullInteger < absentInteger && maxSlotInteger < undefinedInteger);
        static_assert(undefinedDouble < nullDouble && nullDouble < absentDouble && PNaNAsBits < undefinedDouble);
        static_assert(static_cast<uint64_t>(absentInteger - undefinedInteger) == absentDouble - undefinedDouble);
        uint64_t bits = load(uint64_t());
        if (bits - (move.kind == Kind::OptionalInt64UnlessMarker ? static_cast<uint64_t>(undefinedInteger) : undefinedDouble) <= absentDouble - undefinedDouble) [[unlikely]]
            return moveInOtherWay();
        return storeAlways(bits);
    }
    case Kind::OptionalBooleanUnlessMarker: {
        static_assert((JSValue::ValueFalse | 1) == JSValue::ValueTrue);
        uint8_t bits = load(uint8_t());
        if ((bits | 1) != JSValue::ValueTrue) [[unlikely]]
            return moveInOtherWay();
        return storeAlways(bits);
    }
    case Kind::JSValueUnlessMarker: {
        static_assert(JSValue::ValueEmpty < JSValue::ValueUndefined && JSValue::ValueNull < JSValue::ValueUndefined);
        uint64_t bits = load(uint64_t());
        if (bits <= JSValue::ValueUndefined) [[unlikely]]
            return moveInOtherWay();
        return storeAlways(bits);
    }
    case Kind::Word:
        return storeAlways(load(uint64_t()));
    case Kind::HalfWord:
        return storeAlways(load(uint32_t()));
    case Kind::QuarterWord:
        return storeAlways(load(uint16_t()));
    case Kind::Byte:
        return storeAlways(load(uint8_t()));
    case Kind::BoxedOptionalInt32:
    case Kind::OptionalInt32AsDouble: {
        int32_t integer = load(int32_t());
        if (integer < minInlineInt32) [[unlikely]]
            return integer != absentInt32 && moveInOtherWay();
        return storeAlways(move.kind == Kind::BoxedOptionalInt32 ? static_cast<uint64_t>(JSValue::encode(jsNumber(integer))) : std::bit_cast<uint64_t>(static_cast<double>(integer)));
    }
    case Kind::OptionalInt32AsOptionalInt64: {
        int32_t integer = load(int32_t());
        if (integer < minInlineInt32) [[unlikely]]
            return integer != absentInt32 && moveInOtherWay();
        return storeAlways(static_cast<uint64_t>(static_cast<int64_t>(integer)));
    }
    case Kind::Int32AsInt64:
        return storeAlways(static_cast<uint64_t>(static_cast<int64_t>(load(int32_t()))));
    case Kind::BoxedOptionalInt64:
    case Kind::OptionalInt64AsOptionalInt32:
    case Kind::OptionalInt64AsDouble: {
        int64_t wideInteger = load(int64_t());
        int32_t integer = static_cast<int32_t>(wideInteger);
        if (integer != wideInteger || integer < minInlineInt32) [[unlikely]]
            return wideInteger != absentInteger && moveInOtherWay();
        if (move.kind == Kind::OptionalInt64AsOptionalInt32)
            return storeAlways(static_cast<uint32_t>(integer));
        return storeAlways(move.kind == Kind::BoxedOptionalInt64 ? static_cast<uint64_t>(JSValue::encode(jsNumber(integer))) : std::bit_cast<uint64_t>(static_cast<double>(integer)));
    }
    case Kind::BoxedInt32:
        return storeAlways(static_cast<uint64_t>(JSValue::encode(jsNumber(load(int32_t())))));
    case Kind::Int32AsOptionalInt32: {
        int32_t integer = load(int32_t());
        if (integer < minInlineInt32) [[unlikely]]
            return moveInOtherWay();
        return storeAlways(static_cast<uint32_t>(integer));
    }
    case Kind::Int32AsDouble:
        return storeAlways(std::bit_cast<uint64_t>(static_cast<double>(load(int32_t()))));
    case Kind::WideNarrowReference: {
        uint64_t bits = load(uint32_t());
        if (bits > undefinedCell32) [[likely]]
            bits = std::bit_cast<uint64_t>(MarkedBlockCage::decode(cellCageBase, static_cast<uint32_t>(bits)));
        return storeUnless(bits, 0);
    }
    case Kind::NarrowWideReference: {
        uint64_t bits = load(uint64_t());
        if (bits - cellCageBase >= MarkedBlockCage::maxSize) [[unlikely]]
            return bits && moveInOtherWay();
        return storeAlways(static_cast<uint32_t>(bits >> MarkedBlockCage::shift));
    }
    case Kind::NarrowLiteralString: {
        // The word of a literal string names its cell once somebody has made it.
        uint32_t word = load(uint32_t());
        if (isLiteralStringWord(word))
            word = literalStrings[literalStringIndex(word)];
        if (word <= undefinedCell32) [[unlikely]]
            return load(uint32_t()) != absentCell32 && moveInOtherWay();
        return storeAlways(word);
    }
    case Kind::WideLiteralString: {
        uint64_t bits = load(uint64_t());
        if (isLiteralStringWord(bits)) {
            uint32_t word = literalStrings ? literalStrings[literalStringIndex(static_cast<uint32_t>(bits))] : 0;
            if (!word) [[unlikely]]
                return moveInOtherWay();
            return storeAlways(std::bit_cast<uint64_t>(MarkedBlockCage::decode(cellCageBase, word)));
        }
        if (bits <= JSValue::ValueUndefined) [[unlikely]]
            return bits != JSValue::ValueEmpty && moveInOtherWay();
        return storeAlways(bits);
    }
    case Kind::BoxedAsInt32:
    case Kind::BoxedAsInt64:
    case Kind::BoxedAsDouble: {
        JSValue value = JSValue::decode(load(uint64_t()));
        if (value.isInt32()) [[likely]] {
            int32_t integer = value.asInt32();
            if (integer < move.leastInt32 || integer > move.greatestInt32) [[unlikely]]
                return moveInOtherWay();
            if (move.kind == Kind::BoxedAsInt32)
                return storeAlways(static_cast<uint32_t>(integer));
            return storeAlways(move.kind == Kind::BoxedAsInt64 ? static_cast<uint64_t>(static_cast<int64_t>(integer)) : std::bit_cast<uint64_t>(static_cast<double>(integer)));
        }
        if (move.kind == Kind::BoxedAsDouble && value.isDouble() && move.acceptedKinds & static_cast<uint8_t>(ValueKind::Double))
            return storeAlways(std::bit_cast<uint64_t>(withoutPayload(value.asDouble())));
        return moveInOtherWay();
    }
    case Kind::BoxedAsNarrowString: {
        JSValue value = JSValue::decode(load(uint64_t()));
        if (!value.isCell() || !value.asCell()->isString()) [[unlikely]]
            return moveInOtherWay();
        return storeAlways(static_cast<uint32_t>(load(uint64_t()) >> MarkedBlockCage::shift));
    }
    case Kind::BoxedAsJSValue: {
        JSValue value = JSValue::decode(load(uint64_t()));
        bool isTaken;
        if (value.isInt32())
            isTaken = value.asInt32() >= move.leastInt32 && value.asInt32() <= move.greatestInt32;
        else if (value.isCell())
            isTaken = value && value.asCell()->isString() && move.acceptedKinds & static_cast<uint8_t>(ValueKind::String);
        else if (value.isDouble())
            isTaken = move.acceptedKinds & static_cast<uint8_t>(ValueKind::Double);
        else
            isTaken = value.isBoolean() && move.acceptedKinds & static_cast<uint8_t>(value.isTrue() ? ValueKind::True : ValueKind::False);
        if (!isTaken) [[unlikely]]
            return moveInOtherWay();
        return storeAlways(load(uint64_t()));
    }
    case Kind::OtherForm:
    case Kind::Tested:
        return !slotHoldsAbsent(move.representationOfSource, slot) && moveInOtherWay();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE void storeBitsOfWidth(unsigned width, uint8_t* slot, uint64_t bits)
{
    if (width == sizeof(uint64_t))
        *reinterpret_cast<uint64_t*>(slot) = bits;
    else if (width == sizeof(uint32_t))
        *reinterpret_cast<uint32_t*>(slot) = static_cast<uint32_t>(bits);
    else if (width == sizeof(uint8_t))
        *slot = static_cast<uint8_t>(bits);
    else
        *reinterpret_cast<uint16_t*>(slot) = static_cast<uint16_t>(bits);
}

// It collects nothing, throws nothing and tells nobody, so it runs before the operation has a frame. While the collector marks, every owner needs a barrier, so no thread of it sees the target here.
ALWAYS_INLINE bool tryCopyByFieldMapWithoutFrame(Instance& instance, JSCell* target, EncodedJSValue encodedSource, uint32_t excludedRunNumberPlusOne)
{
    static_assert(std::endian::native == std::endian::little && offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t));
    JSValue sourceValue = JSValue::decode(encodedSource);
    if (!sourceValue.isCell())
        return false;
    const JSCell* source = sourceValue.asCell();
    uint8_t* cellOfTarget = reinterpret_cast<uint8_t*>(target);
    const uint8_t* cellOfSource = reinterpret_cast<const uint8_t*>(source);
    uint64_t targetWords = *reinterpret_cast<const uint64_t*>(cellOfTarget + offsetOfOrderWord);
    uint64_t sourceWords = *reinterpret_cast<const uint64_t*>(cellOfSource + offsetOfOrderWord);
    const MappedCopy* copy = instance.copiesWithoutFrame()[AllocationWords::placeOfCopyWithoutFrame(targetWords, sourceWords, excludedRunNumberPlusOne)];
    if (!copy || copy->targetWords != targetWords || copy->sourceWords != sourceWords || copy->excludedRunNumberPlusOne != excludedRunNumberPlusOne)
        return false;
    if (copy->structureOfTarget != target->structureID() || copy->structureOfSource != source->structureID())
        return false;
    if (static_cast<uint8_t>(target->cellState()) <= instance.barrierThreshold || target->structure()->mayBePrototype())
        return false;

    std::array<const uint8_t*, 2> storagesOfSource { cellOfSource, nullptr };
    if (copy->granuleBitsOfSource) {
        if (copy->readsOrdinaryObject) {
            for (const MappedCopy::Move& move : copy->moves) {
                uint8_t* slotOfTarget = cellOfTarget + move.to;
                bool didMove = tryMove(instance.cellCageBase, move, cellOfSource + move.from, [&]<typename T>(T bits) ALWAYS_INLINE_LAMBDA {
                    *reinterpret_cast<T*>(slotOfTarget) = bits;
                }, [] {
                    return false;
                });
                if (!didMove) [[unlikely]]
                    return clearFieldsMovedInVain(*copy, &move, target);
            }
            *reinterpret_cast<uint32_t*>(cellOfTarget + offsetOfOrderWord) = copy->orderWordAfter;
            return true;
        }
        const uint8_t* word = cellOfSource + copy->offsetOfExtensionWordInSource;
        if (copy->extensionWordOfSourceIsWide) [[unlikely]] {
            storagesOfSource[1] = *reinterpret_cast<const uint8_t* const*>(word);
            if (!storagesOfSource[1])
                return false;
        } else
            storagesOfSource[1] = reinterpret_cast<const uint8_t*>(MarkedBlockCage::decode(instance.cellCageBase, *reinterpret_cast<const uint32_t*>(word))) + offsetOfEmptyExtensionInCage;
        if (*reinterpret_cast<const uint64_t*>(storagesOfSource[1] + offsetOfExtensionGranuleBits) != copy->granuleBitsOfSource)
            return false;
    }
    std::array<uint8_t*, 2> storagesOfTarget { cellOfTarget, nullptr };
    if (copy->granuleBitsFilled) {
        const uint8_t* word = cellOfTarget + copy->offsetOfExtensionWordInTarget;
        if (copy->extensionWordOfTargetIsWide ? !!*reinterpret_cast<const uint64_t*>(word) : !!*reinterpret_cast<const uint32_t*>(word))
            return false;
        FreeList& freeList = *reinterpret_cast<FreeList*>(reinterpret_cast<uint8_t*>(copy->allocatorOfNewExtension) + LocalAllocator::offsetOfFreeList());
        uint8_t* extension = reinterpret_cast<uint8_t*>(freeList.allocateWithCellSize([]() -> HeapCell* {
            return nullptr;
        }, copy->sizeOfNewExtension));
        if (!extension)
            return false;
        static_assert(minExtensionSize == 2 * sizeof(uint64_t));
        auto clearChunkAt = [&](uint32_t offset) ALWAYS_INLINE_LAMBDA {
            reinterpret_cast<uint64_t*>(extension + offset)[0] = 0;
            reinterpret_cast<uint64_t*>(extension + offset)[1] = 0;
            WTF::compilerFence();
        };
        clearChunkAt(0);
        for (uint32_t offset = minExtensionSize; offset < copy->sizeOfNewExtension; offset += minExtensionSize)
            clearChunkAt(offset);
        *reinterpret_cast<uint64_t*>(extension + offsetOfExtensionGranuleBits) = copy->granuleBitsFilled;
        storagesOfTarget[1] = extension;
    }

    for (const MappedCopy::Move& move : copy->moves) {
        uint8_t* slotOfTarget = storagesOfTarget[move.writesExtension] + move.to;
        bool didMove = tryMove(instance.cellCageBase, move, storagesOfSource[move.readsExtension] + move.from, [&]<typename T>(T bits) ALWAYS_INLINE_LAMBDA {
            *reinterpret_cast<T*>(slotOfTarget) = bits;
        }, [] {
            return false;
        }, instance.runtime.constants.literalStrings);
        if (!didMove) [[unlikely]]
            return clearFieldsMovedInVain(*copy, &move, target);
    }
    if (storagesOfTarget[1]) {
        uint8_t* word = cellOfTarget + copy->offsetOfExtensionWordInTarget;
        if (copy->extensionWordOfTargetIsWide) [[unlikely]]
            *reinterpret_cast<uint8_t**>(word) = storagesOfTarget[1];
        else
            *reinterpret_cast<uint32_t*>(word) = static_cast<uint32_t>((std::bit_cast<uintptr_t>(storagesOfTarget[1]) - offsetOfEmptyExtensionInCage) >> MarkedBlockCage::shift);
    }
    *reinterpret_cast<uint32_t*>(cellOfTarget + offsetOfOrderWord) = copy->orderWordAfter;
    return true;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
