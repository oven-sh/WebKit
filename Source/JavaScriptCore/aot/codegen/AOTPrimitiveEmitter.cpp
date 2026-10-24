/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPrimitiveEmitter.h"

#if ENABLE(AOT)

#include "AOTStubRegisters.h"
#include "AirArg.h"
#include "B3StackmapGenerationParams.h"
#include <algorithm>
#include <limits>

namespace JSC { namespace AOT { namespace Typed {

// The back end is told that the type of a cell never changes (AOTHeaps.cpp). These do, in place: in row order ClassPrototype::convertToOrdinaryObject, SlimClosure::reify.
// So a type is tested under that heap only by a range that holds both types of a row or neither. --aot-check=ClaimsToBackEnd | MovedLoads report every other change.
static constexpr B3::WordChange changesOfCellTypeInPlace[] = {
    { ObjectType, FinalObjectType },
    { SlimClosureType, SlimClosureWithPropertiesType },
};

static constexpr bool isStableRange(JSTypeRange range)
{
    auto contains = [&](JSType type) {
        return range.first <= type && type <= range.last;
    };
    for (B3::WordChange change : changesOfCellTypeInPlace) {
        if (contains(static_cast<JSType>(change.before)) != contains(static_cast<JSType>(change.after)))
            return false;
    }
    return true;
}

static constexpr bool isTakenInPlace(JSType type)
{
    for (B3::WordChange change : changesOfCellTypeInPlace) {
        if (change.after == type)
            return true;
    }
    return false;
}

static_assert(isStableRange({ StringType, StringType }) && isStableRange({ ObjectType, MaxJSType }) && isStableRange(slimClosureTypes) && isStableRange({ ArrayType, DerivedArrayType }) && isStableRange({ CompiledClosureType, CompiledClosureType }));
static_assert(isStableRange({ CompiledObjectType, CompiledObjectType }) && isStableRange({ ObjectType, FinalObjectType }));
static_assert(!isStableRange({ ObjectType, ObjectType }) && !isStableRange({ FinalObjectType, FinalObjectType }));
static_assert(!isStableRange({ SlimClosureType, SlimClosureType }) && !isStableRange({ SlimClosureWithPropertiesType, SlimClosureWithPropertiesType }));

bool isStableCellTypeRange(JSTypeRange range)
{
    return isStableRange(range);
}

std::span<const B3::WordChange> cellTypeChangesInPlace()
{
    return changesOfCellTypeInPlace;
}

Condition PrimitiveEmitter::isUndefinedOrNull(Boxed value, uint16_t occurringPrimitives)
{
    // Within the bits of undefined lie undefined, null, the empty value, and UndefinedTag alone, which no value has.
    constexpr int64_t bitsOutsideUndefined = ~static_cast<int64_t>(JSValue::ValueUndefined);
    static_assert(JSValue::ValueUndefined == (JSValue::ValueNull | JSValue::UndefinedTag) && hasOneBitSet(JSValue::ValueNull) && hasOneBitSet(JSValue::UndefinedTag) && !JSValue::ValueEmpty);
    static_assert(emptyLiteralStringWord > JSValue::ValueUndefined, "a literal string word is tested like a boxed value");

    bool mayBeEmpty = occurringPrimitives & Primitive::Absent;
    if (!mayBeEmpty && B3::Air::Arg::isValidBitImm64Form(bitsOutsideUndefined))
        return Condition(m_out.testIsZero64(value.value(), m_out.constInt64(bitsOutsideUndefined)));
    return Condition(m_out.equal(m_out.bitAnd(value.value(), m_out.constInt64(~JSValue::UndefinedTag)), m_out.constInt64(JSValue::ValueNull)));
}

Number PrimitiveEmitter::numberToDouble(Boxed value)
{
    return select(isInt32(value), toNumber(unboxInt32(value)), unboxDouble(value));
}

CellPointer PrimitiveEmitter::decode(NarrowReference reference)
{
    RELEASE_ASSERT(m_cellCageBase);
    return CellPointer(m_out.add(m_cellCageBase, m_out.shl(m_out.zeroExtPtr(reference.value()), m_out.constInt32(cell32Shift))));
}

NarrowReference PrimitiveEmitter::compress(CellPointer cell)
{
    static_assert(MarkedBlockCage::maxSize == 1ull << (32 + cell32Shift), "The cage base is aligned to maxSize, so the truncation removes it.");
    return NarrowReference(m_out.castToInt32(m_out.lShr(cell.value(), m_out.constInt32(cell32Shift))));
}

Condition PrimitiveEmitter::hasCellTypeIn(CellPointer cell, JSTypeRange range)
{
    RELEASE_ASSERT(range.first <= range.last && isStableCellTypeRange(range));
    Word32 type = loadCellType(cell);
    if (range.first == range.last)
        return equal(type, word32(range.first));
    if (range.last == MaxJSType)
        return aboveOrEqual(type, word32(range.first));
    return below(sub(type, word32(range.first)), word32(range.last - range.first + 1));
}

Condition PrimitiveEmitter::hasCellTypeAbove(CellPointer cell, JSType type)
{
    RELEASE_ASSERT(type < MaxJSType && isStableCellTypeRange({ static_cast<JSType>(type + 1), MaxJSType }));
    return above(loadCellType(cell), word32(type));
}

Condition PrimitiveEmitter::hasOrHadCellType(CellPointer cell, JSType type)
{
    RELEASE_ASSERT(!isTakenInPlace(type));
    return equal(loadCellType(cell), word32(type));
}

// A type that a cell takes or leaves in place is read as one of the bytes that change: alone, so that the comparison takes it from memory.
Condition PrimitiveEmitter::hasMutableCellType(CellPointer cell, JSType type)
{
    constexpr ptrdiff_t offsetInUsefulBytes = JSCell::typeInfoTypeOffset() - JSCell::indexingTypeAndMiscOffset();
    static_assert(offsetInUsefulBytes > 0 && offsetInUsefulBytes < 4);
    return equal(Word32(m_out.load8ZeroExt32(m_out.address(cell.value(), m_heaps.JSCell_usefulBytes, offsetInUsefulBytes))), word32(type));
}

Pointer PrimitiveEmitter::loadStructure(CellPointer cell)
{
    return decodeStructureID(loadStructureID(cell));
}

Pointer PrimitiveEmitter::decodeStructureID(Word32 structureID)
{
    return Pointer(m_out.bitOr(m_out.zeroExtPtr(structureID.value()), loadImmutablePointer(AOT_INSTANCE_PREFIX_OFFSET(structureIDBase)).value()));
}

Pointer PrimitiveEmitter::loadImmutablePointer(ptrdiff_t offset)
{
    return Pointer(m_out.loadPtr(instanceField(m_heaps.AOTInstance_fixedFields, offset)));
}

Word32 PrimitiveEmitter::loadImmutable32(ptrdiff_t offset)
{
    return Word32(m_out.load32(instanceField(m_heaps.AOTInstance_fixedFields, offset)));
}

Integer PrimitiveEmitter::loadImmutable64(ptrdiff_t offset)
{
    return Integer(m_out.load64(instanceField(m_heaps.AOTInstance_fixedFields, offset)));
}

Word32 PrimitiveEmitter::loadMutable32(ptrdiff_t offset)
{
    return Word32(m_out.load32(mutableInstanceField(offset)));
}

Integer PrimitiveEmitter::loadMutable64(ptrdiff_t offset)
{
    return Integer(m_out.load64(mutableInstanceField(offset)));
}

TypedPointer PrimitiveEmitter::instanceField(const B3::AbstractHeap& heap, ptrdiff_t offset)
{
    if (offset >= 0)
        return m_out.address(heap, m_instance, offset);
    ptrdiff_t offsetInPrefix = offset + static_cast<ptrdiff_t>(sizeof(InstancePrefix));
    RELEASE_ASSERT(offsetInPrefix >= 0);
    return m_out.address(heap, m_instancePrefix, offsetInPrefix);
}

CellPointer PrimitiveEmitter::singleCharacterString(Word32 character)
{
    return CellPointer(m_out.loadPtr(m_out.baseIndex(m_heaps.singleCharacterStrings, loadImmutablePointer(AOT_INSTANCE_OFFSET(singleCharacterStrings)).value(), m_out.zeroExtPtr(character.value()))));
}

void PrimitiveEmitter::mutatorFence()
{
#if CPU(X86_64)
    m_out.fence(&m_heaps.root, nullptr);
#else
    LBasicBlock fence = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    m_out.branch(m_out.load8ZeroExt32(instanceField(m_heaps.collectorState, AOT_INSTANCE_OFFSET(mutatorShouldBeFenced))), rareEdge(fence), normalEdge(done));
    m_out.appendTo(fence);
    m_out.fence(&m_heaps.root, nullptr);
    m_out.jump(done);
    m_out.appendTo(done);
#endif
}

void PrimitiveEmitter::ensureStillAliveHere(CellPointer cell)
{
    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Void);
    patchpoint->effects = B3::Effects::none();
    patchpoint->effects.writesLocalState = true;
    patchpoint->effects.reads = B3::HeapRange::top();
    patchpoint->append(cell.value(), B3::ValueRep::ColdAny);
    patchpoint->setRunsWithoutFrame(true);
    patchpoint->setGenerator([](CCallHelpers&, const B3::StackmapGenerationParams&) { });
}

void pinRegisters(B3::Procedure& proc)
{
    proc.pinRegister(instanceGPR);
    proc.pinRegister(cellCageBaseGPR);
#if CPU(ARM64)
    proc.pinRegister(GPRInfo::numberTagRegister);
    proc.pinRegister(GPRInfo::notCellMaskRegister);
    if (proc.mutableGPRs().contains(ARM64Registers::x18, IgnoreVectors))
        proc.pinRegister(ARM64Registers::x18);
#endif
}

static void publishDereferenceableBytes(B3::Procedure& proc, LValue pointer, size_t bytes)
{
    if (bytes)
        proc.setKnownDereferenceableBytes(pointer, static_cast<uint32_t>(std::min<size_t>(bytes, std::numeric_limits<uint32_t>::max())));
}

void definePinnedValues(Output& out, B3::Procedure& proc, InstanceSizes sizes, PinnedValues& pinned)
{
    pinned.instance = out.argumentRegister(instanceGPR);
    pinned.cellCageBase = out.argumentRegister(cellCageBaseGPR);
    pinned.numberTag = out.constInt64(JSValue::NumberTag);
    pinned.notCellMask = out.constInt64(JSValue::NotCellMask);
    pinned.instancePrefix = out.add(pinned.instance, out.constIntPtr(-static_cast<ptrdiff_t>(sizeof(InstancePrefix))));
    pinned.statics = out.add(pinned.instance, out.constIntPtr(InstanceLayout::offsetOfStatics(sizes)));

    publishDereferenceableBytes(proc, pinned.instance, InstanceLayout::bytesReservedFromInstance(sizes));
    publishDereferenceableBytes(proc, pinned.instancePrefix, sizeof(InstancePrefix));
    publishDereferenceableBytes(proc, pinned.statics, sizes.sizeOfStatics);
}

static_assert(absentCell32 < undefinedCell32 && nullCell32 < undefinedCell32 && undefinedCell32 < minCell32);

std::optional<NarrowReference> PrimitiveEmitter::narrowWordBehind(LiteralStringWord word)
{
    B3::Value* value = word.value();
    if (value->opcode() != B3::Select || value->child(2)->opcode() != B3::ZExt32)
        return std::nullopt;
    B3::Value* narrow = value->child(2)->child(0);
    B3::Value* isReference = value->child(0);
    if (isReference->opcode() != B3::AboveEqual || isReference->child(0) != narrow || !isReference->child(1)->isInt32(minCell32))
        return std::nullopt;
    return NarrowReference(narrow);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
