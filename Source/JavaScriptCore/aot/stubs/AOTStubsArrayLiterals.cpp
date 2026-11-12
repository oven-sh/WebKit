/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsArrays.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsObjectCreation.h"
#include "ArrayConventions.h"
#include "CompleteSubspace.h"
#include "IndexingHeader.h"
#include "IndexingType.h"
#include "JSCellButterfly.h"
#include "JSObject.h"
#include "MarkedSpace.h"
#include "PureNaN.h"
#include "Structure.h"
#include "StructureRareData.h"
#include <algorithm>
#include <bit>
#include <optional>

namespace JSC { namespace AOT {

static_assert(descriptorOf(StubFamily::NewEmptyArray).numberOfForms == static_cast<unsigned>(EmptyArray::ContiguousCapacity17) + 1);

static constexpr StubRegister countAndNumberMaskRegister = StubRegister::T13;

ptrdiff_t offsetOfArrayWithStorageAllocator(unsigned vectorLength, unsigned propertyRoom)
{
    return CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(sizeOfArrayWithStorage(vectorLength, propertyRoom)) * sizeof(Allocator);
}

Jump branchIfHavingABadTime(StubAssembler& jit)
{
    return jit.branchTest32(CCallHelpers::Zero, Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(newArrayWithContiguousHeader)));
}

static Jump branchIfEitherIsNaN(CCallHelpers& jit, FPRReg first, FPRReg second)
{
#if CPU(ARM64)
    jit.m_assembler.fcmp<64>(first, second);
    jit.m_assembler.b_cond(ARM64Assembler::ConditionVS);
    AssemblerLabel branch = jit.m_assembler.labelIgnoringWatchpoints();
    jit.m_assembler.nop();
    return Jump(branch, ARM64Assembler::JumpCondition, ARM64Assembler::ConditionVS);
#else
    if (jit.supportsAVX())
        jit.m_assembler.vucomisd_rr(second, first);
    else
        jit.m_assembler.ucomisd_rr(second, first);
    return Jump(jit.m_assembler.jCC(X86Assembler::ConditionP));
#endif
}

struct ArrayLiteralShape {
    unsigned count { 0 };
    std::optional<unsigned> emptyCapacity { };
    bool areDoubles { false };
    bool areInt32 { false };
    bool hasPropertyRoom { false };
    bool choosesStorage { false };
};

static void emitStoreBoxedInt32(StubAssembler& jit, unsigned count, GPRReg base, size_t offset, GPRReg scratch)
{
    constexpr int32_t tag = static_cast<int32_t>(JSValue::NumberTag >> 32);
    auto at = [&](unsigned i) { return static_cast<int32_t>(offset + i * sizeof(EncodedJSValue)); };
#if CPU(ARM64)
    jit.move(TrustedImm32(tag), scratch);
    for (unsigned i = 0; i < count; ++i)
        jit.storePair32(jit.operand(i), scratch, base, TrustedImm32(at(i)));
#else
    UNUSED_PARAM(scratch);
    for (unsigned i = 0; i < count; ++i) {
        jit.store32(jit.operand(i), Address(base, at(i)));
        jit.store32(TrustedImm32(tag), Address(base, at(i) + static_cast<int32_t>(sizeof(int32_t))));
    }
#endif
}

static void emitArrayLiteral(StubAssembler& jit, ArrayLiteralShape shape)
{
    const auto [count, emptyCapacity, areDoubles, areInt32, hasPropertyRoom, choosesStorage] = shape;
    auto number = [&](unsigned i) { return jit.operandFPR(1 + i); };
    static_assert(maxArrayElementsInRegisters == 4);
    RELEASE_ASSERT(count <= maxArrayElementsInRegisters);
    static_assert(arrayLiteralVectorLength(0) == (BASE_CONTIGUOUS_VECTOR_LEN_EMPTY | 1));
    RELEASE_ASSERT(!emptyCapacity || (!count && *emptyCapacity & 1));
    const unsigned vectorLength = emptyCapacity.value_or(arrayLiteralVectorLength(count));
    const unsigned countAndCapacity = count | emptyCapacity.value_or(0) << capacityShiftInArrayLiteralCount;
    const GPRReg array = jit.result();
    const GPRReg storage = jit.allocateScratch(arrayStorageRegister);
    const GPRReg allocator = jit.allocateScratch(StubRegister::T9);
    const GPRReg scratch = jit.allocateScratch(arrayAllocationScratchRegister);

    JumpList slowPath;
    if (choosesStorage) {
        jit.move(jit.operand(0), scratch);
        for (unsigned i = 1; i < count; ++i)
            jit.and64(jit.operand(i), scratch);
        slowPath.append(jit.branch64(CCallHelpers::Below, scratch, numberTag));
    }
    for (unsigned i = 0; areDoubles && i < count; i += 2)
        slowPath.append(branchIfEitherIsNaN(jit, number(i), number(std::min(i + 1, count - 1))));
    const unsigned propertyRoom = hasPropertyRoom ? propertyRoomOfArray : 0;
    const unsigned indexOfAllocator = ShortArrayWords::indexOfAllocator(vectorLength + propertyRoom / sizeof(EncodedJSValue));
    RELEASE_ASSERT(indexOfAllocator < ShortArrayWords::numberOfAllocators);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(shortArrays.allocators) + indexOfAllocator * sizeof(void*)), allocator);
    AssemblyHelpers::FreeListRefill arrayRefill = allocateCellWithLateRefill(jit, array, allocator, scratch, slowPath, sizeOfArrayWithStorage(vectorLength, propertyRoom));
    emitStoreValuesAndClear(jit, 1, 0, propertyRoom / sizeof(EncodedJSValue), array, sizeof(JSArray));
    jit.addPtr(TrustedImm32(sizeof(JSArray) + propertyRoom), array, storage);
    jit.store64(TrustedImm64(static_cast<uint64_t>(vectorLength) << 32 | count), Address(storage, 0));
    if (areDoubles) {
        for (unsigned i = 0; i < count; ++i)
            jit.storeDouble(number(i), Address(storage, sizeof(IndexingHeader) + i * sizeof(double)));
        if (count < vectorLength)
            jit.move(TrustedImm64(std::bit_cast<int64_t>(PNaN)), scratch);
        for (unsigned i = count; i < vectorLength; ++i)
            jit.store64(scratch, Address(storage, sizeof(IndexingHeader) + i * sizeof(double)));
    } else if (areInt32) {
        emitStoreBoxedInt32(jit, count, storage, sizeof(IndexingHeader), scratch);
        emitStoreValuesAndClear(jit, 1, 0, vectorLength - count, storage, sizeof(IndexingHeader) + count * sizeof(EncodedJSValue));
    } else
        emitStoreValuesAndClear(jit, 1, count, vectorLength, storage, sizeof(IndexingHeader));
    jit.addPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
    jit.load64(Address(instanceGPR, Instance::offsetOfNewArrayHeader(areDoubles ? ArrayWithDouble : areInt32 || choosesStorage ? ArrayWithInt32 : ArrayWithContiguous)), scratch);
    jit.storePair64(scratch, storage, array, TrustedImm32(0));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    emitLateRefill(jit, arrayRefill, slowPath);
    slowPath.link(&jit);
    if (areDoubles) {
        jit.jumpToStub(arrayLiteralWithDoubleSlow());
        return;
    }
    jit.callOperationAndReturn(Entry::operationAOTNewArray, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .spilledArguments = OperationCall::SpilledArguments { count, countAndCapacity | (areInt32 ? int32ValuesInArrayLiteralCount : 0) | (hasPropertyRoom ? roomInArrayLiteralCount : 0) | (choosesStorage ? numbersInArrayLiteralCount : 0) },
    });
}

StubSignature signatureOfArrayLiteral(StubKey key)
{
    StubSignature signature = StubSignature::withGPRResult({ });
    for (unsigned i = 0; i < key.count(); ++i)
        signature.addOperand(operationArgumentGPR(1 + i));
    return key.count() ? signature.fillingOperandInFront(key.count() - 1) : signature;
}

void generateArrayLiteral(StubAssembler& jit) { emitArrayLiteral(jit, { .count = jit.key().count() }); }
StubSignature signatureOfArrayLiteralWithInt32(StubKey key) { return signatureOfArrayLiteral(arrayLiteral(key.count())); }
void generateArrayLiteralWithInt32(StubAssembler& jit) { emitArrayLiteral(jit, { .count = jit.key().count(), .areInt32 = true }); }
StubSignature signatureOfArrayLiteralOfNumbers(StubKey key) { return signatureOfArrayLiteral(arrayLiteral(key.count())); }
void generateArrayLiteralOfNumbers(StubAssembler& jit) { emitArrayLiteral(jit, { .count = jit.key().count(), .choosesStorage = true }); }
StubSignature signatureOfArrayLiteralWithRoom(StubKey key) { return signatureOfArrayLiteral(arrayLiteral(key.count())); }
void generateArrayLiteralWithRoom(StubAssembler& jit) { emitArrayLiteral(jit, { .count = jit.key().count(), .areInt32 = !!jit.key().form(), .hasPropertyRoom = true }); }

static StubSignature signatureTakingDoubles(unsigned count)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(countAndNumberMaskRegister) });
    for (unsigned i = 0; i < count; ++i)
        signature.addOperand(FPRInfo::toArgumentRegister(i));
    return signature;
}

StubSignature signatureOfArrayLiteralWithDouble(StubKey key) { return signatureTakingDoubles(key.count()); }
StubSignature signatureOfArrayLiteralWithDoubleSlow(StubKey) { return signatureTakingDoubles(maxArrayElementsInRegisters); }

void generateArrayLiteralWithDouble(StubAssembler& jit) { emitArrayLiteral(jit, { .count = jit.key().count(), .areDoubles = true }); }

void generateArrayLiteralWithDoubleSlow(StubAssembler& jit)
{
    jit.callOperationAndReturn(Entry::operationAOTNewArrayFromDoubles, {
        .returns = OperationCall::Returns::Value,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .secondArgumentIn = jit.operand(0),
    });
}

StubSignature signatureOfNewCopyOnWriteArray(StubKey key)
{
    return StubSignature::withGPRResult({ gprOf(shortStubImmediateRegister), gprOf(StubRegister::T11) }).withResultIn(key.operandRegister());
}

void generateNewCopyOnWriteArray(StubAssembler& jit)
{
    const auto kind = static_cast<ArrayElementKind>(jit.key().form() % 3);
    const auto elements = static_cast<SharedElements>(jit.key().form() / 3);
    const Address header(instanceGPR, Instance::offsetOfNewCopyOnWriteArrayHeader(indexingTypeOf(kind)));
    const GPRReg array = jit.result();
    const GPRReg request = jit.operand(0);
    const GPRReg firstElement = jit.clobberOperand(1);
    const GPRReg allocator = jit.allocateScratch(scratchBesideShortStubImmediate);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T13);
    static_assert(sizeof(JSArray) <= MarkedSpace::preciseCutoff && !(sizeof(JSArray) % MarkedSpace::sizeStep));

    JumpList slowPath;
    switch (elements) {
    case SharedElements::InImage:
        jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.data)), firstElement);
        break;
    case SharedElements::InStorageOfSite:
        slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, firstElement));
        jit.addPtr(TrustedImm32(JSCellButterfly::offsetOfData()), firstElement);
        break;
    }
    slowPath.append(jit.branchTest64(CCallHelpers::Zero, header));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(arrayAllocator)), allocator);
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, array, allocator, scratch, slowPath, sizeof(JSArray));
    jit.load64(header, scratch);
    jit.storePair64(scratch, firstElement, array, TrustedImm32(0));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slowPath);
    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTNewFromData, { .saves = OperationCall::Saves::CallerSavedRegisters, .secondArgumentIn = request });
}

StubSignature signatureOfListOwnKeys(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A1) }); }

void generateListOwnKeys(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    const GPRReg array = jit.result();
    const GPRReg list = jit.allocateScratch(StubRegister::T9);
    const GPRReg word = jit.allocateScratch(StubRegister::T11);
    const GPRReg order = jit.allocateScratch(StubRegister::T12);
    const GPRReg entry = jit.allocateScratch(StubRegister::T13);
    const Address header(instanceGPR, Instance::offsetOfNewCopyOnWriteArrayHeader(CopyOnWriteArrayWithContiguous));
    RELEASE_ASSERT(noOverlap(value, list, word, order, entry) && noOverlap(value, array));

    JumpList slowPath;
    jit.load32(Address(value, offsetOfShapeWord), word);
    Jump isCompiledObject = jit.branchTest32(CCallHelpers::NonZero, word, TrustedImm32(static_cast<int32_t>(compiledObjectShapeBit)));
    loadStructure(jit, value, word);
    jit.loadPtr(Address(word, Structure::previousOrRareDataOffset()), list);
    slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, list));
    slowPath.append(jit.branch8(CCallHelpers::Equal, Address(list, JSCell::typeInfoTypeOffset()), TrustedImm32(StructureType)));
    jit.loadPtr(Address(list, StructureRareData::offsetOfCachedPropertyNames(CachedPropertyNamesKind::EnumerableStrings)), list);
    slowPath.append(jit.branch64(CCallHelpers::BelowOrEqual, list, TrustedImm32(static_cast<int32_t>(std::bit_cast<uintptr_t>(StructureRareData::cachedPropertyNamesSentinel())))));

    Label hasList = jit.label();
    slowPath.append(jit.branchTest64(CCallHelpers::Zero, header));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(arrayAllocator)), entry);
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, array, entry, order, slowPath, sizeof(JSArray));
    jit.load64(header, order);
    jit.addPtr(TrustedImm32(JSCellButterfly::offsetOfData()), list);
    jit.storePair64(order, list, array, TrustedImm32(0));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    isCompiledObject.link(&jit);
    auto findList = [&](JumpList& isNotListed) {
        static_assert(sizeof(KeyListCacheEntry) == 16 && hasOneBitSet(numberOfKeyLists));
        jit.add32(word, order, entry);
        jit.and32(TrustedImm32(numberOfKeyLists - 1), entry);
        jit.lshift32(TrustedImm32(getLSBSet(sizeof(KeyListCacheEntry))), entry);
        jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(objects.keyLists)), entry);
        isNotListed.append(jit.branch32(CCallHelpers::NotEqual, Address(entry, OBJECT_OFFSETOF(KeyListCacheEntry, shapeWord)), word));
        isNotListed.append(jit.branch32(CCallHelpers::NotEqual, Address(entry, OBJECT_OFFSETOF(KeyListCacheEntry, orderWord)), order));
        jit.loadPtr(Address(entry, OBJECT_OFFSETOF(KeyListCacheEntry, list)), list);
        jit.branchTestPtr(CCallHelpers::NonZero, list).linkTo(hasList, &jit);
    };
    JumpList isNotListedByShape;
    jit.or32(TrustedImm32(shapeWordIsUnmarked), word);
    jit.load32(Address(value, offsetOfOrderWord), order);
    findList(isNotListedByShape);
    isNotListedByShape.link(&jit);
    slowPath.append(jit.branchTest32(CCallHelpers::Zero, order, TrustedImm32(static_cast<int32_t>(orderWordHasExtraProperties))));
    static_assert(keyListWordOfStructure(UINT32_MAX) == UINT32_MAX >> 1);
    jit.load32(Address(value, JSCell::structureIDOffset()), word);
    jit.urshift32(TrustedImm32(1), word);
    findList(slowPath);
    slowPath.append(jit.jump());

    emitLateRefill(jit, refill, slowPath);
    slowPath.link(&jit);
    constexpr uint32_t kindOfEnumerableStrings = 0;
    OperationCall call { .saves = OperationCall::Saves::CallerSavedRegisters, .secondArgumentIn = value };
    call.constantArguments[0] = OperationCall::ConstantArgument { operationArgumentGPR(2), kindOfEnumerableStrings };
    jit.callOperationAndReturn(Entry::operationAOTOwnPropertyKeys, call);
}

StubSignature signatureOfNewEmptyArray(StubKey) { return StubSignature::withGPRResult({ }); }

void generateNewEmptyArray(StubAssembler& jit)
{
    const EmptyArray form = static_cast<EmptyArray>(jit.key().form());
    switch (form) {
    case EmptyArray::ContiguousCapacity1:
        return emitArrayLiteral(jit, { .emptyCapacity = 1 });
    case EmptyArray::ContiguousCapacity3:
        return emitArrayLiteral(jit, { .emptyCapacity = 3 });
    case EmptyArray::ContiguousCapacity7:
        return emitArrayLiteral(jit, { .emptyCapacity = 7 });
    case EmptyArray::ContiguousCapacity9:
        return emitArrayLiteral(jit, { .emptyCapacity = 9 });
    case EmptyArray::ContiguousCapacity13:
        return emitArrayLiteral(jit, { .emptyCapacity = 13 });
    case EmptyArray::ContiguousCapacity17:
        return emitArrayLiteral(jit, { .emptyCapacity = 17 });
    case EmptyArray::DoubleCapacity1:
    case EmptyArray::DoubleCapacity3:
    case EmptyArray::DoubleDefaultCapacity:
    case EmptyArray::Int32DefaultCapacity:
    case EmptyArray::Int32Capacity1:
    case EmptyArray::Int32Capacity3:
        return emitEmptyArrayOfHoles(jit, form);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
