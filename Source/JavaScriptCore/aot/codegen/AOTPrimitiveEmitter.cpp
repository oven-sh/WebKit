/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPrimitiveEmitter.h"

#if ENABLE(AOT)

#include "AOTMachineTarget.h"
#include "AOTPrimitives.h"
#include "AOTStubRegisters.h"

namespace JSC { namespace AOT { namespace Typed {

struct CellTypeChangeInPlace {
    JSType from;
    JSType to;
};

static constexpr CellTypeChangeInPlace changesOfCellTypeInPlace[] = {
    { ObjectType, FinalObjectType },
    { SlimClosureType, SlimClosureWithPropertiesType },
};

static constexpr bool isStableRangeOfCellTypes(JSTypeRange range)
{
    auto contains = [&](JSType type) {
        return range.first <= type && type <= range.last;
    };
    for (CellTypeChangeInPlace change : changesOfCellTypeInPlace) {
        if (contains(change.from) != contains(change.to))
            return false;
    }
    return true;
}

static constexpr bool isCellTypeTakenInPlace(JSType type)
{
    for (CellTypeChangeInPlace change : changesOfCellTypeInPlace) {
        if (change.to == type)
            return true;
    }
    return false;
}

static_assert(isStableRangeOfCellTypes({ StringType, StringType }) && isStableRangeOfCellTypes({ ObjectType, MaxJSType }) && isStableRangeOfCellTypes(slimClosureTypes) && isStableRangeOfCellTypes({ ArrayType, DerivedArrayType }) && isStableRangeOfCellTypes({ CompiledClosureType, CompiledClosureType }));
static_assert(isStableRangeOfCellTypes({ CompiledObjectType, CompiledObjectType }) && isStableRangeOfCellTypes({ ObjectType, FinalObjectType }));
static_assert(!isStableRangeOfCellTypes({ ObjectType, ObjectType }) && !isStableRangeOfCellTypes({ FinalObjectType, FinalObjectType }));
static_assert(!isStableRangeOfCellTypes({ SlimClosureType, SlimClosureType }) && !isStableRangeOfCellTypes({ SlimClosureWithPropertiesType, SlimClosureWithPropertiesType }));
static_assert(isCellTypeTakenInPlace(FinalObjectType) && isCellTypeTakenInPlace(SlimClosureWithPropertiesType) && !isCellTypeTakenInPlace(ObjectType) && !isCellTypeTakenInPlace(SlimClosureType));

bool isStableCellTypeRange(JSTypeRange range)
{
    return isStableRangeOfCellTypes(range);
}

Condition PrimitiveEmitter::isUndefinedOrNull(Boxed value, uint16_t occurringPrimitives)
{
    constexpr int64_t bitsOutsideUndefined = ~static_cast<int64_t>(JSValue::ValueUndefined);
    static_assert(JSValue::ValueUndefined == (JSValue::ValueNull | JSValue::UndefinedTag) && hasOneBitSet(JSValue::ValueNull) && hasOneBitSet(JSValue::UndefinedTag) && !JSValue::ValueEmpty);

    bool mayBeEmpty = occurringPrimitives & Primitive::Absent;
    if (!mayBeEmpty && Machine::isEncodableBitImmediate(Width64, bitsOutsideUndefined))
        return Condition(m_out.testIsZero64(value.value(), m_out.constInt64(bitsOutsideUndefined)));
    return Condition(m_out.equal(m_out.bitAnd(value.value(), ~JSValue::UndefinedTag), JSValue::ValueNull));
}

Number PrimitiveEmitter::numberToDouble(Boxed value)
{
    Condition holdsInt32 = isInt32(value);
    Number fromInt32 = toNumber(unboxInt32(value));
    return select(holdsInt32, fromInt32, unboxDouble(value));
}

CellPointer PrimitiveEmitter::decode(NarrowReference reference)
{
    RELEASE_ASSERT(m_cellCageBase);
    return CellPointer(m_out.add(m_cellCageBase, m_out.shl(m_out.zeroExtPtr(reference.value()), cell32Shift)));
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
    return below(sub(type, range.first), range.last - range.first + 1);
}

Condition PrimitiveEmitter::hasCellTypeAbove(CellPointer cell, JSType type)
{
    RELEASE_ASSERT(type < MaxJSType && isStableCellTypeRange({ static_cast<JSType>(type + 1), MaxJSType }));
    return above(loadCellType(cell), type);
}

Condition PrimitiveEmitter::hasOrHadCellType(CellPointer cell, JSType type)
{
    RELEASE_ASSERT(!isCellTypeTakenInPlace(type));
    return equal(loadCellType(cell), type);
}

Condition PrimitiveEmitter::hasMutableCellType(CellPointer cell, JSType type)
{
    constexpr ptrdiff_t offsetInUsefulBytes = JSCell::typeInfoTypeOffset() - JSCell::indexingTypeAndMiscOffset();
    static_assert(offsetInUsefulBytes > 0 && offsetInUsefulBytes < 4);
    return equal(Word32(m_out.load8ZeroExt32(m_out.address(cell.value(), m_heaps.JSCell_usefulBytes, offsetInUsefulBytes))), type);
}

Pointer PrimitiveEmitter::loadStructure(CellPointer cell)
{
    return decodeStructureID(loadStructureID(cell));
}

Pointer PrimitiveEmitter::decodeStructureID(Word32 structureID)
{
    LValue offset = m_out.zeroExtPtr(structureID.value());
    return Pointer(m_out.bitOr(offset, loadImmutablePointer(AOT_INSTANCE_PREFIX_OFFSET(structureIDBase)).value()));
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

Word32 PrimitiveEmitter::loadFilledOnce32(ptrdiff_t offset)
{
    return Word32(m_out.load32(instanceField(m_heaps.AOTInstance_fieldsFilledOnce, offset)));
}

Word32 PrimitiveEmitter::loadMutable32(ptrdiff_t offset)
{
    return Word32(m_out.load32(mutableInstanceField(offset)));
}

TypedPointer PrimitiveEmitter::instanceField(const Machine::AbstractHeap& heap, ptrdiff_t offset)
{
    if (offset >= 0)
        return m_out.address(heap, m_instance, offset);
    ptrdiff_t offsetInPrefix = offset + static_cast<ptrdiff_t>(sizeof(InstancePrefix));
    RELEASE_ASSERT(offsetInPrefix >= 0);
    return m_out.address(heap, m_instancePrefix, offsetInPrefix);
}

CellPointer PrimitiveEmitter::singleCharacterString(Word32 character)
{
    Pointer strings = loadImmutablePointer(AOT_INSTANCE_OFFSET(singleCharacterStrings));
    return CellPointer(m_out.loadPtr(m_out.baseIndex(m_heaps.singleCharacterStrings, strings.value(), m_out.zeroExtPtr(character.value()))));
}

LValue PrimitiveEmitter::countTrailingZerosOfNonZero(LValue bits)
{
    Machine::PatchOperation* count = m_out.patchpoint(Machine::Int64);
    count->appendSomeRegister(bits);
    count->effects = Machine::Effects::none();
    count->setRunsWithoutFrame(true);
    count->setGenerator([](CCallHelpers& jit, const Machine::PatchParameters& params) {
        jit.countTrailingZeros64WithoutNullCheck(params[1].gpr(), params[0].gpr());
    });
    return count;
}

void PrimitiveEmitter::mutatorFence()
{
    m_owesFenceBehindStores = false;
    if (isX86_64())
        return;

    LBasicBlock fence = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    LValue shouldBeFenced = m_out.load8ZeroExt32(instanceField(m_heaps.collectorState, AOT_INSTANCE_OFFSET(mutatorShouldBeFenced)));
    ScopeFork fork = m_out.forkScope();
    m_out.branch(shouldBeFenced, rareEdge(fence), normalEdge(done));
    m_out.appendToArm(fence, fork);
    m_out.fence();
    m_out.jump(done);
    m_out.appendToJoin(done, fork);
}

void PrimitiveEmitter::ensureStillAliveHere(CellPointer cell)
{
    Machine::PatchOperation* patch = m_out.patchpoint(Machine::Void);
    patch->effects = Machine::Effects::none();
    patch->effects.writesLocalState = true;
    patch->effects.reads = Machine::HeapSet::top();
    patch->append(cell.value(), Machine::ValueRep::ColdAny);
    patch->setRunsWithoutFrame(true);
    patch->onlyKeepsChildrenAlive = true;
}

void definePinnedValues(Output& out, PinnedValues& pinned)
{
    pinned.instance = out.argumentRegister(instanceGPR);
    pinned.cellCageBase = out.argumentRegister(cellCageBaseGPR);
    pinned.numberTag = out.constInt64(JSValue::NumberTag);
    pinned.notCellMask = out.constInt64(JSValue::NotCellMask);
    pinned.instancePrefix = out.add(pinned.instance, out.constIntPtr(-static_cast<ptrdiff_t>(sizeof(InstancePrefix))));
}

static_assert(absentCell32 < undefinedCell32 && nullCell32 < undefinedCell32 && undefinedCell32 < minCell32);

std::optional<Word32> PrimitiveEmitter::narrowWordBehind(OptionalInteger widened)
{
    if (LValue narrow = m_narrowWordsOfIntegers.get(widened.value()))
        return Word32(narrow);
    return std::nullopt;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
