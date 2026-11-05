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
#include "AOTPureOperations.h"
#include "AOTRepresentation.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubCatalog.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTTypeArena.h"
#include <bit>
#include <optional>
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

LValue SlotEmitter::constantWithBits(LType type, uint64_t bits)
{
    if (type == Machine::Int32)
        return m_out.constInt32(static_cast<int32_t>(bits));
    if (type == Machine::Int64)
        return m_out.constInt64(static_cast<int64_t>(bits));
    RELEASE_ASSERT(type == Machine::Double);
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
    return operandOfBits(bits, slot);
}

Operand SlotEmitter::operandOfBits(LValue bits, SlotRepresentation slot)
{
    Representation representation = Typed::representationOf(slot);
    if (slot == SlotRepresentation::OptionalInt32)
        return widenOptionalInt32(Word32(bits), AbsentReadsAs::Absent);
    if (slot == SlotRepresentation::LiteralString32) {
        NarrowReference word(bits);
        Condition isCell = isCellReferenceAmongAllWords(word);
        CellPointer cell = decode(word);
        return LiteralStringWord(m_out.select(isCell.value(), cell.value(), m_out.zeroExtPtr(bits)));
    }
    if (bits->type() == Machine::Int32 && machineTypeOf(representation) == Machine::Int64)
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
        return Condition(m_out.bitAnd(m_out.load8ZeroExt32(pointer), 1));
    case SlotRepresentation::JSValue:
    case SlotRepresentation::Cell:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
    case SlotRepresentation::Int32:
    case SlotRepresentation::Uint32:
    case SlotRepresentation::Int64:
    case SlotRepresentation::Double:
    case SlotRepresentation::Float:
    case SlotRepresentation::Boolean:
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
    case SlotRepresentation::DeferredClosure:
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
    Condition holdsMarker = lessThan(word, word32(minInlineInt32));
    ScopeFork fork = m_out.forkScope();
    branch(holdsMarker, { unlikelyEdge(isMarker), normalEdge(isInteger) });

    m_out.appendToArm(isInteger, fork);
    join.add(OptionalInteger(signExtended(word).value()));

    m_out.appendToArm(isMarker, fork);
    OptionalInteger undefinedOrAbsent = marker(undefinedInteger);
    if (absentReadsAs == AbsentReadsAs::Absent) {
        Condition isAbsent = equal(word, absentInt32);
        undefinedOrAbsent = select(isAbsent, marker(absentInteger), undefinedOrAbsent);
    }
    Condition isNull = equal(word, nullInt32);
    join.add(select(isNull, marker(nullInteger), undefinedOrAbsent));
    return join.finishBehind(fork);
}

Word32 SlotEmitter::narrowOptionalInt32(OptionalInteger value)
{
    if (value.value()->opcode() == Machine::SExt32)
        return Word32(value.value()->child(0));
    if (std::optional<Word32> narrow = narrowWordBehind(value))
        return *narrow;

    Integer bits(value.value());
    auto is = [&](int64_t marker) {
        return equal(bits, Integer(m_out.constInt64(marker)));
    };
    Condition isAbsent = is(absentInteger);
    Word32 absent = word32(absentInt32);
    Word32 marker = select(isAbsent, absent, select<Word32>(is(undefinedInteger), undefinedInt32, nullInt32));
    Condition isMarker = greaterThan(bits, maxSlotInteger);
    return select(isMarker, marker, low32(bits));
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
        NarrowReference reference = compress(CellPointer(bits));
        bits = m_out.select(isStringCell.value(), reference.value(), m_out.castToInt32(bits));
    } else if (traits.width < sizeof(uint64_t) && bits->type() == Machine::Int64)
        bits = m_out.castToInt32(bits);
    storeBits(pointer, slot, bits);
}

void SlotEmitter::storeAbsent(TypedPointer pointer, SlotRepresentation slot)
{
    SlotTraits traits = traitsOf(slot);
    RELEASE_ASSERT(traits.hasAbsent);
    storeBits(pointer, slot, constantWithBits(traits.width == sizeof(uint64_t) ? Machine::Int64 : Machine::Int32, traits.absentBits));
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
    LValue bits = loaded.value->type() == Machine::Double ? emitter<ConversionEmitter>().bitsOf(loaded).value() : loaded.value;
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
    return load(m_out.address(m_heaps.AOTObject_hiddenWords, cell.value(), extensionWord.offset), extensionWord.representation);
}

Pointer SlotEmitter::extensionAt(Operand word)
{
    if (!word.is<NarrowReference>())
        return asPointer(word.as<CellPointer>());
    RELEASE_ASSERT(m_cellCageBase);
    static_assert(FaultSite::reach <= offsetOfEmptyExtensionInCage, "A load through a null narrow reference has to go on faulting.");
    LValue emptyExtension = m_out.add(m_cellCageBase, m_out.constIntPtr(offsetOfEmptyExtensionInCage));
    return Pointer(m_out.add(emptyExtension, m_out.shl(m_out.zeroExtPtr(word.value), cell32Shift)));
}

Pointer SlotEmitter::extensionOf(CellPointer cell, const PlacedSlot& extensionWord)
{
    return extensionAt(extensionWordOf(cell, extensionWord));
}

CellPointer SlotEmitter::extraPropertiesOf(CellPointer cell, const PlacedSlot& extraPropertiesWord, LBasicBlock hasNone)
{
    Operand word = extensionWordOf(cell, extraPropertiesWord);
    LBasicBlock hasThem = newBlock();
    branch(isAbsent(word, extraPropertiesWord.representation), { rareEdge(hasNone), normalEdge(hasThem) });
    m_out.appendToContinuation(hasThem);
    return word.is<NarrowReference>() ? decode(word.as<NarrowReference>()) : word.as<CellPointer>();
}

SlotEmitter::TestedExtension SlotEmitter::branchUnlessExtensionHolds(CellPointer cell, const PlacedSlot& extensionWord, uint64_t granuleBits, BlockEdge lacksGranule, bool asksForExtensionFirst)
{
    std::optional<ScopeFork> fork;
    Operand word = extensionWordOf(cell, extensionWord);
    if (!word.is<NarrowReference>() || asksForExtensionFirst) {
        LBasicBlock hasExtension = newBlock();
        Condition hasNone = isAbsent(word, extensionWord.representation);
        fork = m_out.forkScope();
        branch(hasNone, { lacksGranule, normalEdge(hasExtension) });
        m_out.appendToContinuation(hasExtension);
    }
    Pointer extension = extensionAt(word);
    LBasicBlock holds = newBlock();
    Condition holdsThem = holdsGranules(extension, granuleBits);
    if (!fork)
        fork = m_out.forkScope();
    branch(holdsThem, { normalEdge(holds), lacksGranule });
    m_out.appendToContinuation(holds);
    return { extension, *fork };
}

void SlotEmitter::ensureExtensionHolds(const OperationContext& context, CellPointer cell, const PlacedSlot& extensionWord, uint64_t testedBits, uint64_t reservedBits, OwnerBarrier barrier, bool mayCollect)
{
    RELEASE_ASSERT(testedBits && !(testedBits & ~reservedBits));
    LBasicBlock done = newBlock();
    LBasicBlock lacksGranule = newBlock();
    branchUnlessExtensionHolds(cell, extensionWord, testedBits, rareEdge(lacksGranule));
    m_out.jump(done);

    m_out.appendTo(lacksGranule);
    reserveGranules(context, cell, extensionWord, reservedBits, barrier, mayCollect);
    m_out.jump(done);

    // With an empty scope: the hidden word and the bits of the granules are read again, whatever the call declares that it writes.
    m_out.appendTo(done);
}

void SlotEmitter::reserveGranules(const OperationContext& context, CellPointer cell, const PlacedSlot& extensionWord, uint64_t reservedBits, OwnerBarrier barrier, bool mayCollect)
{
    unsigned size = extensionCellSizeFor(std::popcount(reservedBits));
    CallOptions options { .isOnRarePath = true };
    if (size <= maxExtensionSizeFromStub) {
        LValue operands[] { cell.value(), m_out.constIntPtr(extensionWord.offset), m_out.constInt64(static_cast<int64_t>(reservedBits)) };
        if (mayCollect)
            callStub(context, allocateExtension(size), operands, options);
        else
            emitter<RuntimeCallEmitter>().callStubWithoutSite(allocateExtension(size), operands, options);
        if (barrier == OwnerBarrier::Emitted)
            emitter<BarrierEmitter>().barrier(context, cell);
    } else {
        Operand arguments[] { cell, integer(static_cast<int64_t>(reservedBits)) };
        if (mayCollect)
            callOperation(context, RuntimeOperation::EnsureExtensionGranules, arguments, Representation::Void, options);
        else
            callWithoutSite(RuntimeOperation::EnsureExtensionGranulesWithoutCollecting, arguments, Representation::Void, options);
    }
}

// ARM64 counts bits in a vector register. For a few bits it is quicker to subtract: x - (x >> 1) - (x >> 2) .. is the number of bits set in x, and a shifted operand costs nothing there.
static constexpr uint32_t maxGranuleCountedBySubtraction = isARM64() ? 5 : 1;

// The bit of the granule becomes the sign and the bits of the granules behind it fall out, so one value answers whether the granule is held and how many are held up to it.
Integer SlotEmitter::granuleBitsUpTo(Pointer extension, uint32_t granule)
{
    return Integer(m_out.shl(loadField<Integer>(extension, m_heaps.AOTExtension_granuleBits).value(), maxExtensionGranules - 1 - granule));
}

Condition SlotEmitter::holdsGranules(Pointer extension, uint64_t granuleBits)
{
    if (hasOneBitSet(granuleBits) && static_cast<uint32_t>(std::countr_zero(granuleBits)) > maxGranuleCountedBySubtraction)
        return lessThan(granuleBitsUpTo(extension, std::countr_zero(granuleBits)), 0);
    Integer held = bitAnd(loadField<Integer>(extension, m_heaps.AOTExtension_granuleBits), static_cast<int64_t>(granuleBits));
    return hasOneBitSet(granuleBits) ? isNotZero(held) : equal(held, integer(static_cast<int64_t>(granuleBits)));
}

TypedPointer SlotEmitter::addressInExtension(const Machine::AbstractHeap& heap, Pointer extension, uint32_t offset)
{
    static_assert(extensionGranuleSize == 8 && offsetOfFirstExtensionSlot == extensionGranuleSize);
    uint32_t granule = granuleOf(offset);
    if (!granule)
        return m_out.address(heap, extension.value(), offsetOfFirstExtensionSlot + offsetInGranule(offset));
    if (granule > maxGranuleCountedBySubtraction)
        return m_out.baseIndex(heap, extension.value(), m_out.popCount(granuleBitsUpTo(extension, granule).value()), ScaleEight, offsetInGranule(offset));

    Integer bitsBefore = bitAnd(loadField<Integer>(extension, m_heaps.AOTExtension_granuleBits), static_cast<int64_t>((1ull << granule) - 1));
    Integer count = bitsBefore;
    for (uint32_t shift = 1; shift < granule; ++shift)
        count = sub(count, Integer(m_out.lShr(bitsBefore.value(), m_out.constInt32(shift))));
    return m_out.baseIndex(heap, extension.value(), count.value(), ScaleEight, offsetOfFirstExtensionSlot + offsetInGranule(offset));
}

TypedPointer SlotEmitter::addressOf(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    const Machine::AbstractHeap& heap = m_heaps.at(m_program, location);
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

bool SlotEmitter::readsExtensionByCall(const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot& extensionWord) const
{
    static_assert(formDependsOnLoopOf(Opcode::GetSlot) && formDependsOnLoopOf(Opcode::GetSlotOrUndefined) && formDependsOnLoopOf(Opcode::IsSlotFilled));
    static_assert(formDependsOnLoopOf(Opcode::GetSlotGetter) && formDependsOnLoopOf(Opcode::GetSlotSetter) && formDependsOnLoopOf(Opcode::PutSlotOrAdd));
    BlockID block = m_generator.blockBeingGenerated();
    if (!block)
        return false;
    // What the stub has in its code: where the word of the Extension lies and how wide it is.
    if (extensionWord.offset != offsetOfFirstField || extensionWord.representation != hiddenReferenceRepresentation(inputs().stubCatalog.options().useWideReferences))
        return false;
    if (location.heap() != AbstractHeap::ObjectFields || !representsAbsentAsZero(slot.representation) || !findSlotInGranule(offsetInGranule(slot.offset), widthOf(slot.representation)))
        return false;
    // For PutSlotOrAdd the read is a part of its form.
    static constexpr OpcodeSet readsOfSlots { Opcode::GetSlot, Opcode::GetSlotOrUndefined, Opcode::IsSlotFilled, Opcode::GetSlotGetter, Opcode::GetSlotSetter };
    // `if (message.isMeta)`: the call would cost more than all that the read does where the object has no Extension.
    bool readsInLine = m_generator.paysForCodeInLine(block) || slot.isLeftOutByMakers;
    m_generator.out().noteFormOf(readsOfSlots, readsInLine ? Form::Open : Form::Closed, StateOfKnowledge::FieldInExtension, m_generator.paysForCodeInLine(block) ? Place::Loop : Place::Anywhere);
    return !readsInLine;
}

LValue SlotEmitter::readFieldInExtension(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location)
{
    constexpr unsigned bitsOfOffset = 10;
    constexpr unsigned bitsOfPlace = 4;
    static_assert(maxExtensionSize <= 1u << bitsOfOffset && static_cast<unsigned>(SlotInGranule::ByteAt7) < 1u << bitsOfPlace);
    static_assert(bitsOfOffset + bitsOfPlace + 8 * sizeof(Machine::HeapIndex) <= 32);
    unsigned width = widthOf(slot.representation);
    SlotInGranule place = *findSlotInGranule(offsetInGranule(slot.offset), width);
    // As the number of a load has its offset, its width and its heap.
    uint32_t payload = slot.offset | static_cast<uint32_t>(place) << bitsOfOffset | static_cast<uint32_t>(m_heaps.at(m_program, location).index()) << (bitsOfOffset + bitsOfPlace);
    CallOptions options { .pureFunction = functionOfMemory(PureOperation::ReadFieldInExtension, payload), .convertsNoCells = true, .returnsWord32 = width < sizeof(uint64_t) };
    LValue object = cell.value();
    return emitter<RuntimeCallEmitter>().callStubThatReads(AOT::readFieldInExtension(granuleOf(slot.offset), place), singleElementSpan(object), location, options);
}

Operand SlotEmitter::loadOutOfLine(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot& extensionWord, bool absentReadsAsUndefined)
{
    RELEASE_ASSERT(representsAbsentAsZero(slot.representation));
    // The stub takes the cell, and nothing collects until it has read the slot.
    if (readsExtensionByCall(slot, location, extensionWord)) {
        Operand read = operandOfBits(readFieldInExtension(cell, slot, location), slot.representation);
        return absentReadsAsUndefined ? absentAsUndefined(read, slot.representation) : read;
    }
    Representation representation = Typed::representationOf(slot.representation);
    LBasicBlock hasNoStorage = newBlock();
    OperandJoin join(m_out, representation);
    auto [extension, fork] = branchUnlessExtensionHolds(cell, extensionWord, granuleBitOf(slot.offset), normalEdge(hasNoStorage), slot.isLeftOutByMakers);
    Operand loaded = load(addressInExtension(m_heaps.at(m_program, location), extension, slot.offset), slot.representation);
    keepOwnerOfExtensionAlive(cell);
    join.add(absentReadsAsUndefined ? absentAsUndefined(loaded, slot.representation) : loaded);
    m_out.appendToJoin(hasNoStorage, fork);
    join.add(Operand(representation, constantWithBits(machineTypeOf(representation), absentReadsAsUndefined ? traitsOfLoaded(slot.representation).undefinedBits : traitsOf(slot.representation).absentBits)));
    return join.finishBehind(fork);
}

Operand SlotEmitter::loadSlot(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    if (!slot.isOutOfLine())
        return load(addressOf(cell, slot, location), slot.representation);
    RELEASE_ASSERT(extensionWord);
    // A field that an object lists before it has an extension reads as undefined.
    return loadOutOfLine(cell, slot, location, *extensionWord, traitsOf(slot.representation).hasUndefined);
}

Operand SlotEmitter::loadSlotThatHoldsValue(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    if (!slot.isOutOfLine())
        return load(addressOf(cell, slot, location), slot.representation);
    RELEASE_ASSERT(extensionWord);
    // The call is the one that holdsValue() has made, unless something has written there since.
    if (readsExtensionByCall(slot, location, *extensionWord))
        return operandOfBits(readFieldInExtension(cell, slot, location), slot.representation);
    Operand loaded = load(addressOf(cell, slot, location, extensionWord), slot.representation);
    keepOwnerOfExtensionAlive(cell);
    return loaded;
}

Operand SlotEmitter::loadSlot(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord, const Type& loadedType)
{
    bool holdsNoMarker = loadedRepresentationOf(slot.representation, loadedType) != Typed::representationOf(slot.representation);
    if (holdsNoMarker && !slot.isOutOfLine())
        return loadWithoutMarkers(addressOf(cell, slot, location), slot.representation);
    // The slot is filled. Only a field that an object may list without storage is filled while its granule is missing or empty: isFilled().
    if (slot.isOutOfLine() && !slot.mayExistWithoutStorage) {
        RELEASE_ASSERT(extensionWord);
        return loadOutOfLine(cell, slot, location, *extensionWord);
    }
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
    if (representsAbsentAsZero(representation)) {
        Condition holdsAbsent = isAbsent(loaded, representation);
        return Operand(loaded.representation, m_out.select(holdsAbsent.value(), constantWithBits(loaded.value->type(), traits.undefinedBits), loaded.value));
    }

    LBasicBlock isValue = newBlock();
    LBasicBlock mayBeAbsent = newBlock();
    OperandJoin join(m_out, loaded.representation);
    Condition holdsNoMarker = isNoMarker(loaded, representation);
    ScopeFork fork = m_out.forkScope();
    branch(holdsNoMarker, { normalEdge(isValue), unlikelyEdge(mayBeAbsent) });

    m_out.appendToArm(isValue, fork);
    join.add(loaded);

    m_out.appendToArm(mayBeAbsent, fork);
    join.add(absentAsUndefined(loaded, representation));
    return join.finishBehind(fork);
}

Condition SlotEmitter::holdsValue(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    if (!traitsOf(slot.representation).hasAbsent)
        return boolean(true);
    if (!slot.isOutOfLine())
        return logicalNot(holdsAbsent(addressOf(cell, slot, location), slot.representation));
    RELEASE_ASSERT(extensionWord);
    if (readsExtensionByCall(slot, location, *extensionWord)) {
        LValue bits = readFieldInExtension(cell, slot, location);
        return Condition(m_out.notEqual(bits, constantWithBits(bits->type(), 0)));
    }
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
    case SlotRepresentation::Cell:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::Int32:
    case SlotRepresentation::Uint32:
    case SlotRepresentation::Int64:
    case SlotRepresentation::OptionalInt32:
    case SlotRepresentation::Double:
    case SlotRepresentation::Float:
    case SlotRepresentation::Boolean:
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::DeferredClosure:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Condition SlotEmitter::holdsDeclaredType(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    // The closure that is not made yet, or a reference: what lies between is absent, null or undefined.
    if (slot.representation == SlotRepresentation::DeferredClosure) {
        Word32 word(m_out.load32(addressOf(cell, slot, location)));
        Condition isUncreated = equal(word, uncreatedDeferredClosure);
        return logicalOr(isUncreated, above(word, undefinedCell32));
    }
    if (!traitsOf(slot.representation).hasUndefined)
        return holdsValue(cell, slot, location, extensionWord);
    if (slot.isOutOfLine()) {
        RELEASE_ASSERT(extensionWord);
        return isNoMarker(loadOutOfLine(cell, slot, location, *extensionWord), slot.representation);
    }
    TypedPointer pointer = addressOf(cell, slot, location);
    if (slot.representation == SlotRepresentation::OptionalInt32)
        return greaterThanOrEqual(Word32(m_out.load32(pointer)), minInlineInt32);
    if (slot.representation == SlotRepresentation::OptionalDouble) {
        // Read as a signed word, the upper half of a marker is larger than that of any number, of an infinity and of NaN.
        static_assert(undefinedDouble < nullDouble && nullDouble < absentDouble && !(absentDouble >> 63));
        static_assert(std::bit_cast<uint64_t>(PNaN) < undefinedDouble && !(undefinedDouble & UINT32_MAX));
        TypedPointer upperHalf = m_out.address(m_heaps.at(m_program, location), cell.value(), slot.offset + sizeof(uint32_t));
        return lessThan(Word32(m_out.load32(upperHalf)), static_cast<int32_t>(undefinedDouble >> 32));
    }
    return isNoMarker(load(pointer, slot.representation), slot.representation);
}

bool SlotEmitter::holdsRawValueFromBirthInSomeMember(LayoutSetID receivers, PropertyKeyID key) const
{
    return m_generator.inputs().types.at(receivers).anyOf([&](LayoutID layout) {
        std::optional<FieldIndex> field = m_plan.slots().findField(m_program, layout, key);
        return field && m_plan.layout(layout).holdsRawValueFromBirth(*field);
    });
}

// For a store into the slot of a number or a boolean for whose raw value the mark of the cell answers.
void SlotEmitter::markCellIfMarkerIsStored(CellPointer cell, ValueID stored, LayoutSetID possible)
{
    const Type& type = typeOf(stored);
    if (!type.mayBeUndefinedOrNull())
        return;
    auto mark = [&] {
        ShapeTestEmitter& shapes = emitter<ShapeTestEmitter>();
        // An object of any other layout never bears the mark: whoever asks about such layouts takes the word as it lies.
        Vector<LayoutID, 8> bearers;
        if (possible) {
            for (LayoutID member : membersOf(m_generator.inputs().types, possible)) {
                if (m_plan.layout(member).mayBearMark)
                    bearers.append(member);
            }
        }
        if (!possible || bearers.size() == membersOf(m_generator.inputs().types, possible).size()) {
            shapes.setMark(cell);
            return;
        }
        LBasicBlock mayBearIt = newBlock();
        LBasicBlock behind = newBlock();
        ScopeFork fork = m_out.forkScope();
        shapes.branchOnShapeIn(cell, types().layoutSets().ofSorted(bearers.span()), types().at(types().forLayoutSet(possible)), { normalEdge(mayBearIt), normalEdge(behind) });
        m_out.appendToArm(mayBearIt, fork);
        shapes.setMark(cell);
        m_out.jump(behind);
        m_out.appendToJoin(behind, fork);
    };
    if (type.isUndefinedOrNull()) {
        mark();
        return;
    }
    Operand operand = get(stored);
    Condition isMarker = operand.is<Boxed>() ? isUndefinedOrNull(operand.as<Boxed>(), type.primitives) : emitter<ConversionEmitter>().isMarker(operand);
    LBasicBlock storesMarker = newBlock();
    LBasicBlock done = newBlock();
    ScopeFork fork = m_out.forkScope();
    branch(isMarker, { unlikelyEdge(storesMarker), normalEdge(done) });
    m_out.appendToArm(storesMarker, fork);
    mark();
    m_out.jump(done);
    m_out.appendToJoin(done, fork);
}

void SlotEmitter::storeSlot(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, Operand operand, const PlacedSlot* extensionWord)
{
    store(addressOf(cell, slot, location, extensionWord), slot.representation, operand);
    if (slot.isOutOfLine())
        keepOwnerOfExtensionAlive(cell);
}

void SlotEmitter::store(TypedPointer pointer, SlotRepresentation slot, Operand operand, const Type& storedType)
{
    if (slot == SlotRepresentation::OptionalInt32 && storedType.isInteger()) {
        m_out.store32(m_out.castToInt32(operand.as<OptionalInteger>().value()), pointer);
        return;
    }
    store(pointer, slot, operand);
}

void SlotEmitter::storeSlot(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, Operand operand, const PlacedSlot* extensionWord, const Type& storedType)
{
    store(addressOf(cell, slot, location, extensionWord), slot.representation, operand, storedType);
    if (slot.isOutOfLine())
        keepOwnerOfExtensionAlive(cell);
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
    auto [extension, fork] = branchUnlessExtensionHolds(cell, extensionWord, granuleBitOf(slot.offset), normalEdge(done));
    storeInExtension(extension, slot, location, operand);
    keepOwnerOfExtensionAlive(cell);
    m_out.jump(done);

    m_out.appendToJoin(done, fork);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
