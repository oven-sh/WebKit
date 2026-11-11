/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsConstants.h"

#if ENABLE(AOT)

#include "AOTImageText.h"
#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTRuntimeWords.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsGlobalBindings.h"
#include "JSGlobalObject.h"
#include "JSString.h"
#include "RememberedSlot.h"
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT {

static StubSignature signatureOfConstant()
{
    StubSignature signature = StubSignature::withGPRResult({ });
    signature.immediate = isARM64() ? gprOf(stubImmediateRegister) : GPRInfo::returnValueGPR;
    return signature;
}

static void createConstantAndReturn(StubAssembler& jit, Entry operation, GPRReg number)
{
    jit.callOperationAndReturn(operation, { .saves = OperationCall::Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = number });
}

struct ConstantRegisters {
    GPRReg number;
    GPRReg result;
    GPRReg table;
    GPRReg index;
};

static ConstantRegisters allocateConstantRegisters(StubAssembler& jit)
{
    const GPRReg number = jit.immediate();
    const GPRReg result = jit.result();
#if CPU(X86_64)
    constexpr GPRReg table = CCallHelpers::s_scratchRegister;
    const GPRReg index = jit.allocateScratch(StubRegister::T9);
#else
    const GPRReg table = jit.allocateScratch(StubRegister::T11);
    const GPRReg index = jit.allocateScratch(StubRegister::T12);
#endif
    RELEASE_ASSERT(noOverlap(result, table, index, instanceGPR, cellCageBaseGPR) && number != table);
    return { number, result, table, index };
}

enum class CellReference : bool { Wide, Narrow };

static void returnCellFromTable(StubAssembler& jit, int32_t offsetOfTable, CellReference reference, Entry createOperation)
{
    const auto [number, result, table, index] = allocateConstantRegisters(jit);
    Jump isNotCreated;

    jit.loadPtr(Address(instanceGPR, offsetOfTable), table);
    jit.zeroExtend32ToWord(number, index);
    if (reference == CellReference::Narrow) {
        jit.load32(BaseIndex(table, index, CCallHelpers::TimesFour), result);
        isNotCreated = jit.branchTest32(CCallHelpers::Zero, result);
        decodeCell32(jit, result, result);
    } else {
        jit.load64(BaseIndex(table, index, CCallHelpers::TimesEight), result);
        isNotCreated = jit.branchTest64(CCallHelpers::Zero, result);
    }
    jit.returnToCaller();

    isNotCreated.link(&jit);
    createConstantAndReturn(jit, createOperation, index);
}

StubSignature signatureOfCreateConstant(StubKey key)
{
    StubSignature signature = signatureOfConstant();
    if (std::optional<GPRReg> numberAndResult = key.operandRegister()) {
        signature.firstResult = *numberAndResult;
        signature.immediate = *numberAndResult;
    }
    return signature;
}

#if CPU(X86_64)
static constexpr StubRegister newCellRegister = StubRegister::T10;
static constexpr StubRegister allocatorRegister = StubRegister::T11;
static constexpr StubRegister allocationScratchRegister = StubRegister::T12;
#else
static constexpr StubRegister newCellRegister = StubRegister::T13;
static constexpr StubRegister allocatorRegister = StubRegister::T14;
static constexpr StubRegister allocationScratchRegister = StubRegister::T15;
#endif

static void lockUnlessLocked(StubAssembler& jit, Address lock, GPRReg scratch, JumpList& isLocked)
{
#if CPU(X86_64)
    jit.move(TrustedImm32(1), scratch);
    jit.atomicXchg32(scratch, lock);
    isLocked.append(jit.branchTest32(CCallHelpers::NonZero, scratch));
#else
    const OwnedScratchRegister ownedScratch(jit);
    const GPRReg one = ownedScratch;
    RELEASE_ASSERT(noOverlap(lock.base, scratch, one));
    Label again = jit.label();
    jit.loadLinkAcq32(lock, scratch);
    isLocked.append(jit.branchTest32(CCallHelpers::NonZero, scratch));
    jit.move(TrustedImm32(1), one);
    jit.storeCond32(one, lock, scratch);
    jit.branchTest32(CCallHelpers::NonZero, scratch).linkTo(again, &jit);
#endif
}

static void storeRelease32(StubAssembler& jit, GPRReg value, BaseIndex address)
{
#if CPU(X86_64)
    jit.store32(value, address);
#else
    RELEASE_ASSERT(noOverlap(value, address.base));
    jit.getEffectiveAddress(address, address.base);
    jit.storeRel32(value, Address(address.base));
#endif
}

static void unlock(StubAssembler& jit, Address lock)
{
#if CPU(X86_64)
    jit.store32(TrustedImm32(0), lock);
#else
    jit.storeRel32(ARM64Registers::zr, lock);
#endif
}

void createLazyHeader(StubAssembler& jit, const LazyHeaderRegisters& r, unsigned longestTextWithoutHeader, JumpList& cannot)
{
    const GPRReg headerNumber = r.number;
    const GPRReg record = r.record;
    const GPRReg offset = r.offset;
    const GPRReg lazyHeaders = r.lazyHeaders;
    const GPRReg newHeader = lazyHeaders;
    const GPRReg word = r.word;
    constexpr unsigned flagsFor16Bit = StringImpl::hashAndFlagsOfStaticString(0, false, StringImpl::StringNormal);
    constexpr unsigned atomFlags = StringImpl::hashAndFlagsOfStaticString(0, false, StringImpl::StringAtom) ^ flagsFor16Bit;
    static_assert(StringImpl::hashAndFlagsOfStaticString(0, true, StringImpl::StringNormal) == (flagsFor16Bit | StringImpl::flagIs8Bit()));
    static_assert(StringImpl::hashAndFlagsOfStaticString(0, true, StringImpl::StringAtom) == (flagsFor16Bit | atomFlags | StringImpl::flagIs8Bit()) && !(atomFlags & flagsFor16Bit));
    static_assert(sizeof(ImageStringRecord) == sizeof(uint64_t) && !OBJECT_OFFSETOF(ImageStringRecord, rangeOrIndex) && OBJECT_OFFSETOF(ImageStringRecord, hashAndFlags) == sizeof(uint32_t));
    static_assert(OBJECT_OFFSETOF(ImageStringHeader, length) == sizeof(uint32_t) && OBJECT_OFFSETOF(ImageStringHeader, unused) == OBJECT_OFFSETOF(ImageStringHeader, hashAndFlags) + sizeof(uint32_t));
    RELEASE_ASSERT(jit.options().isPositionIndependent && noOverlap(headerNumber, record, offset, lazyHeaders, word, instanceGPR));

    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.stringRecords)), record);
    jit.load64(BaseIndex(record, headerNumber, CCallHelpers::TimesEight), record);
    jit.move(record, word);
    jit.and32(TrustedImm32(ImageString::lengthMask), word);
    cannot.append(jit.branch32(CCallHelpers::BelowOrEqual, word, TrustedImm32(longestTextWithoutHeader)));
    lockUnlessLocked(jit, Address(lazyHeaders, OBJECT_OFFSETOF(LazyStringHeaderArena, isLocked)), word, cannot);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.lazyStringHeaderOffsets)), word);
    jit.load32(BaseIndex(word, headerNumber, CCallHelpers::TimesFour), word);
    Jump wasCreatedConcurrently = jit.branchTest32(CCallHelpers::NonZero, word);
    jit.load32(Address(lazyHeaders, OBJECT_OFFSETOF(LazyStringHeaderArena, end)), word);
    jit.add32(TrustedImm32(sizeof(LazyStringHeader)), Address(lazyHeaders, OBJECT_OFFSETOF(LazyStringHeaderArena, end)));
    jit.add64(word, newHeader);
    jit.move(record, word);
    jit.and32(TrustedImm32(ImageString::lengthMask), word);
    jit.lshift64(TrustedImm32(32), word);
    jit.or64(TrustedImm32(StringImpl::refCountOfStaticString()), word);
    jit.store64(word, Address(newHeader));
    jit.zeroExtend32ToWord(record, word);
    jit.urshift32(TrustedImm32(ImageString::lengthBits), word);
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.text)), word);
    jit.store64(word, Address(newHeader, OBJECT_OFFSETOF(ImageStringHeader, characters)));
    jit.urshift64(TrustedImm32(32), record);
    jit.move(record, word);
    jit.lshift32(TrustedImm32(StringImpl::s_flagCount), word);
    jit.or32(TrustedImm32(flagsFor16Bit), word);
    Jump is16Bit = jit.branchTest32(CCallHelpers::NonZero, record, TrustedImm32(ImageString::is16BitFlag));
    jit.or32(TrustedImm32(StringImpl::flagIs8Bit()), word);
    is16Bit.link(&jit);
    Jump isNotAtom = jit.branchTest32(CCallHelpers::Zero, record, TrustedImm32(ImageString::isAtomFlag));
    jit.or32(TrustedImm32(atomFlags), word);
    isNotAtom.link(&jit);
    jit.store64(word, Address(newHeader, OBJECT_OFFSETOF(ImageStringHeader, hashAndFlags)));
    jit.store64(headerNumber, Address(newHeader, OBJECT_OFFSETOF(LazyStringHeader, number)));
    jit.move(newHeader, offset);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(lazyStrings.headers)), lazyHeaders);
    jit.sub64(lazyHeaders, offset);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.lazyStringHeaderOffsets)), word);
    storeRelease32(jit, offset, BaseIndex(word, headerNumber, CCallHelpers::TimesFour));
    Jump isWritten = jit.jump();
    wasCreatedConcurrently.link(&jit);
    jit.move(word, offset);
    isWritten.link(&jit);
    unlock(jit, Address(lazyHeaders, OBJECT_OFFSETOF(LazyStringHeaderArena, isLocked)));
}

enum class AtomnessOfHeader : uint8_t { IsReadFromHeader, IsSaidByWordInScratch };

template<typename Functor>
static void createCellOfHeaderAndReturn(StubAssembler& jit, GPRReg headerAndResult, GPRReg scratch, GPRReg number, JumpList& cannot, const Functor& prepareHeader, AtomnessOfHeader atomness = AtomnessOfHeader::IsReadFromHeader)
{
    const GPRReg wordAndResult = headerAndResult;
    const GPRReg header = headerAndResult;
    const GPRReg cell = gprOf(newCellRegister);
    const GPRReg allocator = gprOf(allocatorRegister);
    const GPRReg allocationScratch = gprOf(allocationScratchRegister);
    RELEASE_ASSERT(noOverlap(headerAndResult, scratch, number, cell, allocator, allocationScratch, instanceGPR, cellCageBaseGPR));

    jit.preserving({ cell, allocator, allocationScratch }, [&] {
        JumpList cannotAllocate;
        prepareHeader(cannotAllocate);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.stringAllocator)), allocator);
        allocateCell(jit, cell, allocator, allocationScratch, cannotAllocate);
        if (atomness == AtomnessOfHeader::IsSaidByWordInScratch)
            jit.move(scratch, allocationScratch);
        jit.load64(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(stringHeader)), scratch);
        constexpr unsigned shiftOfAtomFlagToCellBit = (JSCell::typeInfoFlagsOffset() * 8 + getLSBSet(static_cast<unsigned>(TypeInfoPerCellBit))) - getLSBSet(StringImpl::flagIsAtom());
        static_assert(hasOneBitSet(StringImpl::flagIsAtom()) && hasOneBitSet(static_cast<unsigned>(TypeInfoPerCellBit)));
        if (atomness == AtomnessOfHeader::IsSaidByWordInScratch) {
            constexpr unsigned shiftOfIdentifierFlagToCellBit = (JSCell::typeInfoFlagsOffset() * 8 + getLSBSet(static_cast<unsigned>(TypeInfoPerCellBit))) - getLSBSet(ImageString::hasIdentifierNumberFlag);
            static_assert(hasOneBitSet(ImageString::hasIdentifierNumberFlag));
            jit.and32(TrustedImm32(ImageString::hasIdentifierNumberFlag), allocationScratch);
            jit.lshift64(TrustedImm32(shiftOfIdentifierFlagToCellBit), allocationScratch);
        } else {
            jit.load32(Address(header, StringImpl::flagsOffset()), allocationScratch);
            jit.and32(TrustedImm32(StringImpl::flagIsAtom()), allocationScratch);
            jit.lshift64(TrustedImm32(shiftOfAtomFlagToCellBit), allocationScratch);
        }
        jit.or64(allocationScratch, scratch);
        static_assert(JSString::offsetOfValue() == sizeof(uint64_t) && sizeof(JSString) == 2 * sizeof(uint64_t));
        jit.storePair64(scratch, header, cell, TrustedImm32(0));
        fenceAfterAllocation(jit);
        static_assert(InstanceLayout::sizeOfStringCell == sizeof(uint32_t));
        encodeCell32(jit, cell, scratch);
        jit.store32(scratch, BaseIndex(instanceGPR, number, CCallHelpers::TimesFour, InstanceLayout::offsetOfStringCells));
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.cardsOfStringCells)), scratch);
        jit.move(number, allocator);
        jit.urshift32(TrustedImm32(InstanceLayout::stringCellCardShift - 2), allocator);
        jit.store8(TrustedImm32(RememberedSlotSet::writtenCard), BaseIndex(scratch, allocator, CCallHelpers::TimesOne));
        jit.move(cell, wordAndResult);
        Jump isMade = jit.jump();
        cannotAllocate.link(&jit);
        jit.move(TrustedImm32(0), wordAndResult);
        isMade.link(&jit);
    });
    cannot.append(jit.branchTest64(CCallHelpers::Zero, wordAndResult));
    jit.returnToCaller();
}

static void createCellOfImageStringAndReturn(StubAssembler& jit, GPRReg wordAndResult, GPRReg scratch, GPRReg number, JumpList& cannot)
{
    const GPRReg header = wordAndResult;
    const GPRReg cell = gprOf(newCellRegister);
    const GPRReg allocator = gprOf(allocatorRegister);
    const GPRReg allocationScratch = gprOf(allocationScratchRegister);
    static_assert(sizeof(ImageStringHeader) == 24 && ImageString::hashMask * sizeof(ImageStringHeader) <= INT32_MAX);

    cannot.append(jit.branchTest32(CCallHelpers::Zero, wordAndResult, TrustedImm32(ImageString::hasTextFlag)));
    jit.move(wordAndResult, scratch);
    jit.and32(TrustedImm32(ImageString::isAtomFlag | ImageString::hasIdentifierNumberFlag), scratch);
    cannot.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(ImageString::isAtomFlag)));
    Jump hasNoHeader = jit.branchTest32(CCallHelpers::NonZero, wordAndResult, TrustedImm32(ImageString::hasNoHeaderFlag));
    if (jit.options().isPositionIndependent) {
        createCellOfHeaderAndReturn(jit, wordAndResult, scratch, number, cannot, [&](JumpList& cannotAllocate) {
            const GPRReg headerNumber = wordAndResult;
            const GPRReg offset = allocator;
            const GPRReg lazyHeaders = allocationScratch;
            const GPRReg word = scratch;
            jit.and32(TrustedImm32(ImageString::hashMask), headerNumber);
            jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.lazyStringHeaderOffsets)), word);
            jit.load32(BaseIndex(word, headerNumber, CCallHelpers::TimesFour), offset);
            jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(lazyStrings.headers)), lazyHeaders);
            Jump isCreated = jit.branchTest32(CCallHelpers::NonZero, offset);

            createLazyHeader(jit, { headerNumber, cell, offset, lazyHeaders, word }, 1, cannotAllocate);

            isCreated.link(&jit);
            jit.getEffectiveAddress(BaseIndex(lazyHeaders, offset, CCallHelpers::TimesOne), header);
            cannotAllocate.append(jit.branch32(CCallHelpers::BelowOrEqual, Address(header, StringImpl::lengthMemoryOffset()), TrustedImm32(1)));
        });
    } else {
        cannot.append(jit.branchTest32(CCallHelpers::NonZero, wordAndResult, TrustedImm32(ImageString::isShorterThanTwoFlag)));
        jit.move(wordAndResult, scratch);
        jit.and32(TrustedImm32(ImageString::hashMask), wordAndResult);
        jit.mul32(TrustedImm32(sizeof(ImageStringHeader)), wordAndResult, header);
        jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.programIdentifiers)), header);
        createCellOfHeaderAndReturn(jit, wordAndResult, scratch, number, cannot, [](JumpList&) { }, AtomnessOfHeader::IsSaidByWordInScratch);
    }

    hasNoHeader.link(&jit);
    createCellOfHeaderAndReturn(jit, wordAndResult, scratch, number, cannot, [&](JumpList& cannotAllocate) {
        const GPRReg characters = scratch;
        const GPRReg newHeader = cell;
        const GPRReg hashAndFlags = allocator;
        const GPRReg length = allocationScratch;
        constexpr unsigned flagsOf16BitText = StringImpl::hashAndFlagsOfStaticString(0, false, StringImpl::StringNormal);
        static_assert(StringImpl::hashAndFlagsOfStaticString(0, true, StringImpl::StringNormal) == (flagsOf16BitText | StringImpl::flagIs8Bit()));
        static_assert(OBJECT_OFFSETOF(ImageStringHeader, length) == sizeof(uint32_t) && OBJECT_OFFSETOF(ImageStringHeader, characters) == sizeof(uint64_t));
        static_assert(OBJECT_OFFSETOF(ImageStringHeader, unused) == OBJECT_OFFSETOF(ImageStringHeader, hashAndFlags) + sizeof(uint32_t));
        cannotAllocate.append(jit.branchTest32(CCallHelpers::Zero, wordAndResult, TrustedImm32(ImageString::hasListedHashFlag)));
        jit.move(wordAndResult, characters);
        jit.and32(TrustedImm32(ImageString::hashMask), characters);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.listedStrings)), hashAndFlags);
        jit.getEffectiveAddress(BaseIndex(hashAndFlags, characters, CCallHelpers::TimesFour), hashAndFlags);
        jit.load32(Address(hashAndFlags), characters);
        jit.move(characters, length);
        jit.and32(TrustedImm32(ImageString::lengthMask), length);
        cannotAllocate.append(jit.branch32(CCallHelpers::BelowOrEqual, length, TrustedImm32(1)));
        jit.urshift32(TrustedImm32(ImageString::lengthBits), characters);
        jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.text)), characters);
        jit.load32(Address(hashAndFlags, sizeof(uint32_t)), hashAndFlags);
        jit.lshift32(TrustedImm32(StringImpl::s_flagCount), hashAndFlags);
        jit.or32(TrustedImm32(flagsOf16BitText), hashAndFlags);
        Jump is16Bit = jit.branchTest32(CCallHelpers::NonZero, wordAndResult, TrustedImm32(ImageString::is16BitFlag));
        jit.or32(TrustedImm32(StringImpl::flagIs8Bit()), hashAndFlags);
        is16Bit.link(&jit);
        const GPRReg next = wordAndResult;
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.nextStringHeader)), newHeader);
        jit.addPtr(TrustedImm32(sizeof(ImageStringHeader)), newHeader, next);
        cannotAllocate.append(jit.branchPtr(CCallHelpers::Above, next, Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.endOfStringHeaders))));
        jit.storePtr(next, Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.nextStringHeader)));
        jit.lshift64(TrustedImm32(32), length);
        jit.or64(TrustedImm32(StringImpl::refCountOfStaticString()), length);
        jit.storePair64(length, characters, newHeader, TrustedImm32(0));
        jit.store64(hashAndFlags, Address(newHeader, OBJECT_OFFSETOF(ImageStringHeader, hashAndFlags)));
        jit.move(newHeader, header);
    });
}

static void returnStringConstant(StubAssembler& jit)
{
    const auto [number, result, address, index] = allocateConstantRegisters(jit);
    Jump isNotCreated;

    jit.zeroExtend32ToWord(number, index);
    if (jit.options().useWideReferences) {
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.wideStringCells)), address);
        jit.load64(BaseIndex(address, index, CCallHelpers::TimesEight), result);
        isNotCreated = jit.branchTest64(CCallHelpers::Zero, result);
    } else {
        static_assert(InstanceLayout::sizeOfStringCell == sizeof(uint32_t));
        jit.getEffectiveAddress(BaseIndex(instanceGPR, index, CCallHelpers::TimesFour), address);
        jit.load32(Address(address, InstanceLayout::offsetOfStringCells), result);
        isNotCreated = jit.branchTest32(CCallHelpers::Zero, result);
        decodeCell32(jit, result, result);
    }
    jit.returnToCaller();

    isNotCreated.link(&jit);
    if (!jit.options().useWideReferences) {
        JumpList cannot;
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.stringConstants)), address);
        jit.load32(BaseIndex(address, index, CCallHelpers::TimesFour), result);
        createCellOfImageStringAndReturn(jit, result, address, index, cannot);
        cannot.link(&jit);
    }
    createConstantAndReturn(jit, Entry::operationAOTStringConstant, index);
}

#if CPU(X86_64)
static constexpr StubRegister registersWrittenByMakersOfConstants[] = { StubRegister::R0, StubRegister::T9 };
#else
static constexpr StubRegister registersWrittenByMakersOfConstants[] = { StubRegister::R0, stubImmediateRegister, StubRegister::T11, StubRegister::T12 };
#endif

static void callMakerOfConstantAndReturn(StubAssembler& jit, StubKey maker, GPRReg result, NOESCAPE const Invocable<void(GPRReg number)> auto& emitNumber)
{
    const StubSignature signatureOfMaker = signatureOfConstant();
    Vector<GPRReg, 4> kept;
    for (StubRegister written : registersWrittenByMakersOfConstants) {
        if (gprOf(written) != result)
            kept.append(gprOf(written));
    }

    jit.emitFunctionPrologue();
    jit.preserving(kept.span(), [&] {
        emitNumber(*signatureOfMaker.immediate);
        jit.callStub(maker);
        jit.move(signatureOfMaker.firstResult.gpr(), result);
    });
    jit.emitFunctionEpilogueWithEmptyFrame();
}

static void returnConstantInRegisterOfKey(StubAssembler& jit, CreatedConstant constant)
{
    const GPRReg numberAndResult = jit.result();
    const OwnedScratchRegister ownedScratch(jit);
    const GPRReg word = ownedScratch;
    RELEASE_ASSERT(numberAndResult == jit.immediate() && noOverlap(numberAndResult, word, instanceGPR, cellCageBaseGPR));
    const bool isNarrow = !jit.options().useWideReferences;
    JumpList isNotCreated;

    auto returnCellAt = [&](BaseIndex address) {
        if (isNarrow) {
            jit.load32(address, word);
            isNotCreated.append(jit.branchTest32(CCallHelpers::Zero, word));
            decodeCell32(jit, word, numberAndResult);
        } else {
            jit.load64(address, word);
            isNotCreated.append(jit.branchTest64(CCallHelpers::Zero, word));
            jit.move(word, numberAndResult);
        }
        jit.returnToCaller();
    };
    auto returnCellInTableAt = [&](int32_t offsetOfTable) {
        jit.loadPtr(Address(instanceGPR, offsetOfTable), word);
        returnCellAt(BaseIndex(word, numberAndResult, isNarrow ? CCallHelpers::TimesFour : CCallHelpers::TimesEight));
    };
    switch (constant) {
    case CreatedConstant::String:
        static_assert(InstanceLayout::sizeOfStringCell == sizeof(uint32_t));
        if (isNarrow)
            returnCellAt(BaseIndex(instanceGPR, numberAndResult, CCallHelpers::TimesFour, InstanceLayout::offsetOfStringCells));
        else
            returnCellInTableAt(AOT_INSTANCE_OFFSET(runtime.constants.wideStringCells));
        break;
    case CreatedConstant::Key:
        returnCellInTableAt(isNarrow ? AOT_INSTANCE_OFFSET(runtime.constants.keyCells) : AOT_INSTANCE_OFFSET(runtime.constants.wideKeyCells));
        break;
    }

    isNotCreated.link(&jit);
    callMakerOfConstantAndReturn(jit, createConstant(constant), numberAndResult, [&](GPRReg number) {
        jit.move(numberAndResult, number);
    });
    jit.returnToCaller();
}

void loadStringConstantsAndJump(StubAssembler& jit, std::span<const GPRReg> numbers, StubKey target)
{
    struct LateConstant {
        Jump isNotCreated;
        GPRReg number;
        Label isLoaded;
    };

    const OwnedScratchRegister ownedScratch(jit);
    const GPRReg word = ownedScratch;
    const bool isNarrow = !jit.options().useWideReferences;
    Vector<LateConstant, 2> lateConstants;
    for (GPRReg number : numbers) {
        RELEASE_ASSERT(noOverlap(number, word, instanceGPR, cellCageBaseGPR));
        Jump isNotCreated;
        if (isNarrow) {
            jit.load32(BaseIndex(instanceGPR, number, CCallHelpers::TimesFour, InstanceLayout::offsetOfStringCells), word);
            isNotCreated = jit.branchTest32(CCallHelpers::Zero, word);
            decodeCell32(jit, word, number);
        } else {
            jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.wideStringCells)), word);
            jit.load64(BaseIndex(word, number, CCallHelpers::TimesEight), word);
            isNotCreated = jit.branchTest64(CCallHelpers::Zero, word);
            jit.move(word, number);
        }
        lateConstants.append({ isNotCreated, number, jit.label() });
    }
    jit.jumpToStub(target);

    for (const LateConstant& late : lateConstants) {
        late.isNotCreated.link(&jit);
        callMakerOfConstantAndReturn(jit, createConstant(CreatedConstant::String), late.number, [&](GPRReg number) {
            jit.move(late.number, number);
        });
        jit.jump().linkTo(late.isLoaded, &jit);
    }
}

void generateCreateConstant(StubAssembler& jit)
{
    if (jit.key().operandRegister())
        return returnConstantInRegisterOfKey(jit, static_cast<CreatedConstant>(jit.key().form()));
    switch (static_cast<CreatedConstant>(jit.key().form())) {
    case CreatedConstant::String:
        return returnStringConstant(jit);
    case CreatedConstant::Key:
        if (jit.options().useWideReferences)
            return returnCellFromTable(jit, AOT_INSTANCE_OFFSET(runtime.constants.wideKeyCells), CellReference::Wide, Entry::operationAOTKeyConstant);
        return returnCellFromTable(jit, AOT_INSTANCE_OFFSET(runtime.constants.keyCells), CellReference::Narrow, Entry::operationAOTKeyConstant);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StubSignature signatureOfBuiltinCell(StubKey key)
{
#if CPU(X86_64)
    if (key.immediate())
        return signatureOfConstant().fillingKeptRegisterInFront(X86Registers::ecx);
#else
    UNUSED_PARAM(key);
#endif
    return signatureOfConstant();
}

void generateBuiltinCell(StubAssembler& jit)
{
    constexpr int32_t offsetOfTable = AOT_INSTANCE_PREFIX_OFFSET(builtinCells);
    std::optional<uint32_t> number = jit.key().immediate();
    if (!number) {
        returnCellFromTable(jit, offsetOfTable, CellReference::Wide, Entry::operationAOTBuiltinCell);
        return;
    }
    const GPRReg result = jit.result();

    jit.loadPtr(Address(instanceGPR, offsetOfTable), result);
    jit.load64(Address(result, safeCast<int32_t>(*number * sizeof(JSCell*))), result);
    Jump isNotCreated = jit.branchTest64(CCallHelpers::Zero, result);
    jit.returnToCaller();

    isNotCreated.link(&jit);
    jit.jumpToStubTakingImmediate();
}

StubSignature signatureOfCreateConstantAtSite(StubKey key)
{
    StubSignature signature = StubSignature::withGPRResult({ });
    signature.firstResult = key.operandRegister().value_or(GPRInfo::returnValueGPR);
    return signature;
}

static StubKey makerOfConstantAtSite(ConstantAtSite constant)
{
    switch (constant) {
    case ConstantAtSite::String:
        return createConstant(CreatedConstant::String);
    case ConstantAtSite::Key:
        return createConstant(CreatedConstant::Key);
    case ConstantAtSite::BuiltinCell:
        return builtinCell();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void generateCreateConstantAtSite(StubAssembler& jit)
{
    const auto constant = static_cast<ConstantAtSite>(jit.key().form());
    const GPRReg result = jit.result();

    callMakerOfConstantAndReturn(jit, makerOfConstantAtSite(constant), result, [&](GPRReg number) {
        jit.loadPtr(Address(fp, sizeof(CPURegister)), number);
        jit.load32(Address(number, offsetOfNumberBehindCall), number);
    });
#if CPU(ARM64)
    jit.add64(TrustedImm32(sizeof(uint32_t)), CCallHelpers::linkRegister);
#endif
    jit.returnToCaller();
}

StubSignature signatureOfGetGlobalBinding(StubKey)
{
    return signatureOfConstant();
}

void loadValueAtLocationOfGlobalBinding(StubAssembler& jit, bool seesScriptBindings, GPRReg record, GPRReg global, GPRReg result, JumpList& cannot)
{
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), global);
    jit.load32(Address(global, JSCell::structureIDOffset()), result);
    cannot.append(jit.branch32(CCallHelpers::NotEqual, result, Address(record, OBJECT_OFFSETOF(GlobalBinding, structureID))));
    if (seesScriptBindings) {
        jit.load32(Address(global, JSGlobalObject::globalLexicalBindingEpochOffset()), result);
        cannot.append(jit.branch32(CCallHelpers::NotEqual, result, Address(record, OBJECT_OFFSETOF(GlobalBinding, lexicalBindingEpoch))));
    }
    jit.load64(Address(record, OBJECT_OFFSETOF(GlobalBinding, location)), record);
    Jump isAddress = jit.branch64(CCallHelpers::GreaterThanOrEqual, record, TrustedImm32(0));
    jit.loadPtr(Address(global, JSObject::butterflyOffset()), global);
    jit.add64(global, record);
    isAddress.link(&jit);
    jit.load64(Address(record), result);
    cannot.append(jit.branchTest64(CCallHelpers::Zero, result));
}

void generateGetGlobalBinding(StubAssembler& jit)
{
    const auto scope = static_cast<GlobalBindingScope>(jit.key().form());
    const GPRReg result = jit.result();
#if CPU(X86_64)
    const GPRReg number = jit.allocateScratch(stubImmediateRegister);
    jit.zeroExtend32ToWord(jit.immediate(), number);
#else
    const GPRReg number = jit.immediate();
#endif
    const GPRReg record = jit.allocateScratch(StubRegister::T10);
    const GPRReg global = jit.allocateScratch(StubRegister::T11);
    JumpList callsOperation;

    static_assert(sizeof(GlobalBinding) == 3 * sizeof(uint64_t));
    jit.getEffectiveAddress(BaseIndex(number, number, CCallHelpers::TimesTwo), record);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.bindings.globalBindings)), result);
    jit.getEffectiveAddress(BaseIndex(result, record, CCallHelpers::TimesEight), record);
    jit.load64(Address(record, OBJECT_OFFSETOF(GlobalBinding, constant)), result);
    Jump isNoConstant = jit.branchTest64(CCallHelpers::Zero, result);
    jit.returnToCaller();

    isNoConstant.link(&jit);
    loadValueAtLocationOfGlobalBinding(jit, scope == GlobalBindingScope::ScriptsAndGlobalObject, record, global, result, callsOperation);
    jit.returnToCaller();

    callsOperation.link(&jit);
    Entry operation = scope == GlobalBindingScope::ScriptsAndGlobalObject ? Entry::operationAOTGetGlobalBinding : Entry::operationAOTGetGlobalObjectBinding;
    jit.callOperationAndReturn(operation, { .saves = OperationCall::Saves::CallerSavedRegisters, .secondArgumentIn = number });
}

StubSignature signatureOfSetGlobalBinding(StubKey)
{
    StubSignature signature = StubSignature::withoutResult({ gprOf(StubRegister::A2) });
    signature.immediate = gprOf(StubRegister::A1);
    return signature;
}

void generateSetGlobalBinding(StubAssembler& jit)
{
    static constexpr Entry operations[] = { Entry::operationAOTSetGlobalBinding, Entry::operationAOTSetGlobalBindingSloppy, Entry::operationAOTSetGlobalObjectBinding, Entry::operationAOTSetGlobalObjectBindingSloppy };
    static_assert(setGlobalBinding(GlobalBindingScope::ScriptsAndGlobalObject, true).form() == 0 && setGlobalBinding(GlobalBindingScope::GlobalObject, false).form() == 3);
    const auto scope = static_cast<GlobalBindingScope>(jit.key().form() / 2);
    const GPRReg value = jit.operand(0);
    const GPRReg number = jit.immediate();
    const GPRReg record = jit.allocateScratch(StubRegister::T10);
    const GPRReg word = jit.allocateScratch(StubRegister::T11);
    const GPRReg global = jit.allocateScratch(StubRegister::T14);
    RELEASE_ASSERT(noOverlap(value, number, record, word, global, instanceGPR));
    JumpList callsOperation;

    static_assert(sizeof(GlobalBinding) == 3 * sizeof(uint64_t));
    jit.getEffectiveAddress(BaseIndex(number, number, CCallHelpers::TimesTwo), record);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.bindings.globalBindings)), word);
    jit.getEffectiveAddress(BaseIndex(word, record, CCallHelpers::TimesEight), record);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), global);
    jit.load32(Address(global, JSCell::structureIDOffset()), word);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, Address(record, OBJECT_OFFSETOF(GlobalBinding, structureID))));
    if (scope == GlobalBindingScope::ScriptsAndGlobalObject) {
        jit.load32(Address(global, JSGlobalObject::globalLexicalBindingEpochOffset()), word);
        callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, Address(record, OBJECT_OFFSETOF(GlobalBinding, lexicalBindingEpoch))));
    }
    jit.load64(Address(record, OBJECT_OFFSETOF(GlobalBinding, location)), record);
    Jump isAddress = jit.branch64(CCallHelpers::GreaterThanOrEqual, record, TrustedImm32(0));
    jit.loadPtr(Address(global, JSObject::butterflyOffset()), word);
    jit.add64(word, record);
    isAddress.link(&jit);
    jit.store64(value, Address(record));
    returnAfterStoring(jit, global, value);

    callsOperation.link(&jit);
    jit.callOperationAndReturn(operations[jit.key().form()], { .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters, .secondArgumentIn = number, .thirdArgumentIn = value });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
