/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubHelpers.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTRuntimeTable.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubRegisters.h"
#include "CompleteSubspace.h"
#include "JSCell.h"
#include "JSObject.h"
#include "MarkedSpace.h"
#include "Structure.h"
#include <array>

namespace JSC { namespace AOT {

static uint32_t firstWordOf(const LayoutData& layout)
{
    static_assert(!(sizeof(LayoutData) % sizeof(uint32_t)));
    return std::bit_cast<std::array<uint32_t, sizeof(LayoutData) / sizeof(uint32_t)>>(layout)[0];
}

StubSignature signatureOfCopyDataPropertiesFrontEnd(StubKey)
{
    return operationFrontEndSignature(5);
}

static void copyChunk(StubAssembler& jit, GPRReg source, GPRReg target, GPRReg offset, GPRReg firstScratch, GPRReg secondScratch)
{
#if CPU(X86_64)
    constexpr FPRReg chunk = CCallHelpers::fpTempRegister;
    UNUSED_PARAM(firstScratch);
    UNUSED_PARAM(secondScratch);
    if (jit.supportsAVX()) {
        jit.m_assembler.vmovups_mr(0, source, offset, CCallHelpers::TimesOne, chunk);
        jit.m_assembler.vmovups_rm(chunk, 0, target, offset, CCallHelpers::TimesOne);
    } else {
        jit.m_assembler.movups_mr(0, source, offset, CCallHelpers::TimesOne, chunk);
        jit.m_assembler.movups_rm(chunk, 0, target, offset, CCallHelpers::TimesOne);
    }
#else
    jit.load64(BaseIndex(source, offset, CCallHelpers::TimesOne), firstScratch);
    jit.load64(BaseIndex(source, offset, CCallHelpers::TimesOne, sizeof(uint64_t)), secondScratch);
    jit.store64(firstScratch, BaseIndex(target, offset, CCallHelpers::TimesOne));
    jit.store64(secondScratch, BaseIndex(target, offset, CCallHelpers::TimesOne, sizeof(uint64_t)));
#endif
}

void generateCopyDataPropertiesFrontEnd(StubAssembler& jit)
{
    const GPRReg target = jit.operand(0);
    const GPRReg source = jit.operand(1);
    const GPRReg excludedRun = jit.operand(2);
    const GPRReg count = jit.operand(3);
    const GPRReg word = jit.result();
    const GPRReg orderWord = jit.allocateScratch(StubRegister::T9);
    const GPRReg structure = jit.allocateScratch(StubRegister::T12);
    const GPRReg layout = structure;
    const GPRReg fieldBytes = structure;
    JumpList callsOperation;

    callsOperation.append(jit.branchIfNotCell(source));
    jit.load32(Address(target, JSCell::structureIDOffset()), structure);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, structure, Address(source, JSCell::structureIDOffset())));
    callsOperation.append(jit.branch8(CCallHelpers::NotEqual, Address(target, JSCell::typeInfoTypeOffset()), TrustedImm32(CompiledObjectType)));
    jit.move(count, word);
    callsOperation.append(jit.branchOr32(CCallHelpers::NonZero, excludedRun, word));
    jit.load32(Address(target, offsetOfShapeWord), word);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, Address(source, offsetOfShapeWord)));
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, Address(target, offsetOfOrderWord), TrustedImm32(unorderedOrderWord)));
    jit.load32(Address(source, offsetOfOrderWord), orderWord);
    callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, orderWord, TrustedImm32(~(orderAndCountMask | orderWordIntegrityBits))));
    jit.and32(TrustedImm32(orderWordOrderMask), orderWord, word);
    callsOperation.append(jit.branch32(CCallHelpers::Equal, word, TrustedImm32(orderStoredOutOfLine)));

    decodeStructureID(jit, structure);
    constexpr uint8_t hasOnlyPlainSlots = static_cast<uint8_t>(CompiledStructure::Flag::HasOnlyPlainSlots);
    constexpr uint8_t flagsOfOtherStorage = static_cast<uint8_t>(CompiledStructure::Flag::HasExtraProperties) | static_cast<uint8_t>(CompiledStructure::Flag::HasOutOfLineKeyOrder);
    jit.load8(Address(structure, CompiledStructure::offsetOfFlags()), word);
    jit.and32(TrustedImm32(hasOnlyPlainSlots | flagsOfOtherStorage), word);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(hasOnlyPlainSlots)));
    callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));

    LayoutData testedBits { };
    testedBits.kindBits = (1 << 4) - 1;
    testedBits.keysAreNeverEnumerated = true;
    testedBits.hasExtensionData = true;
    LayoutData expectedBits { };
    expectedBits.kindBits = static_cast<uint32_t>(LayoutKind::Record);
    jit.loadPtr(Address(structure, CompiledStructure::offsetOfLayout()), layout);
    jit.load32(Address(layout), word);
    jit.and32(TrustedImm32(static_cast<int32_t>(firstWordOf(testedBits))), word);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(static_cast<int32_t>(firstWordOf(expectedBits)))));
    callsOperation.append(branchIfBarrierIsNeeded(jit, target, word));

    constexpr int32_t wordSize = sizeof(uint64_t);
    constexpr int32_t chunkSize = 2 * wordSize;
    static_assert(offsetOfFirstField == chunkSize);
    const GPRReg secondWord = jit.clobberOperand(2);
    jit.load32(Address(layout, OBJECT_OFFSETOF(LayoutData, size)), fieldBytes);
    jit.add32(TrustedImm32(wordSize - 1 - offsetOfFirstField), fieldBytes);
    jit.and32(TrustedImm32(~(wordSize - 1)), fieldBytes);
    Jump hasWholeChunks = jit.branchTest32(CCallHelpers::Zero, fieldBytes, TrustedImm32(wordSize));
    jit.load64(BaseIndex(source, fieldBytes, CCallHelpers::TimesOne, offsetOfFirstField - wordSize), word);
    jit.store64(word, BaseIndex(target, fieldBytes, CCallHelpers::TimesOne, offsetOfFirstField - wordSize));
    jit.sub32(TrustedImm32(wordSize), fieldBytes);
    hasWholeChunks.link(&jit);
    Jump isCopied = jit.branchTest32(CCallHelpers::Zero, fieldBytes);
    Label copyNextChunk = jit.label();
    copyChunk(jit, source, target, fieldBytes, word, secondWord);
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(chunkSize), fieldBytes).linkTo(copyNextChunk, &jit);
    isCopied.link(&jit);
    jit.and32(TrustedImm32(orderAndCountMask), orderWord);
    jit.store32(orderWord, Address(target, offsetOfOrderWord));
    jit.move(TrustedImm32(0), jit.result());
    jit.returnToCaller();

    callsOperation.link(&jit);
    jit.tailCallOperation(Entry::RawCopyDataProperties, OperationCall::FirstArgument::Instance);
}

StubSignature signatureOfCopyObjectFrontEnd(StubKey)
{
    return operationFrontEndSignature(4);
}

void generateCopyObjectFrontEnd(StubAssembler& jit)
{
    const GPRReg source = jit.operand(0);
    const GPRReg excludedRun = jit.clobberOperand(1);
    const GPRReg count = jit.clobberOperand(2);
    const GPRReg copy = jit.result();
    const GPRReg word = jit.allocateScratch(StubRegister::T9);
    const GPRReg size = jit.allocateScratch(StubRegister::T11);
    const GPRReg structure = jit.allocateScratch(StubRegister::T12);
    const GPRReg layout = structure;
    const GPRReg allocator = structure;
    const GPRReg place = structure;
    const GPRReg header = excludedRun;
    const GPRReg shapeWord = count;
    RELEASE_ASSERT(noOverlap(source, excludedRun, count, jit.operand(3), copy, word, size, structure, instanceGPR));
    JumpList callsOperation;
    JumpList callsOperationWithoutExclusions;

    constexpr int32_t wordSize = sizeof(uint64_t);
    constexpr int32_t chunkSize = 2 * wordSize;
    static_assert(offsetOfFirstField == chunkSize && JSFinalObject::offsetOfInlineStorage() == chunkSize && MarkedSpace::sizeStep == chunkSize);
    static_assert(JSCell::cellStateOffset() == wordSize - 1 && !JSCell::structureIDOffset() && std::endian::native == std::endian::little);
    auto allocateAndCopy = [&](bool isRecord) {
        callsOperationWithoutExclusions.append(jit.branch32(CCallHelpers::Above, size, TrustedImm32(MarkedSpace::largeCutoff)));
        jit.add32(TrustedImm32(chunkSize - 1), size);
        jit.urshift32(size, TrustedImm32(4), word);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), allocator);
        jit.loadPtr(BaseIndex(allocator, word, CCallHelpers::ScalePtr, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
        callsOperationWithoutExclusions.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
        allocateCell(jit, copy, allocator, word, callsOperationWithoutExclusions);

        jit.and32(TrustedImm32(~(chunkSize - 1)), size);
        Jump isCopied = jit.branch32(CCallHelpers::Equal, size, TrustedImm32(chunkSize));
        Label copyNextChunk = jit.label();
        jit.sub32(TrustedImm32(chunkSize), size);
        copyChunk(jit, source, copy, size, word, allocator);
        jit.branch32(CCallHelpers::Above, size, TrustedImm32(chunkSize)).linkTo(copyNextChunk, &jit);
        isCopied.link(&jit);
        jit.load64(Address(source, wordSize), word);
        jit.store64(word, Address(copy, wordSize));
        if (isRecord)
            jit.store32(shapeWord, Address(copy, offsetOfShapeWord));
        jit.store64(header, Address(copy));
        fenceAfterAllocation(jit);
        jit.move(TrustedImm32(0), jit.secondResult());
        jit.returnToCaller();
    };

    callsOperation.append(jit.branchIfNotCell(source));
    jit.move(count, word);
    callsOperation.append(jit.branchOr32(CCallHelpers::NonZero, excludedRun, word));
    Jump isNotCompiledObject = jit.branch8(CCallHelpers::NotEqual, Address(source, JSCell::typeInfoTypeOffset()), TrustedImm32(CompiledObjectType));

    jit.load32(Address(source, offsetOfOrderWord), word);
    callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, word, TrustedImm32(~orderAndCountMask)));
    jit.and32(TrustedImm32(orderWordOrderMask), word);
    callsOperation.append(jit.branch32(CCallHelpers::Equal, word, TrustedImm32(orderStoredOutOfLine)));
    loadStructure(jit, source, structure);
    constexpr uint8_t expectedFlags = static_cast<uint8_t>(CompiledStructure::Flag::IsCopiedByWords) | static_cast<uint8_t>(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype);
    constexpr uint8_t flagsOfOtherStorage = static_cast<uint8_t>(CompiledStructure::Flag::HasExtraProperties) | static_cast<uint8_t>(CompiledStructure::Flag::HasOutOfLineKeyOrder);
    jit.load8(Address(structure, CompiledStructure::offsetOfFlags()), word);
    jit.and32(TrustedImm32(expectedFlags | flagsOfOtherStorage), word);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(expectedFlags)));
    callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));
    jit.loadPtr(Address(structure, Structure::realmOffset()), word);
    callsOperation.append(jit.branchPtr(CCallHelpers::NotEqual, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject))));

    LayoutData testedBits { };
    testedBits.kindBits = (1 << 4) - 1;
    testedBits.holdsInnerObjectsInPlace = true;
    testedBits.mayBeInnerObject = true;
    testedBits.keysAreNeverEnumerated = true;
    testedBits.hasExtensionData = true;
    LayoutData expectedBits { };
    expectedBits.kindBits = static_cast<uint32_t>(LayoutKind::Record);
    jit.loadPtr(Address(structure, CompiledStructure::offsetOfLayout()), layout);
    jit.load32(Address(layout), word);
    jit.and32(TrustedImm32(static_cast<int32_t>(firstWordOf(testedBits))), word);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(static_cast<int32_t>(firstWordOf(expectedBits)))));

    jit.load32(Address(layout, OBJECT_OFFSETOF(LayoutData, shape)), shapeWord);
    jit.neg32(shapeWord);
    jit.load32(Address(layout, OBJECT_OFFSETOF(LayoutData, size)), size);
    jit.load64(Address(source), header);
    jit.lshift64(TrustedImm32(8), header);
    jit.urshift64(TrustedImm32(8), header);
    jit.move(TrustedImm64(static_cast<int64_t>(static_cast<uint64_t>(CellState::DefinitelyWhite) << 56)), word);
    jit.or64(word, header);
    allocateAndCopy(true);

    isNotCompiledObject.link(&jit);
    callsOperation.append(jit.branch8(CCallHelpers::NotEqual, Address(source, JSCell::typeInfoTypeOffset()), TrustedImm32(FinalObjectType)));
    callsOperation.append(jit.branchTestPtr(CCallHelpers::NonZero, Address(source, JSObject::butterflyOffset())));
    static_assert(sizeof(ClonedStructure) == 1 << AllocationWords::clonedStructureShift && hasOneBitSet(AllocationWords::numberOfClonedStructures));
    jit.load32(Address(source, JSCell::structureIDOffset()), word);
    jit.and32(TrustedImm32((AllocationWords::numberOfClonedStructures - 1) << AllocationWords::clonedStructureShift), word, place);
    jit.addPtr(instanceGPR, place);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, Address(place, AOT_INSTANCE_OFFSET(allocation.clonedStructures) + OBJECT_OFFSETOF(ClonedStructure, header))));
    jit.load32(Address(place, AOT_INSTANCE_OFFSET(allocation.clonedStructures) + OBJECT_OFFSETOF(ClonedStructure, cellSize)), size);
    jit.load64(Address(place, AOT_INSTANCE_OFFSET(allocation.clonedStructures) + OBJECT_OFFSETOF(ClonedStructure, header)), header);
    allocateAndCopy(false);

    callsOperationWithoutExclusions.link(&jit);
    jit.move(TrustedImm32(0), excludedRun);
    jit.move(TrustedImm32(0), count);
    callsOperation.link(&jit);
    jit.tailCallOperation(Entry::RawCopyObject, OperationCall::FirstArgument::Instance);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
