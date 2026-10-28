/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsObjectCopy.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompiledStructure.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTMappedCopy.h"
#include "AOTRuntimeTable.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "CompleteSubspace.h"
#include "JSCell.h"
#include "JSObject.h"
#include "MarkedBlockCage.h"
#include "MarkedSpace.h"
#include "Structure.h"

namespace JSC { namespace AOT {

static constexpr bool copyChunkWritesScratches = !isX86_64();

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

struct MoveWalk {
    GPRReg target;
    GPRReg source;
    GPRReg copy;
    GPRReg endOfRun;
    // Of the move, from the end of its run.
    GPRReg offset;
    GPRReg bits;
    GPRReg scratch;
    // On x86-64 an instruction takes the operand out of memory.
    GPRReg operandOfMove;
};

static void walkRun(StubAssembler& jit, const MoveWalk& walk, MappedCopy::RunOfStub run, JumpList& isRefused)
{
    using Run = MappedCopy::RunOfStub;
    const ptrdiff_t offsetOfRun = OBJECT_OFFSETOF(MappedCopy, runsOfStub) + static_cast<size_t>(run) * sizeof(MappedCopy::Run);
    auto inMove = [&](ptrdiff_t offset) {
        return BaseIndex(walk.endOfRun, walk.offset, CCallHelpers::TimesOne, static_cast<int32_t>(offset));
    };
    const BaseIndex slotOfSource(walk.source, walk.bits, CCallHelpers::TimesOne);
    const BaseIndex slotOfTarget(walk.target, walk.scratch, CCallHelpers::TimesOne);
    auto branchIfRefused32 = [&] {
        jit.move(walk.bits, walk.scratch);
#if CPU(X86_64)
        jit.sub32(inMove(OBJECT_OFFSETOF(MappedCopy::Move, firstBitsRefusedByStub)), walk.scratch);
        isRefused.append(jit.branch32(CCallHelpers::Above, inMove(OBJECT_OFFSETOF(MappedCopy::Move, numberOfBitsRefusedByStub)), walk.scratch));
#else
        jit.load32(inMove(OBJECT_OFFSETOF(MappedCopy::Move, firstBitsRefusedByStub)), walk.operandOfMove);
        jit.sub32(walk.operandOfMove, walk.scratch);
        jit.load32(inMove(OBJECT_OFFSETOF(MappedCopy::Move, numberOfBitsRefusedByStub)), walk.operandOfMove);
        isRefused.append(jit.branch32(CCallHelpers::Below, walk.scratch, walk.operandOfMove));
#endif
    };
    auto branchIfRefused64 = [&] {
        jit.move(walk.bits, walk.scratch);
#if CPU(X86_64)
        jit.sub64(inMove(OBJECT_OFFSETOF(MappedCopy::Move, firstBitsRefusedByStub)), walk.scratch);
#else
        jit.load64(inMove(OBJECT_OFFSETOF(MappedCopy::Move, firstBitsRefusedByStub)), walk.operandOfMove);
        jit.sub64(walk.operandOfMove, walk.scratch);
#endif
        isRefused.append(jit.branch64(CCallHelpers::Above, inMove(OBJECT_OFFSETOF(MappedCopy::Move, numberOfBitsRefusedByStub)), walk.scratch));
    };

    jit.loadPtr(Address(walk.copy, offsetOfRun + OBJECT_OFFSETOF(MappedCopy::Run, negativeSize)), walk.offset);
    Jump isEmpty = jit.branchTestPtr(CCallHelpers::Zero, walk.offset);
    jit.loadPtr(Address(walk.copy, offsetOfRun + OBJECT_OFFSETOF(MappedCopy::Run, end)), walk.endOfRun);
    Label makeNextMove = jit.label();
    jit.load32(inMove(OBJECT_OFFSETOF(MappedCopy::Move, from)), walk.bits);
    switch (run) {
    case Run::HalfWords:
        jit.load32(slotOfSource, walk.bits);
        branchIfRefused32();
        jit.load32(inMove(OBJECT_OFFSETOF(MappedCopy::Move, to)), walk.scratch);
        jit.store32(walk.bits, slotOfTarget);
        break;
    case Run::NarrowedReferences:
        static_assert(MarkedBlockCage::maxSize == 1ull << (32 + cell32Shift));
        jit.load64(slotOfSource, walk.bits);
        jit.sub64(cellCageBaseGPR, walk.bits);
        jit.urshift64(TrustedImm32(cell32Shift), walk.bits);
        jit.zeroExtend32ToWord(walk.bits, walk.scratch);
        isRefused.append(jit.branch64(CCallHelpers::NotEqual, walk.bits, walk.scratch));
        jit.load32(inMove(OBJECT_OFFSETOF(MappedCopy::Move, to)), walk.scratch);
        jit.store32(walk.bits, slotOfTarget);
        break;
    case Run::Words:
        jit.load64(slotOfSource, walk.bits);
        branchIfRefused64();
        jit.load32(inMove(OBJECT_OFFSETOF(MappedCopy::Move, to)), walk.scratch);
        jit.store64(walk.bits, slotOfTarget);
        break;
    case Run::Bytes:
        jit.load8(slotOfSource, walk.bits);
        branchIfRefused32();
        jit.load32(inMove(OBJECT_OFFSETOF(MappedCopy::Move, to)), walk.scratch);
        jit.store8(walk.bits, slotOfTarget);
        break;
    case Run::Int32sAsDoubles:
        jit.load32(slotOfSource, walk.bits);
        branchIfRefused32();
        jit.convertInt32ToDouble(walk.bits, CCallHelpers::fpTempRegister);
        jit.load32(inMove(OBJECT_OFFSETOF(MappedCopy::Move, to)), walk.scratch);
        jit.storeDouble(CCallHelpers::fpTempRegister, slotOfTarget);
        break;
    case Run::BoxedInt32s:
        jit.load32(slotOfSource, walk.bits);
        branchIfRefused32();
        boxInt32(jit, walk.bits, walk.bits);
        jit.load32(inMove(OBJECT_OFFSETOF(MappedCopy::Move, to)), walk.scratch);
        jit.store64(walk.bits, slotOfTarget);
        break;
    case Run::WidenedReferences: {
        static_assert(nullCell32 == JSValue::ValueNull && undefinedCell32 == JSValue::ValueUndefined && nullCell32 < undefinedCell32);
        jit.load32(slotOfSource, walk.bits);
        isRefused.append(jit.branchTest32(CCallHelpers::Zero, walk.bits));
        Jump isNoReference = jit.branch32(CCallHelpers::BelowOrEqual, walk.bits, TrustedImm32(undefinedCell32));
        jit.lshift64(TrustedImm32(cell32Shift), walk.bits);
        jit.add64(cellCageBaseGPR, walk.bits);
        isNoReference.link(&jit);
        jit.load32(inMove(OBJECT_OFFSETOF(MappedCopy::Move, to)), walk.scratch);
        jit.store64(walk.bits, slotOfTarget);
        break;
    }
    }
    jit.branchAdd64(CCallHelpers::NonZero, TrustedImm32(sizeof(MappedCopy::Move)), walk.offset).linkTo(makeNextMove, &jit);
    isEmpty.link(&jit);
}

// The copy that is kept at the place of this one. Where there is none, every copy of this kind takes a frame.
static Jump loadCopyWithoutFrame(StubAssembler& jit, GPRReg target, GPRReg source, GPRReg excludedRun, GPRReg copy)
{
    static_assert(std::endian::native == std::endian::little && offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t));
    static_assert(hasOneBitSet(AllocationWords::numberOfCopiesWithoutFrame));
    static_assert(AllocationWords::placeOfCopyWithoutFrame(5ull << 32, 3ull << 32 | 2, 1) == 2 * 5 + 3 + 2 + 1);
    const GPRReg place = copy;
    jit.load32(Address(target, offsetOfShapeWord), place);
    jit.getEffectiveAddress(BaseIndex(excludedRun, place, CCallHelpers::TimesTwo), place);
    jit.add32(Address(source, offsetOfShapeWord), place);
    jit.add32(Address(source, offsetOfOrderWord), place);
    jit.and32(TrustedImm32(AllocationWords::numberOfCopiesWithoutFrame - 1), place);
    jit.loadPtr(BaseIndex(instanceGPR, place, CCallHelpers::ScalePtr, AOT_INSTANCE_OFFSET(allocation.copiesWithoutFrame)), copy);
    return jit.branchTestPtr(CCallHelpers::Zero, copy);
}

// What tryCopyByFieldMapWithoutFrame() does, for a copy all of whose moves the stub makes. It falls through where that function is to make the copy: the copy is kept for this source and the stub does not make its moves, or has refused one.
// The places are few, so in a large program most hold a copy, and most often that of other objects: then the copy takes a frame at once.
static void copyByFieldMap(StubAssembler& jit, GPRReg target, GPRReg source, GPRReg excludedRun, GPRReg count, GPRReg excludedKeys, GPRReg structureIDOfTarget, GPRReg copy, GPRReg word, JumpList& takesFrame, const Invocable<void()> auto& leaveWithCopyMade)
{
    const GPRReg structure = structureIDOfTarget;
    takesFrame.append(jit.branchTest32(CCallHelpers::NonZero, count));
    takesFrame.append(loadCopyWithoutFrame(jit, target, source, excludedRun, copy));
    jit.load32(Address(source, JSCell::structureIDOffset()), word);
    Jump makesNoMoves = jit.branch32(CCallHelpers::NotEqual, word, Address(copy, OBJECT_OFFSETOF(MappedCopy, structureOfSourceForStub)));
    takesFrame.append(jit.branch32(CCallHelpers::NotEqual, structureIDOfTarget, Address(copy, OBJECT_OFFSETOF(MappedCopy, structureOfTarget))));
    jit.load64(Address(source, offsetOfOrderWord), word);
    takesFrame.append(jit.branch64(CCallHelpers::NotEqual, word, Address(copy, OBJECT_OFFSETOF(MappedCopy, sourceWords))));
    jit.load64(Address(target, offsetOfOrderWord), word);
    takesFrame.append(jit.branch64(CCallHelpers::NotEqual, word, Address(copy, OBJECT_OFFSETOF(MappedCopy, targetWords))));
    takesFrame.append(jit.branch32(CCallHelpers::NotEqual, excludedRun, Address(copy, OBJECT_OFFSETOF(MappedCopy, excludedRunNumberPlusOne))));
    takesFrame.append(branchIfBarrierIsNeeded(jit, target, word));
    decodeStructureID(jit, structure);
    takesFrame.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));

    // The operation reads no keys where their number is zero.
    const MoveWalk walk {
        .target = target,
        .source = source,
        .copy = copy,
        .endOfRun = count,
        .offset = excludedKeys,
        .bits = structure,
        .scratch = word,
#if CPU(X86_64)
        .operandOfMove = InvalidGPRReg,
#else
        .operandOfMove = jit.reuseScratch(StubRegister::T10),
#endif
    };
    JumpList isRefused;
    walkRun(jit, walk, MappedCopy::RunOfStub::HalfWords, isRefused);
    walkRun(jit, walk, MappedCopy::RunOfStub::NarrowedReferences, isRefused);
    Jump hasMoreRuns = jit.branchTest8(CCallHelpers::NonZero, Address(copy, OBJECT_OFFSETOF(MappedCopy, hasRunsBehindSecondForStub)));
    Label isCopied = jit.label();
    jit.load32(Address(copy, OBJECT_OFFSETOF(MappedCopy, orderWordAfter)), word);
    jit.store32(word, Address(target, offsetOfOrderWord));
    leaveWithCopyMade();

    hasMoreRuns.link(&jit);
    walkRun(jit, walk, MappedCopy::RunOfStub::Words, isRefused);
    walkRun(jit, walk, MappedCopy::RunOfStub::Bytes, isRefused);
    jit.branchTest8(CCallHelpers::Zero, Address(copy, OBJECT_OFFSETOF(MappedCopy, hasRunsBehindFourthForStub))).linkTo(isCopied, &jit);
    walkRun(jit, walk, MappedCopy::RunOfStub::Int32sAsDoubles, isRefused);
    walkRun(jit, walk, MappedCopy::RunOfStub::BoxedInt32s, isRefused);
    walkRun(jit, walk, MappedCopy::RunOfStub::WidenedReferences, isRefused);
    jit.jump().linkTo(isCopied, &jit);

    makesNoMoves.link(&jit);
    takesFrame.append(jit.branch32(CCallHelpers::NotEqual, word, Address(copy, OBJECT_OFFSETOF(MappedCopy, structureOfSource))));
    Jump hasCount = jit.jump();
    isRefused.link(&jit);
    jit.move(TrustedImm32(0), count);
    hasCount.link(&jit);
}

struct ChunkCopy {
    GPRReg target;
    GPRReg source;
    GPRReg excludedRun;
    GPRReg count;
    // Holds the StructureID of both.
    GPRReg structure;
    GPRReg word;
    GPRReg orderWord;
};

// Between records of one Structure that hold their fields in plain slots of the cell. It falls through where the copy is made.
static void copyByChunks(StubAssembler& jit, const ChunkCopy& copy, JumpList& callsOperation)
{
    const auto [target, source, excludedRun, count, structure, word, orderWord] = copy;
    const GPRReg layout = structure;
    const GPRReg fieldBytes = structure;
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
    jit.and32(TrustedImm32(static_cast<int32_t>(testedBits.firstWord())), word);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(static_cast<int32_t>(expectedBits.firstWord()))));
    callsOperation.append(branchIfBarrierIsNeeded(jit, target, word));

    constexpr int32_t wordSize = sizeof(uint64_t);
    constexpr int32_t chunkSize = 2 * wordSize;
    static_assert(offsetOfFirstField == chunkSize);
    const GPRReg secondWord = excludedRun;
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
}

PreludeExit emitCopyDataPropertiesPrelude(StubAssembler& jit)
{
    StubFamily family = jit.key().family();
    bool preservesRegisters = family == StubFamily::CallOperationPreservingRegisters;
    if ((family != StubFamily::CallOperation && !preservesRegisters) || jit.signature().result != StubResult::None)
        return PreludeExit::CallsOperation;

    constexpr GPRReg target = operationArgumentGPR(1);
    constexpr GPRReg source = operationArgumentGPR(2);
    constexpr GPRReg excludedRun = operationArgumentGPR(3);
    constexpr GPRReg count = operationArgumentGPR(4);
    constexpr GPRReg excludedKeys = operationArgumentGPR(5);
    constexpr GPRReg word = gprOf(StubRegister::R0);
    constexpr GPRReg orderWord = gprOf(StubRegister::T9);
    constexpr GPRReg structure = gprOf(StubRegister::T12);
    static_assert(noOverlap(target, source, excludedRun, count, excludedKeys, word, orderWord, structure, instanceGPR));
    auto allocateScratches = [&] {
        for (StubRegister scratch : { StubRegister::R0, StubRegister::T9, StubRegister::T12 })
            jit.allocateScratch(scratch);
    };
    JumpList callsOperation;
    callsOperation.append(jit.branchIfNotCell(source));

    // A call that preserves registers may write that of its immediate. With it the Structures are compared and the place of the copy is looked at; what the copy by chunks or by moves writes is saved behind that.
    if (preservesRegisters) {
        const GPRReg immediate = *jit.signature().immediate;
        RELEASE_ASSERT(noOverlap(immediate, target, source, excludedRun, count, word, structure, instanceGPR));
        jit.recordClobbers(RegisterSet(immediate));
        jit.load32(Address(target, JSCell::structureIDOffset()), immediate);
        Jump structuresDiffer = jit.branch32(CCallHelpers::NotEqual, immediate, Address(source, JSCell::structureIDOffset()));
        Vector<GPRReg, 4> saved;
        for (GPRReg written : { word, orderWord, structure }) {
            if (written != immediate)
                saved.append(written);
        }
        if (copyChunkWritesScratches)
            saved.append(excludedRun);
        jit.preserving(saved.span(), [&] {
            allocateScratches();
            JumpList isLeftToOperation;
            jit.move(immediate, structure);
            copyByChunks(jit, { target, source, excludedRun, count, structure, word, orderWord }, isLeftToOperation);
            jit.move(TrustedImm32(0), immediate);
            Jump isCopied = jit.jump();
            isLeftToOperation.link(&jit);
            jit.move(TrustedImm32(1), immediate);
            isCopied.link(&jit);
        });
        Jump isNotCopied = jit.branchTest32(CCallHelpers::NonZero, immediate);
        jit.returnToCaller();

        isNotCopied.link(&jit);
        structuresDiffer.link(&jit);
        callsOperation.append(loadCopyWithoutFrame(jit, target, source, excludedRun, immediate));
        jit.load32(Address(immediate, OBJECT_OFFSETOF(MappedCopy, structureOfSource)), immediate);
        callsOperation.append(jit.branch32(CCallHelpers::NotEqual, immediate, Address(source, JSCell::structureIDOffset())));
        Vector<GPRReg, 6> savedForMoves;
        for (GPRReg written : { word, orderWord, structure, count, excludedKeys, isARM64() ? gprOf(StubRegister::T10) : InvalidGPRReg }) {
            if (written != immediate && written != InvalidGPRReg)
                savedForMoves.append(written);
        }
        constexpr int32_t isMoved = 0;
        constexpr int32_t isLeftToFunctionWithoutFrame = 1;
        constexpr int32_t takesFrame = 2;
        jit.preserving(savedForMoves.span(), [&] {
            JumpList leavesToTakeFrame;
            JumpList leavesWithCopyMade;
            JumpList leaves;
            jit.load32(Address(target, JSCell::structureIDOffset()), structure);
            copyByFieldMap(jit, target, source, excludedRun, count, excludedKeys, structure, word, orderWord, leavesToTakeFrame, [&] {
                leavesWithCopyMade.append(jit.jump());
            });
            jit.move(TrustedImm32(isLeftToFunctionWithoutFrame), immediate);
            leaves.append(jit.jump());
            leavesToTakeFrame.link(&jit);
            jit.move(TrustedImm32(takesFrame), immediate);
            leaves.append(jit.jump());
            leavesWithCopyMade.link(&jit);
            jit.move(TrustedImm32(isMoved), immediate);
            leaves.link(&jit);
        });
        Jump isNotMoved = jit.branchTest32(CCallHelpers::NonZero, immediate);
        jit.returnToCaller();

        isNotMoved.link(&jit);
        callsOperation.append(jit.branch32(CCallHelpers::Equal, immediate, TrustedImm32(takesFrame)));
        jit.jumpToOperationCallOf(Entry::operationAOTCopyDataPropertiesByFieldMap);
        callsOperation.link(&jit);
        return PreludeExit::CallsOperation;
    }

    allocateScratches();
    jit.recordClobbers(RegisterSet(excludedRun, count, excludedKeys));
    jit.load32(Address(target, JSCell::structureIDOffset()), structure);
    Jump structuresDiffer = jit.branch32(CCallHelpers::NotEqual, structure, Address(source, JSCell::structureIDOffset()));
    JumpList isNotCopiedByChunks;
    copyByChunks(jit, { target, source, excludedRun, count, structure, word, orderWord }, isNotCopiedByChunks);
    jit.returnToCaller();

    isNotCopiedByChunks.link(&jit);
    jit.load32(Address(target, JSCell::structureIDOffset()), structure);
    structuresDiffer.link(&jit);
    copyByFieldMap(jit, target, source, excludedRun, count, excludedKeys, structure, word, orderWord, callsOperation, [&] {
        jit.returnToCaller();
    });
    jit.jumpToOperationCallOf(Entry::operationAOTCopyDataPropertiesByFieldMap);
    callsOperation.link(&jit);
    return PreludeExit::CallsOperation;
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
    testedBits.keysAreNeverEnumerated = true;
    testedBits.hasExtensionData = true;
    LayoutData expectedBits { };
    expectedBits.kindBits = static_cast<uint32_t>(LayoutKind::Record);
    jit.loadPtr(Address(structure, CompiledStructure::offsetOfLayout()), layout);
    jit.load32(Address(layout), word);
    jit.and32(TrustedImm32(static_cast<int32_t>(testedBits.firstWord())), word);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(static_cast<int32_t>(expectedBits.firstWord()))));

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
    // What tryCreateObjectViaCloning() asks of a Structure before it gives the copy the same one.
    constexpr uint32_t bitsThatForbid = Structure::s_dictionaryKindBits | Structure::s_hasAnyKindOfGetterSetterPropertiesBits | Structure::s_hasReadOnlyOrGetterSetterPropertiesExcludingProtoBits
        | Structure::s_hasNonEnumerablePropertiesBits | Structure::s_anyImmutablePropertiesBits | Structure::s_mayBePrototypeBits | Structure::s_didPreventExtensionsBits | Structure::s_hasBeenFlattenedBeforeBits
        | Structure::s_hasBeenDictionaryBits | Structure::s_hasUnderscoreProtoPropertyExcludingOriginalProtoBits | Structure::s_hasNonConfigurablePropertiesBits;
    constexpr uint32_t bitsThatAreNeeded = Structure::s_isQuickPropertyAccessAllowedForEnumerationBits | Structure::s_didTransitionBits;
    static_assert(!TypeInfoBlob::indexingModeIncludingHistoryOffset() && sizeof(TypeInfoBlob) == sizeof(uint32_t) && JSCell::indexingTypeAndMiscOffset() == sizeof(uint32_t));
    loadStructure(jit, source, structure);
    jit.load32(Address(structure, Structure::bitFieldOffset()), word);
    jit.and32(TrustedImm32(static_cast<int32_t>(bitsThatForbid | bitsThatAreNeeded)), word);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(static_cast<int32_t>(bitsThatAreNeeded))));
    callsOperation.append(jit.branch8(CCallHelpers::NotEqual, Address(structure, Structure::variantOffset()), TrustedImm32(static_cast<int32_t>(Structure::StructureVariant::Normal))));
    jit.loadPtr(Address(structure, Structure::realmOffset()), word);
    callsOperation.append(jit.branchPtr(CCallHelpers::NotEqual, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject))));
    jit.load64(Address(structure, Structure::prototypeOffset()), word);
    callsOperation.append(jit.branch64(CCallHelpers::NotEqual, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.objectPrototype))));
    jit.load8(Address(structure, Structure::inlineCapacityOffset()), size);
    jit.lshift32(TrustedImm32(3), size);
    jit.add32(TrustedImm32(static_cast<int32_t>(JSFinalObject::allocationSize(0))), size);
    jit.load32(Address(structure, Structure::indexingModeIncludingHistoryOffset()), header);
    jit.lshift64(TrustedImm32(32), header);
    jit.load32(Address(source, JSCell::structureIDOffset()), word);
    jit.or64(word, header);
    allocateAndCopy(false);

    callsOperationWithoutExclusions.link(&jit);
    jit.move(TrustedImm32(0), excludedRun);
    jit.move(TrustedImm32(0), count);
    callsOperation.link(&jit);
    jit.tailCallOperation(Entry::RawCopyObject, OperationCall::FirstArgument::Instance);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
