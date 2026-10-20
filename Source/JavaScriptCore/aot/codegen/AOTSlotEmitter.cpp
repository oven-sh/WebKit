/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotEmitter.h"

#if ENABLE(AOT)

#include "AOTBarrierEmitter.h"
#include "AOTCellFormat.h"
#include "AOTConversionEmitter.h"
#include "AOTFaultTable.h"
#include "AOTPlacement.h"
#include "AOTRepresentation.h"
#include "AOTStubsAllocation.h"
#include <bit>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(minExtensionSize == copyChunkBytes && minExtensionSize <= maxExtensionSizeFromStub);
static_assert(JSValue::ValueTrue <= UINT8_MAX && JSValue::ValueUndefined <= UINT8_MAX);
static_assert(absentInt32 < nullInt32 && undefinedInt32 < nullInt32 && nullInt32 < doubleInExtensionInt32 && doubleInExtensionInt32 < minInlineInt32);

static constexpr SlotTraits traitsOfLoaded(SlotRepresentation slot)
{
    switch (slot) {
    case SlotRepresentation::Int32OrDouble:
        return traitsOf(SlotRepresentation::OptionalDouble);
    case SlotRepresentation::OptionalInt32:
        return traitsOf(SlotRepresentation::OptionalInt64);
    default:
        return traitsOf(slot);
    }
}

static std::optional<int32_t> inlineWordOf(double number)
{
    switch (std::bit_cast<uint64_t>(number)) {
    case absentDouble:
        return absentInt32;
    case undefinedDouble:
        return undefinedInt32;
    case nullDouble:
        return nullInt32;
    }
    std::optional<int32_t> int32 = tryConvertToStrictInt32(number);
    return int32 && *int32 >= minInlineInt32 ? int32 : std::nullopt;
}

bool isStoredInCell(const PlacedSlot& slot, const Type& stored)
{
    if (slot.isOutOfLine())
        return false;
    if (slot.representation != SlotRepresentation::Int32OrDouble)
        return true;
    return stored.hasOnlyPrimitives(Primitive::Integer | Primitive::UndefinedOrNull | Primitive::Absent) && stored.integers.isSubsetOf(inlineInt32Range);
}

SlotEmitter::SlotEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

LValue SlotEmitter::constantWithBits(LType type, uint64_t bits)
{
    if (type == B3::Int32)
        return m_out.constInt32(static_cast<int32_t>(bits));
    if (type == B3::Int64)
        return m_out.constInt64(static_cast<int64_t>(bits));
    RELEASE_ASSERT(type == B3::Double);
    return m_out.constDouble(std::bit_cast<double>(bits));
}

LValue SlotEmitter::loadBits(TypedPointer pointer, SlotRepresentation slot)
{
    switch (widthOf(slot)) {
    case 1:
        return m_out.load8ZeroExt32(pointer);
    case 2:
        return m_out.load16ZeroExt32(pointer);
    case 4:
        return m_out.load32(pointer);
    case 8:
        return m_out.load64(pointer);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SlotEmitter::storeBits(TypedPointer pointer, SlotRepresentation slot, LValue bits)
{
    switch (widthOf(slot)) {
    case 1:
        m_out.store32As8(bits, pointer);
        return;
    case 2:
        m_out.store32As16(bits, pointer);
        return;
    case 4:
        m_out.store32(bits, pointer);
        return;
    case 8:
        m_out.store64(bits, pointer);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand SlotEmitter::load(TypedPointer pointer, SlotRepresentation slot)
{
    Operand loaded;
    switch (slot) {
    case SlotRepresentation::Uint8:
        loaded = Word32(m_out.load8ZeroExt32(pointer));
        break;
    case SlotRepresentation::Int8:
        loaded = Word32(m_out.load8SignExt32(pointer));
        break;
    case SlotRepresentation::Uint16:
        loaded = Word32(m_out.load16ZeroExt32(pointer));
        break;
    case SlotRepresentation::Int16:
        loaded = Word32(m_out.load16SignExt32(pointer));
        break;
    case SlotRepresentation::Int32:
        loaded = Word32(m_out.load32(pointer));
        break;
    case SlotRepresentation::Boolean:
        loaded = Condition(m_out.load8ZeroExt32(pointer));
        break;
    case SlotRepresentation::Uint32:
        loaded = zeroExtended(Word32(m_out.load32(pointer)));
        break;
    case SlotRepresentation::Int64:
    case SlotRepresentation::OptionalInt64:
    case SlotRepresentation::JSValue:
    case SlotRepresentation::Cell:
    case SlotRepresentation::LiteralString:
        loaded = Operand(Typed::representationOf(slot), m_out.load64(pointer));
        break;
    case SlotRepresentation::Double:
    case SlotRepresentation::OptionalDouble:
        loaded = Operand(Typed::representationOf(slot), m_out.loadDouble(pointer));
        break;
    case SlotRepresentation::Float:
        loaded = Number(m_out.floatToDouble(m_out.loadFloat(pointer)));
        break;
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
        loaded = NarrowReference(m_out.load32(pointer));
        break;
    case SlotRepresentation::LiteralString32: {
        NarrowReference word(m_out.load32(pointer));
        loaded = LiteralStringWord(m_out.select(isCellReference(word).value(), decode(word).value(), m_out.zeroExtPtr(word.value())));
        break;
    }
    case SlotRepresentation::OptionalBoolean:
        loaded = Boxed(m_out.zeroExtPtr(m_out.load8ZeroExt32(pointer)));
        break;
    case SlotRepresentation::DeferredClosure:
        loaded = Pointer(m_out.zeroExtPtr(m_out.load32(pointer)));
        break;
    case SlotRepresentation::OptionalInt32:
        loaded = widenOptionalInt32(Word32(m_out.load32(pointer)));
        break;
    case SlotRepresentation::Int32OrDouble:
        RELEASE_ASSERT_NOT_REACHED();
    }
    RELEASE_ASSERT(loaded.representation == Typed::representationOf(slot));
    return loaded;
}

OptionalInteger SlotEmitter::widenOptionalInt32(Word32 word)
{
    auto marker = [&](int64_t bits) {
        return OptionalInteger(m_out.constInt64(bits));
    };
    LBasicBlock isInteger = newBlock();
    LBasicBlock isMarker = newBlock();
    Join<OptionalInteger> join(m_out);
    branch(lessThan(word, word32(minInlineInt32)), { unlikelyEdge(isMarker), normalEdge(isInteger) });

    m_out.appendTo(isInteger);
    join.add(OptionalInteger(signExtended(word).value()));

    m_out.appendTo(isMarker);
    join.add(select(equal(word, word32(absentInt32)), marker(absentInteger), select(equal(word, word32(undefinedInt32)), marker(undefinedInteger), marker(nullInteger))));
    return join.finish();
}

Word32 SlotEmitter::narrowOptionalInt32(OptionalInteger value)
{
    if (value.value()->opcode() == B3::SExt32)
        return Word32(value.value()->child(0));

    Integer bits(value.value());
    auto is = [&](int64_t marker) {
        return equal(bits, Integer(m_out.constInt64(marker)));
    };
    Word32 marker = select(is(absentInteger), word32(absentInt32), select(is(undefinedInteger), word32(undefinedInt32), word32(nullInt32)));
    return select(greaterThan(bits, Integer(m_out.constInt64(maxSlotInteger))), marker, low32(bits));
}

void SlotEmitter::store(TypedPointer pointer, SlotRepresentation slot, Operand operand)
{
    RELEASE_ASSERT(operand.representation == Typed::representationOf(slot));
    switch (slot) {
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Boolean:
        m_out.store32As8(operand.value, pointer);
        return;
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
        m_out.store32As16(operand.value, pointer);
        return;
    case SlotRepresentation::Int32:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
        m_out.store32(operand.value, pointer);
        return;
    case SlotRepresentation::Uint32:
    case SlotRepresentation::DeferredClosure:
        m_out.store32(m_out.castToInt32(operand.value), pointer);
        return;
    case SlotRepresentation::OptionalInt32:
        m_out.store32(narrowOptionalInt32(operand.as<OptionalInteger>()).value(), pointer);
        return;
    case SlotRepresentation::Int64:
    case SlotRepresentation::OptionalInt64:
    case SlotRepresentation::Cell:
    case SlotRepresentation::JSValue:
    case SlotRepresentation::LiteralString:
        m_out.store64(operand.value, pointer);
        return;
    case SlotRepresentation::LiteralString32: {
        LiteralStringWord word = operand.as<LiteralStringWord>();
        Condition isStringCell = emitter<ConversionEmitter>().isStringCell(word);
        m_out.store32(m_out.select(isStringCell.value(), compress(CellPointer(word.value())).value(), m_out.castToInt32(word.value())), pointer);
        return;
    }
    case SlotRepresentation::OptionalBoolean:
        m_out.store32As8(m_out.castToInt32(operand.value), pointer);
        return;
    case SlotRepresentation::Float:
        m_out.storeFloat(m_out.doubleToFloat(operand.value), pointer);
        return;
    case SlotRepresentation::Double:
    case SlotRepresentation::OptionalDouble:
        m_out.storeDouble(operand.value, pointer);
        return;
    case SlotRepresentation::Int32OrDouble:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void SlotEmitter::storeAbsent(TypedPointer pointer, SlotRepresentation slot)
{
    SlotTraits traits = traitsOf(slot);
    RELEASE_ASSERT(traits.hasAbsent);
    storeBits(pointer, slot, constantWithBits(traits.width == sizeof(uint64_t) ? B3::Int64 : B3::Int32, traits.absentBits));
}

Condition SlotEmitter::holdsAbsent(TypedPointer pointer, SlotRepresentation slot)
{
    SlotTraits traits = traitsOf(slot);
    RELEASE_ASSERT(traits.hasAbsent);
    LValue bits = loadBits(pointer, slot);
    return Condition(m_out.equal(bits, constantWithBits(bits->type(), traits.absentBits)));
}

Condition SlotEmitter::isAbsent(Operand loaded, SlotRepresentation slot)
{
    SlotTraits traits = traitsOfLoaded(slot);
    RELEASE_ASSERT(traits.hasAbsent && loaded.representation == Typed::representationOf(slot));
    LValue bits = loaded.value->type() == B3::Double ? emitter<ConversionEmitter>().bitsOf(loaded).value() : loaded.value;
    return Condition(m_out.equal(bits, constantWithBits(bits->type(), traits.absentBits)));
}

Operand SlotEmitter::absentAsUndefined(Operand loaded, SlotRepresentation slot)
{
    SlotTraits traits = traitsOfLoaded(slot);
    RELEASE_ASSERT(traits.hasUndefined);
    LValue undefinedMarker = constantWithBits(loaded.value->type(), traits.undefinedBits);
    return Operand(loaded.representation, m_out.select(isAbsent(loaded, slot).value(), undefinedMarker, loaded.value));
}

Operand SlotEmitter::extensionWordOf(CellPointer cell, const PlacedSlot& extensionWord)
{
    RELEASE_ASSERT(extensionWord.storage == SlotStorage::Inline);
    return load(m_out.address(m_heaps.entire(AbstractHeap::ObjectFields), cell.value(), extensionWord.offset), extensionWord.representation);
}

Pointer SlotEmitter::extensionAt(Operand word)
{
    if (!word.is<NarrowReference>())
        return asPointer(word.as<CellPointer>());
    RELEASE_ASSERT(m_cellCageBase);
    static_assert(FaultSite::reach <= offsetOfEmptyExtensionInCage, "A load through a null narrow reference has to go on faulting.");
    LValue emptyExtension = m_out.add(m_cellCageBase, m_out.constIntPtr(offsetOfEmptyExtensionInCage));
    return Pointer(m_out.add(emptyExtension, m_out.shl(m_out.zeroExtPtr(word.value), m_out.constInt32(cell32Shift))));
}

Pointer SlotEmitter::extensionOf(CellPointer cell, const PlacedSlot& extensionWord)
{
    return extensionAt(extensionWordOf(cell, extensionWord));
}

Pointer SlotEmitter::branchUnlessExtensionHolds(CellPointer cell, const PlacedSlot& extensionWord, uint64_t granuleBits, B3::FrequentedBlock lacksGranule)
{
    Operand word = extensionWordOf(cell, extensionWord);
    if (!word.is<NarrowReference>()) {
        LBasicBlock hasExtension = newBlock();
        branch(isAbsent(word, extensionWord.representation), { lacksGranule, normalEdge(hasExtension) });
        m_out.appendTo(hasExtension);
    }
    Pointer extension = extensionAt(word);
    LBasicBlock holds = newBlock();
    branch(holdsGranules(extension, granuleBits), { normalEdge(holds), lacksGranule });
    m_out.appendTo(holds);
    return extension;
}

void SlotEmitter::ensureExtensionHolds(const OperationContext& context, CellPointer cell, const PlacedSlot& extensionWord, uint64_t granuleBits)
{
    unsigned size = extensionCellSizeFor(std::popcount(granuleBits));
    LBasicBlock done = newBlock();
    LBasicBlock lacksGranule = newBlock();
    branchUnlessExtensionHolds(cell, extensionWord, granuleBits, rareEdge(lacksGranule));
    m_out.jump(done);

    m_out.appendTo(lacksGranule);
    if (size <= maxExtensionSizeFromStub) {
        callStub(context, allocateExtension(size), { cell.value(), m_out.constIntPtr(extensionWord.offset), m_out.constInt64(static_cast<int64_t>(granuleBits)) }, { .isOnRarePath = true });
        emitter<BarrierEmitter>().barrier(context, cell);
    } else
        callVoidOperation(context, RuntimeOperation::EnsureExtensionGranules, { cell, integer(static_cast<int64_t>(granuleBits)) }, { .isOnRarePath = true });
    m_out.jump(done);

    m_out.appendTo(done);
}

// ARM64 counts bits in a vector register. For a few bits it is quicker to subtract: x - (x >> 1) - (x >> 2) .. is the number of bits set in x, and a shifted operand costs nothing there.
static constexpr uint32_t maxGranuleCountedBySubtraction = isARM64() ? 5 : 1;

// The bit of the granule becomes the sign and the bits of the granules behind it fall out, so one value answers whether the granule is held and how many are held up to it.
Integer SlotEmitter::granuleBitsUpTo(Pointer extension, uint32_t granule)
{
    return Integer(m_out.shl(loadField<Integer>(extension, m_heaps.AOTExtension_granuleBits).value(), m_out.constInt32(maxExtensionGranules - 1 - granule)));
}

Condition SlotEmitter::holdsGranules(Pointer extension, uint64_t granuleBits)
{
    if (hasOneBitSet(granuleBits) && static_cast<uint32_t>(std::countr_zero(granuleBits)) > maxGranuleCountedBySubtraction)
        return lessThan(granuleBitsUpTo(extension, std::countr_zero(granuleBits)), integer(0));
    Integer held = bitAnd(loadField<Integer>(extension, m_heaps.AOTExtension_granuleBits), integer(static_cast<int64_t>(granuleBits)));
    return hasOneBitSet(granuleBits) ? isNotZero(held) : equal(held, integer(static_cast<int64_t>(granuleBits)));
}

TypedPointer SlotEmitter::addressInExtension(const B3::AbstractHeap& heap, Pointer extension, uint32_t offset)
{
    static_assert(extensionGranuleSize == 8 && offsetOfFirstExtensionSlot == extensionGranuleSize);
    uint32_t granule = granuleOf(offset);
    if (!granule)
        return m_out.address(heap, extension.value(), offsetOfFirstExtensionSlot + offsetInGranule(offset));
    if (granule > maxGranuleCountedBySubtraction)
        return m_out.baseIndex(heap, extension.value(), m_out.popCount(granuleBitsUpTo(extension, granule).value()), FTL::ScaleEight, offsetInGranule(offset));

    Integer bitsBefore = bitAnd(loadField<Integer>(extension, m_heaps.AOTExtension_granuleBits), integer(static_cast<int64_t>((1ull << granule) - 1)));
    Integer count = bitsBefore;
    for (uint32_t shift = 1; shift < granule; ++shift)
        count = sub(count, Integer(m_out.lShr(bitsBefore.value(), m_out.constInt32(shift))));
    return m_out.baseIndex(heap, extension.value(), count.value(), FTL::ScaleEight, offsetOfFirstExtensionSlot + offsetInGranule(offset));
}

TypedPointer SlotEmitter::addressOf(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    const B3::AbstractHeap& heap = m_heaps.at(m_program, location);
    switch (slot.storage) {
    case SlotStorage::Inline:
        return m_out.address(heap, cell.value(), slot.offset);
    case SlotStorage::OutOfLine:
        RELEASE_ASSERT(extensionWord);
        return addressInExtension(heap, extensionOf(cell, *extensionWord), slot.offset);
    case SlotStorage::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypedPointer SlotEmitter::addressOfDouble(Pointer extension, const PlacedSlot& slot, const MemoryLocation& location)
{
    RELEASE_ASSERT(slot.doubleOffsetInExtension);
    return addressInExtension(m_heaps.at(m_program, location), extension, *slot.doubleOffsetInExtension);
}

Operand SlotEmitter::loadOutOfLine(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot& extensionWord)
{
    RELEASE_ASSERT(representsAbsentAsZero(slot.representation));
    Representation representation = Typed::representationOf(slot.representation);
    LBasicBlock hasNoStorage = newBlock();
    OperandJoin join(m_out, representation);
    Pointer extension = branchUnlessExtensionHolds(cell, extensionWord, granuleBitOf(slot.offset), normalEdge(hasNoStorage));
    Operand loaded = load(addressInExtension(m_heaps.at(m_program, location), extension, slot.offset), slot.representation);
    // The collector reaches the extension's slots through the cell alone, and B3 may load the extension once in front of a loop and let the cell die.
    if (traitsOf(slot.representation).holdsCell)
        ensureStillAliveHere(cell);
    join.add(loaded);
    m_out.appendTo(hasNoStorage);
    join.add(Operand(representation, constantWithBits(b3TypeOf(representation), traitsOf(slot.representation).absentBits)));
    return join.finish();
}

Operand SlotEmitter::loadInt32OrDouble(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot& extensionWord)
{
    RELEASE_ASSERT(Typed::representationOf(slot.representation) == OptionalNumber::representation);
    auto marker = [&](uint64_t bits) {
        return OptionalNumber(constantWithBits(B3::Double, bits));
    };
    Word32 word(m_out.load32(addressOf(cell, slot, location)));
    LBasicBlock isInline = newBlock();
    LBasicBlock isNotInline = newBlock();
    LBasicBlock isMarker = newBlock();
    LBasicBlock isInExtension = newBlock();
    Join<OptionalNumber> join(m_out);
    branch(lessThan(word, word32(minInlineInt32)), { unlikelyEdge(isNotInline), normalEdge(isInline) });

    m_out.appendTo(isInline);
    join.add(OptionalNumber(toNumber(word).value()));

    m_out.appendTo(isNotInline);
    branch(greaterThan(word, word32(nullInt32)), { normalEdge(isInExtension), normalEdge(isMarker) });

    m_out.appendTo(isMarker);
    join.add(select(equal(word, word32(absentInt32)), marker(absentDouble), select(equal(word, word32(undefinedInt32)), marker(undefinedDouble), marker(nullDouble))));

    m_out.appendTo(isInExtension);
    join.add(OptionalNumber(m_out.loadDouble(addressOfDouble(extensionOf(cell, extensionWord), slot, location))));
    return join.finish();
}

Operand SlotEmitter::loadSlot(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    if (!slot.needsExtension())
        return load(addressOf(cell, slot, location), slot.representation);
    RELEASE_ASSERT(extensionWord);
    if (slot.representation == SlotRepresentation::Int32OrDouble)
        return loadInt32OrDouble(cell, slot, location, *extensionWord);
    Operand loaded = loadOutOfLine(cell, slot, location, *extensionWord);
    // A field that an object lists before it has an extension reads as undefined.
    return traitsOf(slot.representation).hasUndefined ? absentAsUndefined(loaded, slot.representation) : loaded;
}

Operand SlotEmitter::loadSlot(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord, const Type& loadedType)
{
    if (slot.representation == SlotRepresentation::OptionalInt32 && !slot.needsExtension() && loadedType.isInteger())
        return Word32(m_out.load32(addressOf(cell, slot, location)));
    return loadSlot(cell, slot, location, extensionWord);
}

Condition SlotEmitter::isFilled(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    if (!traitsOf(slot.representation).hasAbsent)
        return boolean(true);
    if (!slot.isOutOfLine())
        return logicalNot(holdsAbsent(addressOf(cell, slot, location), slot.representation));
    RELEASE_ASSERT(extensionWord);
    Condition holdsValue = logicalNot(isAbsent(loadOutOfLine(cell, slot, location, *extensionWord), slot.representation));
    if (!slot.mayExistWithoutStorage)
        return holdsValue;
    // Absent bytes do not say that the field is absent: only the key order says whether the object lists it.
    RELEASE_ASSERT(!location.isEntireHeap());
    return either<Condition>(holdsValue, [&] {
        return boolean(true);
    }, [&] {
        return callWithoutSite<Condition>(RuntimeOperation::HasExtraProperty, { cell, identifierNumber(location.key()) }, { .isOnRarePath = true });
    });
}

void SlotEmitter::storeInt32OrDouble(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, Operand operand, NOESCAPE const Invocable<void(Number)> auto& storeDouble)
{
    TypedPointer inlineWord = addressOf(cell, slot, location);
    Number number(operand.as<OptionalNumber>().value());
    if (number.value()->hasDouble()) {
        std::optional<int32_t> word = inlineWordOf(number.value()->asDouble());
        if (!word)
            storeDouble(number);
        m_out.store32(m_out.constInt32(word.value_or(doubleInExtensionInt32)), inlineWord);
        return;
    }

    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    bool isConvertedInt32 = number.value()->opcode() == B3::IToD && number.value()->child(0)->type() == B3::Int32;
    Word32 word = isConvertedInt32 ? Word32(number.value()->child(0)) : low32(conversions.toInteger(number));
    Condition fits = greaterThanOrEqual(word, word32(minInlineInt32));
    if (!isConvertedInt32)
        fits = logicalAnd(fits, equal(conversions.bitsOf(toNumber(word)), conversions.bitsOf(number)));

    LBasicBlock fitsInline = newBlock();
    LBasicBlock doesNotFit = newBlock();
    LBasicBlock done = newBlock();
    branch(fits, { normalEdge(fitsInline), unlikelyEdge(doesNotFit) });

    m_out.appendTo(fitsInline);
    m_out.store32(word.value(), inlineWord);
    m_out.jump(done);

    m_out.appendTo(doesNotFit);
    if (!isConvertedInt32) {
        LBasicBlock isMarker = newBlock();
        LBasicBlock isDouble = newBlock();
        branch(conversions.isMarker(operand), { normalEdge(isMarker), normalEdge(isDouble) });

        m_out.appendTo(isMarker);
        Word32 marker = select(conversions.isMarker(operand, absentDouble), word32(absentInt32), select(conversions.isMarker(operand, undefinedDouble), word32(undefinedInt32), word32(nullInt32)));
        m_out.store32(marker.value(), inlineWord);
        m_out.jump(done);

        m_out.appendTo(isDouble);
    }
    // storeDouble may collect, so the word names the double only once it is there.
    storeDouble(number);
    m_out.store32(m_out.constInt32(doubleInExtensionInt32), inlineWord);
    m_out.jump(done);

    m_out.appendTo(done);
}

void SlotEmitter::storeDoubleInExtension(const OperationContext& context, CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, Number number, const PlacedSlot& extensionWord)
{
    RELEASE_ASSERT(slot.doubleOffsetInExtension);
    uint32_t offset = *slot.doubleOffsetInExtension;
    LBasicBlock needsExtension = newBlock();
    LBasicBlock done = newBlock();
    Pointer extension = branchUnlessExtensionHolds(cell, extensionWord, granuleBitOf(offset), rareEdge(needsExtension));
    m_out.storeDouble(number.value(), addressOfDouble(extension, slot, location));
    m_out.jump(done);

    m_out.appendTo(needsExtension);
    callVoidOperation(context, RuntimeOperation::StoreDoubleInExtension, { cell, word32(offset), number }, { .isOnRarePath = true });
    m_out.jump(done);

    m_out.appendTo(done);
}

void SlotEmitter::storeSlot(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, Operand operand, const PlacedSlot* extensionWord)
{
    if (slot.representation != SlotRepresentation::Int32OrDouble) {
        store(addressOf(cell, slot, location, extensionWord), slot.representation, operand);
        return;
    }
    RELEASE_ASSERT(extensionWord);
    storeInt32OrDouble(cell, slot, location, operand, [&](Number number) {
        m_out.storeDouble(number.value(), addressOfDouble(extensionOf(cell, *extensionWord), slot, location));
    });
}

void SlotEmitter::storeInExtension(Pointer extension, const PlacedSlot& slot, const MemoryLocation& location, Operand operand)
{
    RELEASE_ASSERT(slot.isOutOfLine() && slot.representation != SlotRepresentation::Int32OrDouble);
    store(addressInExtension(m_heaps.at(m_program, location), extension, slot.offset), slot.representation, operand);
}

void SlotEmitter::storeSlot(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, Operand operand, const PlacedSlot* extensionWord, const Type& storedType)
{
    if (slot.representation == SlotRepresentation::OptionalInt32 && storedType.isInteger()) {
        m_out.store32(m_out.castToInt32(operand.as<OptionalInteger>().value()), addressOf(cell, slot, location, extensionWord));
        return;
    }
    storeSlot(cell, slot, location, operand, extensionWord);
}

void SlotEmitter::storeSlot(const OperationContext& context, CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, Operand operand, const PlacedSlot* extensionWord)
{
    if (!slot.needsExtension()) {
        storeSlot(cell, slot, location, operand);
        return;
    }
    RELEASE_ASSERT(extensionWord);
    if (slot.representation == SlotRepresentation::Int32OrDouble) {
        storeInt32OrDouble(cell, slot, location, operand, [&](Number number) {
            storeDoubleInExtension(context, cell, slot, location, number, *extensionWord);
        });
        return;
    }
    ensureExtensionHolds(context, cell, *extensionWord, granuleBitOf(slot.offset));
    storeSlot(cell, slot, location, operand, extensionWord);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
