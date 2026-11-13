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
#include "AOTInliningAcrossFiles.h"
#include "AOTPureOperations.h"
#include "AOTRepresentation.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTShapeTestEmitter.h"
#include "AOTStubCatalog.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeArena.h"
#include <bit>
#include <optional>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(minExtensionSize == copyChunkBytes && minExtensionSize <= maxExtensionSizeFromStub);
static_assert(JSValue::ValueTrue <= UINT8_MAX && JSValue::ValueUndefined <= UINT8_MAX);
static_assert(absentInt32 < minInlineInt32 && undefinedInt32 < minInlineInt32 && nullInt32 < minInlineInt32);
static_assert(maxSlotInteger < undefinedInteger && maxSlotInteger < nullInteger && maxSlotInteger < absentInteger);
static_assert(absentCell32 < undefinedCell32 && nullCell32 < undefinedCell32 && undefinedCell32 < minCell32);

static constexpr int64_t bitsOfBoxedMarkers = JSValue::ValueUndefined | JSValue::ValueNull;
static constexpr bool hasOnlyBitsOfBoxedMarkers(int64_t bits) { return !(bits & ~bitsOfBoxedMarkers); }
static_assert(hasOnlyBitsOfBoxedMarkers(JSValue::ValueEmpty) && !hasOnlyBitsOfBoxedMarkers(JSValue::ValueFalse) && !hasOnlyBitsOfBoxedMarkers(JSValue::ValueTrue));

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

ALWAYS_INLINE_INTO_OTHER_FILES LValue SlotEmitter::loadBits(TypedPointer pointer, SlotRepresentation slot)
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
    if (traits.valueKind == SlotValueKind::Float)
        return Number(m_out.floatToDouble(m_out.loadFloat(pointer)));
    if (traits.valueKind == SlotValueKind::Double)
        return Operand(Typed::representationOf(slot), m_out.loadDouble(pointer));

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
    OptionalInteger widened = join.finishBehind(fork);
    if (absentReadsAs == AbsentReadsAs::Absent)
        noteNarrowWord(widened, word);
    return widened;
}

bool SlotEmitter::isNarrowIntegerOrOneMarker(const Type& type)
{
    static constexpr Range narrowIntegers { minInlineInt32, std::numeric_limits<int32_t>::max() };
    uint16_t markers = type.primitives & Primitive::UndefinedOrNull;
    return (markers == Primitive::Undefined || markers == Primitive::Null) && type.primitives & Primitive::Integer && type.hasOnlyPrimitives(Primitive::Integer | markers) && type.integers.isSubsetOf(narrowIntegers);
}

OptionalInteger SlotEmitter::widenBySelect(Word32 word, const Type& type)
{
    ASSERT(isNarrowIntegerOrOneMarker(type));
    OptionalInteger marker(m_out.constInt64(type.primitives & Primitive::Null ? nullInteger : undefinedInteger));
    OptionalInteger widened = select(lessThan(word, word32(minInlineInt32)), marker, OptionalInteger(signExtended(word).value()));
    if (widened.value()->opcode() == Machine::Select)
        noteNarrowWord(widened, word);
    return widened;
}

Word32 SlotEmitter::narrowOptionalInt32(OptionalInteger value, uint16_t primitives)
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
    constexpr uint16_t otherMarkers = Primitive::Absent | Primitive::Unspecified;
    bool isOneMarker = !(primitives & otherMarkers) && (primitives & Primitive::UndefinedOrNull) != Primitive::UndefinedOrNull;
    Word32 marker = isOneMarker ? word32(primitives & Primitive::Undefined ? undefinedInt32 : nullInt32) : select(isAbsent, absent, select<Word32>(is(undefinedInteger), undefinedInt32, nullInt32));
    Condition isMarker = greaterThan(bits, maxSlotInteger);
    return select(isMarker, marker, emitter<ConversionEmitter>().lowHalfOfOptionalInteger(value));
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
        bits = narrowOptionalInt32(operand.as<OptionalInteger>(), Primitive::UndefinedOrNull | Primitive::Absent).value();
    else if (traits.width < sizeof(uint64_t) && bits->type() == Machine::Int64)
        bits = m_out.castToInt32(bits);
    storeBits(pointer, slot, bits);
}

void SlotEmitter::storeAbsent(TypedPointer pointer, SlotRepresentation slot)
{
    RELEASE_ASSERT(mayBeCreatedByStore(slot));
    storeBits(pointer, slot, constantWithBits(widthOf(slot) == sizeof(uint64_t) ? Machine::Int64 : Machine::Int32, bitsBeforeCreation(slot)));
}

Condition SlotEmitter::holdsAbsent(TypedPointer pointer, SlotRepresentation slot)
{
    RELEASE_ASSERT(mayBeCreatedByStore(slot));
    LValue bits = loadBits(pointer, slot);
    return Condition(m_out.equal(bits, constantWithBits(bits->type(), bitsBeforeCreation(slot))));
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

static constexpr uint32_t maxGranuleCountedBySubtraction = isARM64() ? 5 : 1;

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

static constexpr bool namesFormsOfGetSlotForFieldsOutOfLine(Opcode read)
{
    for (StateOfKnowledge state : { StateOfKnowledge::FieldInExtension, StateOfKnowledge::FieldLeftOutByMakers }) {
        for (bool isInLoop : { false, true }) {
            FormAtPlace named = formByLoopAlone(read, state, isInLoop);
            FormAtPlace namedForGetSlot = formByLoopAlone(Opcode::GetSlot, state, isInLoop);
            if (named.form != namedForGetSlot.form || named.place != namedForGetSlot.place)
                return false;
        }
        if (namedFormOf(read, state, Place::RareBlock) != namedFormOf(Opcode::GetSlot, state, Place::RareBlock))
            return false;
    }
    return true;
}

bool SlotEmitter::readsExtensionByCall(const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot& extensionWord) const
{
    static_assert(formDependsOnLoopOf(Opcode::GetSlot) && formDependsOnLoopOf(Opcode::GetSlotOrUndefined) && formDependsOnLoopOf(Opcode::IsSlotFilled));
    static_assert(formDependsOnLoopOf(Opcode::GetSlotGetter) && formDependsOnLoopOf(Opcode::GetSlotSetter) && formDependsOnLoopOf(Opcode::PutSlotOrAdd));
    static_assert(namesFormsOfGetSlotForFieldsOutOfLine(Opcode::GetSlotOrUndefined) && namesFormsOfGetSlotForFieldsOutOfLine(Opcode::IsSlotFilled));
    static_assert(namesFormsOfGetSlotForFieldsOutOfLine(Opcode::GetSlotGetter) && namesFormsOfGetSlotForFieldsOutOfLine(Opcode::GetSlotSetter));
    BlockID block = m_generator.blockBeingGenerated();
    if (!block)
        return false;
    if (extensionWord.offset != offsetOfFirstField || extensionWord.representation != hiddenReferenceRepresentation(inputs().stubCatalog.options().useWideReferences))
        return false;
    if (location.heap() != AbstractHeap::ObjectFields || !representsAbsentAsZero(slot.representation) || !findSlotInGranule(offsetInGranule(slot.offset), widthOf(slot.representation)))
        return false;
    static constexpr OpcodeSet readsOfSlots { Opcode::GetSlot, Opcode::GetSlotOrUndefined, Opcode::IsSlotFilled, Opcode::GetSlotGetter, Opcode::GetSlotSetter };
    StateOfKnowledge state = slot.isLeftOutByMakers ? StateOfKnowledge::FieldLeftOutByMakers : StateOfKnowledge::FieldInExtension;
    FormAtPlace named = formByLoopAlone(Opcode::GetSlot, state, m_generator.paysForCodeInLine(block));
    static_assert(namedFormOf(Opcode::GetSlot, StateOfKnowledge::FieldInExtension, Place::Anywhere) == Form::Open && namedFormOf(Opcode::GetSlot, StateOfKnowledge::FieldInExtension, Place::RareBlock) == Form::Closed);
    if (named.place == Place::Anywhere && state == StateOfKnowledge::FieldInExtension && m_generator.code().at(block).frequency == BlockFrequency::Rare)
        named = { Form::Closed, Place::RareBlock };
    m_generator.out().noteFormOf(readsOfSlots, named.form, state, named.place);
    return named.form != Form::Open;
}

LValue SlotEmitter::readFieldInExtension(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location)
{
    constexpr unsigned bitsOfOffset = 10;
    constexpr unsigned bitsOfPlace = 4;
    static_assert(maxExtensionSize <= 1u << bitsOfOffset && static_cast<unsigned>(SlotInGranule::ByteAt7) < 1u << bitsOfPlace);
    static_assert(bitsOfOffset + bitsOfPlace + 8 * sizeof(Machine::HeapIndex) <= 32);
    unsigned width = widthOf(slot.representation);
    SlotInGranule place = *findSlotInGranule(offsetInGranule(slot.offset), width);
    uint32_t payload = slot.offset | static_cast<uint32_t>(place) << bitsOfOffset | static_cast<uint32_t>(m_heaps.at(m_program, location).index()) << (bitsOfOffset + bitsOfPlace);
    CallOptions options { .pureFunction = functionOfMemory(PureOperation::ReadFieldInExtension, payload), .convertsNoCells = true, .returnsWord32 = width < sizeof(uint64_t) };
    LValue object = cell.value();
    return emitter<RuntimeCallEmitter>().callStubThatReads(AOT::readFieldInExtension(granuleOf(slot.offset), place), singleElementSpan(object), location, options);
}

Operand SlotEmitter::loadOutOfLine(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot& extensionWord, bool absentReadsAsUndefined)
{
    RELEASE_ASSERT(representsAbsentAsZero(slot.representation));
    Operand word = readsExtensionByCall(slot, location, extensionWord) ? operandOfBits(readFieldInExtension(cell, slot, location), slot.representation) : wordOutOfLine(cell, slot, location, extensionWord);
    if (!absentReadsAsUndefined)
        return word;
    Operand read = absentAsUndefined(word, slot.representation);
    if (word.is<Boxed>() && read.value != word.value)
        m_wordsOfSlots.set(read.value, word.value);
    return read;
}

Machine::PureFunction SlotEmitter::readInLineOf(const FieldInExtension& field)
{
    static_assert(maxExtensionSize <= 1u << 10 && 10 + 8 * sizeof(Machine::HeapIndex) <= 32);
    return functionOfMemory(PureOperation::ReadFieldInExtensionInLine, field.slot.offset | static_cast<uint32_t>(m_heaps.at(m_program, field.location).index()) << 10);
}

LValue SlotEmitter::findFieldInExtension(CellPointer cell, const FieldInExtension& field)
{
    LValue owner = cell.value();
    return m_out.findPurePatch(readInLineOf(field), machineTypeOf(Typed::representationOf(field.slot.representation)), singleElementSpan(owner));
}

Operand SlotEmitter::loadFromExtension(CellPointer cell, const FieldInExtension& field, const PlacedSlot& extensionWord)
{
    const PlacedSlot& slot = field.slot;
    Representation representation = Typed::representationOf(slot.representation);
    LBasicBlock hasNoStorage = newBlock();
    OperandJoin join(m_out, representation);
    auto [extension, fork] = branchUnlessExtensionHolds(cell, extensionWord, granuleBitOf(slot.offset), normalEdge(hasNoStorage), slot.isLeftOutByMakers);
    Operand loaded = load(addressInExtension(m_heaps.at(m_program, field.location), extension, slot.offset), slot.representation);
    keepOwnerOfExtensionAlive(cell);
    join.add(loaded);
    m_out.appendToJoin(hasNoStorage, fork);
    join.add(Operand(representation, constantWithBits(machineTypeOf(representation), traitsOf(slot.representation).absentBits)));
    return join.finishBehind(fork);
}

Operand SlotEmitter::wordOutOfLine(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot& extensionWord)
{
    Representation representation = Typed::representationOf(slot.representation);
    FieldInExtension field { slot, location };
    if (LValue known = findFieldInExtension(cell, field)) {
        const OperationContext* context = generator().contextBeingGenerated();
        if (emittersCheckProofs() && context)
            emitter<ThrowEmitter>().requireOrCrash(*context, Condition(m_out.equal(known, loadFromExtension(cell, field, extensionWord).value)), std::nullopt, ProofSource::OtherOperation);
        return Operand(representation, known);
    }

    Operand word = loadFromExtension(cell, field, extensionWord);
    if (word.value->opcode() == Machine::Phi)
        m_out.recordInPlaceOfReadOfMemory(readInLineOf(field), word.value, cell.value(), m_heaps.at(m_program, location));
    return word;
}

bool SlotEmitter::readsExtensionInLine(const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot& extensionWord) const
{
    return slot.isOutOfLine() && representsAbsentAsZero(slot.representation) && !readsExtensionByCall(slot, location, extensionWord);
}

void SlotEmitter::loadFieldsInExtension(CellPointer cell, const PlacedSlot& extensionWord, std::span<const FieldInExtension> fields)
{
    uint32_t lastGranule = granuleOf(std::ranges::max_element(fields, { }, [](const FieldInExtension& field) { return field.slot.offset; })->slot.offset);
    uint64_t granulesUpToLast = ~0ull >> (63 - lastGranule);
    bool asksForExtensionFirst = std::ranges::any_of(fields, [](const FieldInExtension& field) { return field.slot.isLeftOutByMakers; });
    LBasicBlock lacksSome = newBlock();
    LBasicBlock done = newBlock();
    Vector<Vector<ValueFromBlock, 2>, 8> words(fields.size());
    Vector<LValue, 8> loaded;
    auto leaveArm = [&] {
        for (size_t place = 0; place < fields.size(); ++place)
            words[place].append(m_out.anchor(loaded[place]));
        m_out.jump(done);
        loaded.shrink(0);
    };
    auto [extension, fork] = branchUnlessExtensionHolds(cell, extensionWord, granulesUpToLast, normalEdge(lacksSome), asksForExtensionFirst);
    for (const FieldInExtension& field : fields)
        loaded.append(load(m_out.address(m_heaps.at(m_program, field.location), extension.value(), field.slot.offset), field.slot.representation).value);
    keepOwnerOfExtensionAlive(cell);
    leaveArm();

    m_out.appendToJoin(lacksSome, fork);
    for (const FieldInExtension& field : fields)
        loaded.append(loadFromExtension(cell, field, extensionWord).value);
    leaveArm();

    m_out.appendToJoin(done, fork);
    for (size_t place = 0; place < fields.size(); ++place) {
        const FieldInExtension& field = fields[place];
        LValue word = m_out.phi(machineTypeOf(Typed::representationOf(field.slot.representation)), words[place]);
        if (word->opcode() == Machine::Phi)
            m_out.recordInPlaceOfReadOfMemory(readInLineOf(field), word, cell.value(), m_heaps.at(m_program, field.location));
    }
}

Operand SlotEmitter::loadSlot(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    if (!slot.isOutOfLine())
        return load(addressOf(cell, slot, location), slot.representation);
    RELEASE_ASSERT(extensionWord);
    return loadOutOfLine(cell, slot, location, *extensionWord, traitsOf(slot.representation).hasUndefined);
}

Operand SlotEmitter::loadSlotThatHoldsValue(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    if (!slot.isOutOfLine())
        return load(addressOf(cell, slot, location), slot.representation);
    RELEASE_ASSERT(extensionWord);
    if (readsExtensionByCall(slot, location, *extensionWord))
        return operandOfBits(readFieldInExtension(cell, slot, location), slot.representation);
    Operand loaded = load(addressOf(cell, slot, location, extensionWord), slot.representation);
    keepOwnerOfExtensionAlive(cell);
    return loaded;
}

Operand SlotEmitter::loadSlotOrLeaveIfEmpty(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord, BlockEdge isEmpty)
{
    RELEASE_ASSERT(!slot.isOutOfLine() || extensionWord);
    LBasicBlock holds = newBlock();
    if (!slot.isOutOfLine() || !traitsOf(slot.representation).hasAbsent || readsExtensionByCall(slot, location, *extensionWord)) {
        branch(holdsValue(cell, slot, location, extensionWord), { normalEdge(holds), isEmpty });
        m_out.appendToContinuation(holds);
        return loadSlotThatHoldsValue(cell, slot, location, extensionWord);
    }
    RELEASE_ASSERT(representsAbsentAsZero(slot.representation));
    Pointer extension = branchUnlessExtensionHolds(cell, *extensionWord, granuleBitOf(slot.offset), isEmpty, slot.isLeftOutByMakers).extension;
    Operand loaded = load(addressInExtension(m_heaps.at(m_program, location), extension, slot.offset), slot.representation);
    keepOwnerOfExtensionAlive(cell);
    branch(isAbsent(loaded, slot.representation), { isEmpty, normalEdge(holds) });
    m_out.appendToContinuation(holds);
    return loaded;
}

Operand SlotEmitter::loadSlot(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord, const Type& loadedType)
{
    bool holdsNoMarker = loadedRepresentationOf(slot.representation, loadedType) != Typed::representationOf(slot.representation);
    if (holdsNoMarker && !slot.isOutOfLine())
        return loadWithoutMarkers(addressOf(cell, slot, location), slot.representation);
    if (slot.representation == SlotRepresentation::OptionalInt32 && !slot.isOutOfLine() && !emittersCheckProofs() && isNarrowIntegerOrOneMarker(loadedType))
        return widenBySelect(Word32(m_out.load32(addressOf(cell, slot, location))), loadedType);
    if (slot.isOutOfLine() && (!slot.mayExistWithoutStorage || !(loadedType.primitives & Primitive::Undefined))) {
        RELEASE_ASSERT(extensionWord);
        return loadOutOfLine(cell, slot, location, *extensionWord);
    }
    return loadSlot(cell, slot, location, extensionWord);
}

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
        LValue loadedOrUndefined = m_out.select(holdsAbsent.value(), constantWithBits(loaded.value->type(), traits.undefinedBits), loaded.value);
        if (loaded.is<Boxed>() && loadedOrUndefined != loaded.value)
            m_wordsOfSlots.set(loadedOrUndefined, loaded.value);
        return Operand(loaded.representation, loadedOrUndefined);
    }

    LBasicBlock isValue = newBlock();
    LBasicBlock mayBeAbsent = newBlock();
    OperandJoin join(m_out, loaded.representation);
    LValue number = representation == SlotRepresentation::OptionalDouble ? m_out.phiIn(join.continuation(), Machine::Double) : nullptr;
    Condition holdsNoMarker = isNoMarker(loaded, representation);
    ScopeFork fork = m_out.forkScope();
    branch(holdsNoMarker, { normalEdge(isValue), unlikelyEdge(mayBeAbsent) });

    m_out.appendToArm(isValue, fork);
    if (number)
        m_out.upsilon(loaded.value, number);
    join.add(loaded);

    m_out.appendToArm(mayBeAbsent, fork);
    Operand optional = absentAsUndefined(loaded, representation);
    if (number)
        m_out.upsilon(emitter<ConversionEmitter>().quietNaN(Number(optional.value)).value(), number);
    join.add(optional);
    Operand read = join.finishBehind(fork);
    if (number)
        emitter<ConversionEmitter>().noteNumberWithoutPayload(read, Number(number));
    return read;
}

Condition SlotEmitter::holdsValue(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, const PlacedSlot* extensionWord)
{
    if (!mayBeCreatedByStore(slot.representation))
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
    if (!isListedOnlyByKeyOrder(slot.representation) && (!slot.isOutOfLine() || !slot.mayExistWithoutStorage))
        return slotHoldsValue;
    RELEASE_ASSERT(!location.isEntireHeap());
    return either<Condition>(slotHoldsValue, [&] {
        return boolean(true);
    }, [&] {
        return callWithoutSite<Condition>(RuntimeOperation::HasExtraProperty, { cell, identifierNumber(location.key()) }, { .isOnRarePath = true });
    });
}

Condition SlotEmitter::isNoMarker(Operand loaded, SlotRepresentation slot)
{
    switch (slot) {
    case SlotRepresentation::OptionalInt64:
        return lessThanOrEqual(Integer(loaded.value), integer(maxSlotInteger));
    case SlotRepresentation::OptionalDouble: {
        Number number(loaded.value);
        return equal(number, number);
    }
    case SlotRepresentation::OptionalBoolean:
        return isBoolean(loaded.as<Boxed>());
    case SlotRepresentation::OptionalCell32:
        return above(loaded.as<NarrowReference>(), NarrowReference(m_out.constInt32(undefinedCell32)));
    case SlotRepresentation::JSValue:
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
    if (slot.representation == SlotRepresentation::DeferredClosure) {
        Word32 word(m_out.load32(addressOf(cell, slot, location)));
        Condition isUncreated = equal(word, uncreatedDeferredClosure);
        return logicalOr(isUncreated, above(word, undefinedCell32));
    }
    if (isListedOnlyByKeyOrder(slot.representation))
        return boolean(true);
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

void SlotEmitter::markCellIfMarkerIsStored(CellPointer cell, ValueID stored, LayoutSetID possible)
{
    const Type& type = typeOf(stored);
    if (!type.mayBeUndefinedOrNull())
        return;
    auto mark = [&] {
        ShapeTestEmitter& shapes = emitter<ShapeTestEmitter>();
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
    auto conditionOfMarker = [&] {
        if (operand.is<Boxed>())
            return isUndefinedOrNull(operand.as<Boxed>(), type.primitives);
        return operand.is<NarrowReference>() ? logicalNot(isCellReference(operand.as<NarrowReference>())) : emitter<ConversionEmitter>().isMarker(operand);
    };
    Condition isMarker = conditionOfMarker();
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
    if (storeLooksAtType(slot) && storedType.isInteger()) {
        m_out.store32(emitter<ConversionEmitter>().lowHalfOfIntegerToldInsideInt32(Integer(operand.as<OptionalInteger>().value())).value(), pointer);
        return;
    }
    if (storeLooksAtType(slot) && operand.is<OptionalInteger>()) {
        m_out.store32(narrowOptionalInt32(operand.as<OptionalInteger>(), storedType.primitives).value(), pointer);
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

ScopeFork SlotEmitter::storeIntoFilledSlot(CellPointer cell, const PlacedSlot& slot, const MemoryLocation& location, Operand operand, const Type& storedType, const PlacedSlot& extensionWord, BlockEdge lacksGranule, BlockEdge isEmpty)
{
    RELEASE_ASSERT(slot.isOutOfLine());
    auto [extension, fork] = branchUnlessExtensionHolds(cell, extensionWord, granuleBitOf(slot.offset), lacksGranule);
    TypedPointer address = addressInExtension(m_heaps.at(m_program, location), extension, slot.offset);
    LBasicBlock isFilled = newBlock();
    branch(holdsAbsent(address, slot.representation), { isEmpty, normalEdge(isFilled) });
    m_out.appendToContinuation(isFilled);
    store(address, slot.representation, operand, storedType);
    keepOwnerOfExtensionAlive(cell);
    return fork;
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
