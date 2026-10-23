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
#include "AOTNarrowFieldTypes.h"
#include "AOTPlacement.h"
#include "AOTRepresentation.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubsAllocation.h"
#include <bit>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(minExtensionSize == copyChunkBytes && minExtensionSize <= maxExtensionSizeFromStub);
static_assert(JSValue::ValueTrue <= UINT8_MAX && JSValue::ValueUndefined <= UINT8_MAX);
static_assert(absentInt32 < minInlineInt32 && undefinedInt32 < minInlineInt32 && nullInt32 < minInlineInt32);
static_assert(maxSlotInteger < undefinedInteger && maxSlotInteger < nullInteger && maxSlotInteger < absentInteger);
static_assert(absentCell32 < undefinedCell32 && nullCell32 < undefinedCell32 && undefinedCell32 < emptyLiteralStringWord && undefinedCell32 < minCell32);

// Of what a slot with boxed markers holds, only the markers have no other bit than these.
static constexpr int64_t bitsOfBoxedMarkers = JSValue::ValueUndefined | JSValue::ValueNull;
static constexpr bool hasOnlyBitsOfBoxedMarkers(int64_t bits) { return !(bits & ~bitsOfBoxedMarkers); }
static_assert(hasOnlyBitsOfBoxedMarkers(JSValue::ValueEmpty) && !hasOnlyBitsOfBoxedMarkers(JSValue::ValueFalse) && !hasOnlyBitsOfBoxedMarkers(JSValue::ValueTrue));
static_assert(!hasOnlyBitsOfBoxedMarkers(emptyLiteralStringWord) && bitsOfBoxedMarkers < emptyLiteralStringWord);

static constexpr SlotTraits traitsOfLoaded(SlotRepresentation slot)
{
    return traitsOf(slot == SlotRepresentation::OptionalInt32 ? SlotRepresentation::OptionalInt64 : slot);
}

bool needsStorageFor(const PlacedSlot& slot, const Type& stored)
{
    bool isUndefined = !stored.isNever() && stored.hasOnlyPrimitives(Primitive::Undefined);
    return !(slot.isOutOfLine() && slot.mayExistWithoutStorage && isUndefined);
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
    const SlotTraits& traits = traitsOf(slot);
    Representation representation = Typed::representationOf(slot);
    if (traits.valueKind == SlotValueKind::Float)
        return Number(m_out.floatToDouble(m_out.loadFloat(pointer)));
    if (traits.valueKind == SlotValueKind::Double)
        return Operand(representation, m_out.loadDouble(pointer));

    LValue bits;
    if (traits.valueKind == SlotValueKind::SignedInteger && traits.width < sizeof(int32_t))
        bits = traits.width == 1 ? m_out.load8SignExt32(pointer) : m_out.load16SignExt32(pointer);
    else
        bits = loadBits(pointer, slot);
    if (slot == SlotRepresentation::OptionalInt32)
        return widenOptionalInt32(Word32(bits), AbsentReadsAs::Absent);
    if (slot == SlotRepresentation::LiteralString32) {
        NarrowReference word(bits);
        return LiteralStringWord(m_out.select(isCellReferenceAmongAllWords(word).value(), decode(word).value(), m_out.zeroExtPtr(bits)));
    }
    if (bits->type() == B3::Int32 && b3TypeOf(representation) == B3::Int64)
        bits = m_out.zeroExtPtr(bits);
    return Operand(representation, bits);
}

Operand SlotEmitter::loadWithoutMarkers(TypedPointer pointer, SlotRepresentation slot)
{
    switch (slot) {
    case SlotRepresentation::OptionalInt32:
        return Word32(m_out.load32(pointer));
    case SlotRepresentation::OptionalInt64:
        return Integer(m_out.load64(pointer));
    case SlotRepresentation::OptionalDouble:
        return Number(m_out.loadDouble(pointer));
    case SlotRepresentation::OptionalBoolean:
        static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1) && !(JSValue::ValueFalse & 1));
        return Condition(m_out.bitAnd(m_out.load8ZeroExt32(pointer), m_out.constInt32(1)));
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

OptionalInteger SlotEmitter::widenOptionalInt32(Word32 word, AbsentReadsAs absentReadsAs)
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
    OptionalInteger undefinedOrAbsent = marker(undefinedInteger);
    if (absentReadsAs == AbsentReadsAs::Absent)
        undefinedOrAbsent = select(equal(word, word32(absentInt32)), marker(absentInteger), undefinedOrAbsent);
    join.add(select(equal(word, word32(nullInt32)), marker(nullInteger), undefinedOrAbsent));
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
    const SlotTraits& traits = traitsOf(slot);
    LValue bits = operand.value;
    if (traits.valueKind == SlotValueKind::Float) {
        m_out.storeFloat(m_out.doubleToFloat(bits), pointer);
        return;
    }
    if (traits.valueKind == SlotValueKind::Double) {
        m_out.storeDouble(bits, pointer);
        return;
    }
    if (slot == SlotRepresentation::OptionalInt32)
        bits = narrowOptionalInt32(operand.as<OptionalInteger>()).value();
    else if (slot == SlotRepresentation::LiteralString32) {
        Condition isStringCell = emitter<ConversionEmitter>().isStringCell(operand.as<LiteralStringWord>());
        bits = m_out.select(isStringCell.value(), compress(CellPointer(bits)).value(), m_out.castToInt32(bits));
    } else if (traits.width < sizeof(uint64_t) && bits->type() == B3::Int64)
        bits = m_out.castToInt32(bits);
    storeBits(pointer, slot, bits);
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

Condition SlotEmitter::hasNoExtension(CellPointer cell, const PlacedSlot& extensionWord)
{
    return isAbsent(extensionWordOf(cell, extensionWord), extensionWord.representation);
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

void SlotEmitter::ensureExtensionHolds(const OperationContext& context, CellPointer cell, const PlacedSlot& extensionWord, uint64_t testedBits, uint64_t reservedBits, OwnerBarrier barrier)
{
    RELEASE_ASSERT(testedBits && !(testedBits & ~reservedBits));
    unsigned size = extensionCellSizeFor(std::popcount(reservedBits));
    LBasicBlock done = newBlock();
    LBasicBlock lacksGranule = newBlock();
    branchUnlessExtensionHolds(cell, extensionWord, testedBits, rareEdge(lacksGranule));
    m_out.jump(done);

    m_out.appendTo(lacksGranule);
    if (size <= maxExtensionSizeFromStub) {
        callStub(context, allocateExtension(size), { cell.value(), m_out.constIntPtr(extensionWord.offset), m_out.constInt64(static_cast<int64_t>(reservedBits)) }, { .isOnRarePath = true });
        if (barrier == OwnerBarrier::Emitted)
            emitter<BarrierEmitter>().barrier(context, cell);
    } else
        callVoidOperation(context, RuntimeOperation::EnsureExtensionGranules, { cell, integer(static_cast<int64_t>(reservedBits)) }, { .isOnRarePath = true });
    m_out.jump(done);

    m_out.appendTo(done);
}

// For an operation that declares no collection: neither the stub nor the operation behind it collects.
void SlotEmitter::ensureExtensionHoldsWithoutCollecting(const OperationContext& context, CellPointer cell, const PlacedSlot& extensionWord, uint64_t granuleBits)
{
    LBasicBlock done = newBlock();
    LBasicBlock lacksGranule = newBlock();
    branchUnlessExtensionHolds(cell, extensionWord, granuleBits, rareEdge(lacksGranule));
    m_out.jump(done);

    m_out.appendTo(lacksGranule);
    unsigned size = extensionCellSizeFor(std::popcount(granuleBits));
    if (size <= maxExtensionSizeFromStub) {
        LValue operands[] { cell.value(), m_out.constIntPtr(extensionWord.offset), m_out.constInt64(static_cast<int64_t>(granuleBits)) };
        emitter<RuntimeCallEmitter>().callStubWithoutSite(allocateExtension(size), operands, { .isOnRarePath = true });
        emitter<BarrierEmitter>().barrier(context, cell);
    } else {
        Operand arguments[] { cell, integer(static_cast<int64_t>(granuleBits)) };
        callWithoutSite(RuntimeOperation::EnsureExtensionGranulesWithoutCollecting, arguments, Representation::Void, { .isOnRarePath = true });
    }
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

Operand SlotEmitter::loadSlot(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    if (!slot.isOutOfLine())
        return load(addressOf(cell, slot, location), slot.representation);
    RELEASE_ASSERT(extensionWord);
    Operand loaded = loadOutOfLine(cell, slot, location, *extensionWord);
    // A field that an object lists before it has an extension reads as undefined.
    return traitsOf(slot.representation).hasUndefined ? absentAsUndefined(loaded, slot.representation) : loaded;
}

Operand SlotEmitter::loadSlot(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord, const Type& loadedType)
{
    bool holdsNoMarker = loadedRepresentationOf(slot.representation, loadedType) != Typed::representationOf(slot.representation);
    if (holdsNoMarker && !slot.isOutOfLine())
        return loadWithoutMarkers(addressOf(cell, slot, location), slot.representation);
    return loadSlot(cell, slot, location, extensionWord);
}

// The likely path has one test, which the markers fail.
Operand SlotEmitter::loadSlotOrUndefined(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    SlotRepresentation representation = slot.representation;
    SlotTraits traits = traitsOfLoaded(representation);
    RELEASE_ASSERT(traits.hasUndefined);
    if (slot.isOutOfLine())
        return loadSlot(cell, slot, location, extensionWord);

    TypedPointer pointer = addressOf(cell, slot, location);
    if (representation == SlotRepresentation::OptionalInt32)
        return widenOptionalInt32(Word32(m_out.load32(pointer)), AbsentReadsAs::Undefined);

    Operand loaded = load(pointer, representation);
    if (representsAbsentAsZero(representation))
        return Operand(loaded.representation, m_out.select(isAbsent(loaded, representation).value(), constantWithBits(loaded.value->type(), traits.undefinedBits), loaded.value));

    LBasicBlock isValue = newBlock();
    LBasicBlock mayBeAbsent = newBlock();
    OperandJoin join(m_out, loaded.representation);
    branch(isNoMarker(loaded, representation), { normalEdge(isValue), unlikelyEdge(mayBeAbsent) });

    m_out.appendTo(isValue);
    join.add(loaded);

    m_out.appendTo(mayBeAbsent);
    join.add(absentAsUndefined(loaded, representation));
    return join.finish();
}

Condition SlotEmitter::holdsValue(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    if (!traitsOf(slot.representation).hasAbsent)
        return boolean(true);
    if (!slot.isOutOfLine())
        return logicalNot(holdsAbsent(addressOf(cell, slot, location), slot.representation));
    RELEASE_ASSERT(extensionWord);
    return logicalNot(isAbsent(loadOutOfLine(cell, slot, location, *extensionWord), slot.representation));
}

Condition SlotEmitter::isFilled(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    Condition slotHoldsValue = holdsValue(cell, slot, location, extensionWord);
    if (!slot.isOutOfLine() || !slot.mayExistWithoutStorage)
        return slotHoldsValue;
    // Absent bytes do not say that the field is absent: only the key order says whether the object lists it.
    RELEASE_ASSERT(!location.isEntireHeap());
    return either<Condition>(slotHoldsValue, [&] {
        return boolean(true);
    }, [&] {
        return callWithoutSite<Condition>(RuntimeOperation::HasExtraProperty, { cell, identifierNumber(location.key()) }, { .isOnRarePath = true });
    });
}

// For what load() gives of a slot that has the three markers.
Condition SlotEmitter::isNoMarker(Operand loaded, SlotRepresentation slot)
{
    switch (slot) {
    case SlotRepresentation::OptionalInt64:
        return lessThanOrEqual(Integer(loaded.value), integer(maxSlotInteger));
    case SlotRepresentation::OptionalDouble: {
        // The markers are NaNs. So is NaN, which fails the test with them.
        Number number(loaded.value);
        return equal(number, number);
    }
    case SlotRepresentation::OptionalBoolean:
        return isBoolean(loaded.as<Boxed>());
    case SlotRepresentation::OptionalCell32:
        return above(loaded.as<NarrowReference>(), NarrowReference(m_out.constInt32(undefinedCell32)));
    case SlotRepresentation::JSValue:
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
        return Condition(m_out.testNonZero64(loaded.value, m_out.constInt64(~bitsOfBoxedMarkers)));
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Condition SlotEmitter::holdsDeclaredType(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    // The closure that is not made yet, or a reference: what lies between is absent, null or undefined.
    if (slot.representation == SlotRepresentation::DeferredClosure) {
        Word32 word(m_out.load32(addressOf(cell, slot, location)));
        return logicalOr(equal(word, word32(uncreatedDeferredClosure)), above(word, word32(undefinedCell32)));
    }
    if (!traitsOf(slot.representation).hasUndefined)
        return holdsValue(cell, slot, location, extensionWord);
    if (slot.isOutOfLine()) {
        RELEASE_ASSERT(extensionWord);
        return isNoMarker(loadOutOfLine(cell, slot, location, *extensionWord), slot.representation);
    }
    TypedPointer pointer = addressOf(cell, slot, location);
    if (slot.representation == SlotRepresentation::OptionalInt32)
        return greaterThanOrEqual(Word32(m_out.load32(pointer)), word32(minInlineInt32));
    if (slot.representation == SlotRepresentation::OptionalDouble) {
        // Read as a signed word, the upper half of a marker is larger than that of any number, of an infinity and of NaN.
        static_assert(undefinedDouble < nullDouble && nullDouble < absentDouble && !(absentDouble >> 63));
        static_assert(std::bit_cast<uint64_t>(PNaN) < undefinedDouble && !(undefinedDouble & UINT32_MAX));
        TypedPointer upperHalf = m_out.address(m_heaps.at(m_program, location), cell.value(), slot.offset + sizeof(uint32_t));
        return lessThan(Word32(m_out.load32(upperHalf)), word32(static_cast<int32_t>(undefinedDouble >> 32)));
    }
    return isNoMarker(load(pointer, slot.representation), slot.representation);
}

bool SlotEmitter::holdsRawValueFromBirthInSomeMember(LayoutSetID receivers, PropertyKeyID key) const
{
    return std::ranges::any_of(m_program.at(receivers).members, [&](LayoutID layout) {
        std::optional<FieldReference> field = findField(m_program, layout, key, BaseChain::Proven);
        return field && holdsRawValueFromBirth(m_program, *field);
    });
}

// For a store into the slot of a number or a boolean for whose raw value the mark of the cell answers.
void SlotEmitter::markCellIfMarkerIsStored(CellPointer cell, ValueID stored)
{
    const Type& type = typeOf(stored);
    if (!type.mayBeUndefinedOrNull())
        return;
    auto mark = [&] {
        CodeGenerator::InitializationScope scope(generator());
        Word32 flags = loadByte(cell, m_heaps.JSCell_typeInfoFlags);
        m_out.store32As8(bitOr(flags, word32(TypeInfoPerCellBit)).value(), cell.value(), m_heaps.JSCell_typeInfoFlags);
    };
    if (type.isUndefinedOrNull()) {
        mark();
        return;
    }
    Operand operand = get(stored);
    Condition isMarker = operand.is<Boxed>() ? isUndefinedOrNull(operand.as<Boxed>(), type.primitives) : emitter<ConversionEmitter>().isMarker(operand);
    LBasicBlock storesMarker = newBlock();
    LBasicBlock done = newBlock();
    branch(isMarker, { unlikelyEdge(storesMarker), normalEdge(done) });
    m_out.appendTo(storesMarker);
    mark();
    m_out.jump(done);
    m_out.appendTo(done);
}

void SlotEmitter::storeSlot(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, Operand operand, const PlacedSlot* extensionWord)
{
    store(addressOf(cell, slot, location, extensionWord), slot.representation, operand);
}

void SlotEmitter::storeSlot(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, Operand operand, const PlacedSlot* extensionWord, const Type& storedType)
{
    if (slot.representation == SlotRepresentation::OptionalInt32 && storedType.isInteger()) {
        m_out.store32(m_out.castToInt32(operand.as<OptionalInteger>().value()), addressOf(cell, slot, location, extensionWord));
        return;
    }
    storeSlot(cell, slot, location, operand, extensionWord);
}

void SlotEmitter::storeInExtension(Pointer extension, const PlacedSlot& slot, const MemoryLocation& location, Operand operand)
{
    RELEASE_ASSERT(slot.isOutOfLine());
    store(addressInExtension(m_heaps.at(m_program, location), extension, slot.offset), slot.representation, operand);
}

void SlotEmitter::storeIfExtensionHolds(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, Operand operand, const PlacedSlot& extensionWord)
{
    RELEASE_ASSERT(slot.isOutOfLine());
    LBasicBlock done = newBlock();
    Pointer extension = branchUnlessExtensionHolds(cell, extensionWord, granuleBitOf(slot.offset), normalEdge(done));
    storeInExtension(extension, slot, location, operand);
    m_out.jump(done);

    m_out.appendTo(done);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
