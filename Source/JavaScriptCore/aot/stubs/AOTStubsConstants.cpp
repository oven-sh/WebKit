/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsConstants.h"

#if ENABLE(AOT)

#include "AOTImageText.h"
#include "AOTInstance.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "JSString.h"
#include "RememberedSlot.h"
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT {

// On x86-64 the immediate and the result share a register.
static StubSignature signatureOfConstant()
{
    StubSignature signature;
    signature.result = StubResult::GPR;
    signature.firstResult = GPRInfo::returnValueGPR;
    signature.immediate = isARM64() ? gprOf(stubImmediateRegister) : GPRInfo::returnValueGPR;
    return signature;
}

static void createConstantAndReturn(StubAssembler& jit, Entry operation, GPRReg number)
{
    jit.callOperationAndReturn(operation, {
        .returns = OperationCall::Returns::Value,
        .firstArgument = OperationCall::FirstArgument::Instance,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
        .secondArgumentIn = number,
    });
}

static void jumpToUnboundStub(StubAssembler& jit)
{
    jit.move(TrustedImm32(*jit.key().immediate()), *jit.signature().immediate);
    jit.jumpToStub(jit.key().withoutImmediate());
}

static void returnCellFromTable(StubAssembler& jit, int32_t offsetOfTable, Entry createOperation)
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

    jit.loadPtr(Address(instanceGPR, offsetOfTable), table);
    jit.zeroExtend32ToWord(number, index);
    jit.load64(BaseIndex(table, index, CCallHelpers::TimesEight), result);
    Jump isNotCreated = jit.branchTest64(CCallHelpers::Zero, result);
    jit.returnToCaller();

    isNotCreated.link(&jit);
    createConstantAndReturn(jit, createOperation, index);
}

StubSignature signatureOfCreateConstant(StubKey)
{
    return signatureOfConstant();
}

#if CPU(X86_64)
// ProgramStrings::createString(), for a string whose StringImpl lies in the image: nothing but the cell is made, and no operation is called.
// headerAndResult holds a StringImpl*, once prepareHeader has run, which may use the three registers that are saved here. What it appends to its list leaves for cannot.
template<typename Functor>
static void createCellOfHeaderAndReturn(StubAssembler& jit, GPRReg headerAndResult, GPRReg scratch, std::optional<GPRReg> number, JumpList& cannot, const Functor& prepareHeader)
{
    const GPRReg wordAndResult = headerAndResult;
    const GPRReg header = headerAndResult;
    const GPRReg cell = gprOf(StubRegister::T10);
    const GPRReg allocator = gprOf(StubRegister::T11);
    const GPRReg allocationScratch = gprOf(StubRegister::T12);
    RELEASE_ASSERT(noOverlap(headerAndResult, scratch, number.value_or(InvalidGPRReg), cell, allocator, allocationScratch, instanceGPR, cellCageBaseGPR));

    jit.preserving({ cell, allocator, allocationScratch }, [&] {
        JumpList cannotAllocate;
        prepareHeader(cannotAllocate);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.stringAllocator)), allocator);
        allocateCell(jit, cell, allocator, allocationScratch, cannotAllocate);
        jit.load64(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(stringHeader)), scratch);
        // JSString::typeInfoBlobForAtomness(): whoever looks for the name of a key need not ask the StringImpl.
        constexpr unsigned shiftOfAtomFlagToCellBit = (JSCell::typeInfoFlagsOffset() * 8 + getLSBSet(static_cast<unsigned>(TypeInfoPerCellBit))) - getLSBSet(StringImpl::flagIsAtom());
        static_assert(hasOneBitSet(StringImpl::flagIsAtom()) && hasOneBitSet(static_cast<unsigned>(TypeInfoPerCellBit)));
        jit.load32(Address(header, StringImpl::flagsOffset()), allocationScratch);
        jit.and32(TrustedImm32(StringImpl::flagIsAtom()), allocationScratch);
        jit.lshift64(TrustedImm32(shiftOfAtomFlagToCellBit), allocationScratch);
        jit.or64(allocationScratch, scratch);
        static_assert(JSString::offsetOfValue() == sizeof(uint64_t) && sizeof(JSString) == 2 * sizeof(uint64_t));
        jit.storePair64(scratch, header, cell, TrustedImm32(0));
        fenceAfterAllocation(jit);
        if (number) {
            static_assert(InstanceLayout::sizeOfStringCell == sizeof(uint32_t));
            encodeCell32(jit, cell, scratch);
            jit.store32(scratch, BaseIndex(instanceGPR, *number, CCallHelpers::TimesFour, InstanceLayout::offsetOfStringCells));
            jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.cardsOfStringCells)), scratch);
            jit.move(*number, allocator);
            jit.urshift32(TrustedImm32(InstanceLayout::stringCellCardShift - 2), allocator);
            jit.store8(TrustedImm32(RememberedSlotSet::writtenCard), BaseIndex(scratch, allocator, CCallHelpers::TimesOne));
        }
        jit.move(cell, wordAndResult);
        Jump isMade = jit.jump();
        cannotAllocate.link(&jit);
        jit.move(TrustedImm32(0), wordAndResult);
        isMade.link(&jit);
    });
    cannot.append(jit.branchTest64(CCallHelpers::Zero, wordAndResult));
    jit.returnToCaller();
}

// wordAndResult holds the ImageStringWord of the string. Where number is given, the cell becomes that of the string constant with the number.
// Where the image has no header for the string the stub makes one: ProgramStrings::tryCreateHeader().
// Leaves for cannot with number as it was: the thread's atom table has to be asked, the VM has a cell for the text, the free list is at its end; of a string without a header also: it is none that gets one, it is long or far, or there is no room for a header.
static void createCellOfImageStringAndReturn(StubAssembler& jit, GPRReg wordAndResult, GPRReg scratch, std::optional<GPRReg> number, JumpList& cannot)
{
    const GPRReg header = wordAndResult;
    const GPRReg cell = gprOf(StubRegister::T10);
    const GPRReg allocator = gprOf(StubRegister::T11);
    const GPRReg allocationScratch = gprOf(StubRegister::T12);
#if !ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    static_assert(sizeof(ImageStringHeader) == 24 && ImageString::hashMask * sizeof(ImageStringHeader) <= INT32_MAX);
#endif

    cannot.append(jit.branchTest32(CCallHelpers::Zero, wordAndResult, TrustedImm32(ImageString::hasTextFlag)));
    // ImageText::createAtom(): the header of an identifier's text is the atom. Any other atom is the one in the table of the thread.
    jit.move(wordAndResult, scratch);
    jit.and32(TrustedImm32(ImageString::isAtomFlag | ImageString::hasIdentifierNumberFlag), scratch);
    cannot.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(ImageString::isAtomFlag)));
    Jump hasNoHeader = jit.branchTest32(CCallHelpers::NonZero, wordAndResult, TrustedImm32(ImageString::hasNoHeaderFlag));
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
    createCellOfHeaderAndReturn(jit, wordAndResult, scratch, number, cannot, [&](JumpList& cannotAllocate) {
        const GPRReg headerNumber = wordAndResult;
        const GPRReg record = cell;
        const GPRReg offset = allocator;
        const GPRReg literalStringWord = offset;
        const GPRReg lazyHeaders = allocationScratch;
        const GPRReg newHeader = lazyHeaders;
        const GPRReg word = scratch;
        constexpr unsigned flagsFor16Bit = StringImpl::hashAndFlagsOfStaticString(0, false, StringImpl::StringNormal);
        constexpr unsigned atomFlags = StringImpl::hashAndFlagsOfStaticString(0, false, StringImpl::StringAtom) ^ flagsFor16Bit;
        static_assert(StringImpl::hashAndFlagsOfStaticString(0, true, StringImpl::StringNormal) == (flagsFor16Bit | StringImpl::flagIs8Bit()));
        static_assert(StringImpl::hashAndFlagsOfStaticString(0, true, StringImpl::StringAtom) == (flagsFor16Bit | atomFlags | StringImpl::flagIs8Bit()) && !(atomFlags & flagsFor16Bit));
        static_assert(sizeof(ImageStringRecord) == sizeof(uint64_t) && !OBJECT_OFFSETOF(ImageStringRecord, rangeOrIndex) && OBJECT_OFFSETOF(ImageStringRecord, hashAndFlags) == sizeof(uint32_t));
        static_assert(OBJECT_OFFSETOF(ImageStringHeader, length) == sizeof(uint32_t) && OBJECT_OFFSETOF(ImageStringHeader, literalStringWord) == OBJECT_OFFSETOF(ImageStringHeader, hashAndFlags) + sizeof(uint32_t));
        jit.and32(TrustedImm32(ImageString::hashMask), headerNumber);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.lazyStringHeaderOffsets)), word);
        jit.load32(BaseIndex(word, headerNumber, CCallHelpers::TimesFour), offset);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.lazyStringHeaders)), lazyHeaders);
        Jump isCreated = jit.branchTest32(CCallHelpers::NonZero, offset);

        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.stringRecords)), record);
        jit.load64(BaseIndex(record, headerNumber, CCallHelpers::TimesEight), record);
        Jump hasInlineRange = jit.branchTest32(CCallHelpers::NonZero, record, TrustedImm32(ImageString::lengthMask));
        cannotAllocate.append(jit.branchTestBit64(CCallHelpers::Zero, record, TrustedImm32(32 + WTF::fastLog2(ImageStringRecord::isLiteralStringFlag))));
        jit.zeroExtend32ToWord(record, word);
        jit.urshift32(TrustedImm32(ImageString::lengthBits), word);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.listedStrings)), literalStringWord);
        jit.load64(BaseIndex(literalStringWord, word, CCallHelpers::TimesFour), literalStringWord);
        jit.urshift64(TrustedImm32(32), record);
        jit.lshift64(TrustedImm32(32), record);
        jit.zeroExtend32ToWord(literalStringWord, word);
        jit.or64(word, record);
        jit.urshift64(TrustedImm32(32), literalStringWord);
        hasInlineRange.link(&jit);
        jit.move(record, word);
        jit.and32(TrustedImm32(ImageString::lengthMask), word);
        cannotAllocate.append(jit.branch32(CCallHelpers::BelowOrEqual, word, TrustedImm32(1)));
        jit.move(TrustedImm32(1), word);
        jit.atomicXchg32(word, Address(lazyHeaders, OBJECT_OFFSETOF(LazyStringHeaderArena, isLocked)));
        cannotAllocate.append(jit.branchTest32(CCallHelpers::NonZero, word));
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
        jit.lshift64(TrustedImm32(32), literalStringWord);
        jit.or64(literalStringWord, word);
        jit.store64(word, Address(newHeader, OBJECT_OFFSETOF(ImageStringHeader, hashAndFlags)));
        jit.store64(headerNumber, Address(newHeader, OBJECT_OFFSETOF(LazyStringHeader, number)));
        jit.move(newHeader, offset);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.lazyStringHeaders)), lazyHeaders);
        jit.sub64(lazyHeaders, offset);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.lazyStringHeaderOffsets)), word);
        jit.store32(offset, BaseIndex(word, headerNumber, CCallHelpers::TimesFour));
        Jump isWritten = jit.jump();
        wasCreatedConcurrently.link(&jit);
        jit.move(word, offset);
        isWritten.link(&jit);
        jit.store32(TrustedImm32(0), Address(lazyHeaders, OBJECT_OFFSETOF(LazyStringHeaderArena, isLocked)));

        isCreated.link(&jit);
        jit.getEffectiveAddress(BaseIndex(lazyHeaders, offset, CCallHelpers::TimesOne), header);
        cannotAllocate.append(jit.branch32(CCallHelpers::BelowOrEqual, Address(header, StringImpl::lengthMemoryOffset()), TrustedImm32(1)));
    });
#else
    jit.and32(TrustedImm32(ImageString::hashMask), wordAndResult);
    jit.mul32(TrustedImm32(sizeof(ImageStringHeader)), wordAndResult, header);
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.programIdentifiers)), header);
    // The empty string and the strings of one character have their cells in the VM. A header that is all zero is no string.
    cannot.append(jit.branch32(CCallHelpers::BelowOrEqual, Address(header, StringImpl::lengthMemoryOffset()), TrustedImm32(1)));
    createCellOfHeaderAndReturn(jit, wordAndResult, scratch, number, cannot, [](JumpList&) { });
#endif

    hasNoHeader.link(&jit);
    createCellOfHeaderAndReturn(jit, wordAndResult, scratch, number, cannot, [&](JumpList& cannotAllocate) {
        const GPRReg characters = scratch;
        const GPRReg newHeader = cell;
        const GPRReg hashAndFlags = allocator;
        const GPRReg length = allocationScratch;
        constexpr unsigned flagsOf16BitText = StringImpl::hashAndFlagsOfStaticString(0, false, StringImpl::StringNormal);
        static_assert(StringImpl::hashAndFlagsOfStaticString(0, true, StringImpl::StringNormal) == (flagsOf16BitText | StringImpl::flagIs8Bit()));
        static_assert(OBJECT_OFFSETOF(ImageStringHeader, length) == sizeof(uint32_t) && OBJECT_OFFSETOF(ImageStringHeader, characters) == sizeof(uint64_t));
        static_assert(OBJECT_OFFSETOF(ImageStringHeader, literalStringWord) == OBJECT_OFFSETOF(ImageStringHeader, hashAndFlags) + sizeof(uint32_t));
        cannotAllocate.append(jit.branchTest32(CCallHelpers::Zero, wordAndResult, TrustedImm32(ImageString::hasListedHashFlag)));
        jit.move(wordAndResult, characters);
        jit.and32(TrustedImm32(ImageString::hashMask), characters);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.listedStrings)), hashAndFlags);
        jit.getEffectiveAddress(BaseIndex(hashAndFlags, characters, CCallHelpers::TimesFour), hashAndFlags);
        jit.load32(Address(hashAndFlags), characters);
        // ImageListedString: the offset above the length, and behind that word the hash. No length: the string is long or far. One character: the VM may have the cell.
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
#endif

static void returnStringConstant(StubAssembler& jit)
{
    const GPRReg number = jit.immediate();
    const GPRReg result = jit.result();
#if CPU(X86_64)
    constexpr GPRReg address = CCallHelpers::s_scratchRegister;
    const GPRReg index = jit.allocateScratch(StubRegister::T9);
#else
    const GPRReg address = jit.allocateScratch(StubRegister::T11);
    const GPRReg index = jit.allocateScratch(StubRegister::T12);
#endif
    RELEASE_ASSERT(noOverlap(result, address, index, instanceGPR, cellCageBaseGPR) && number != address);
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
#if CPU(X86_64)
    if (!jit.options().useWideReferences) {
        JumpList cannot;
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.stringConstants)), address);
        jit.load32(BaseIndex(address, index, CCallHelpers::TimesFour), result);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.stringWords)), address);
        jit.load32(BaseIndex(address, result, CCallHelpers::TimesFour), result);
        createCellOfImageStringAndReturn(jit, result, address, index, cannot);
        cannot.link(&jit);
    }
#endif
    createConstantAndReturn(jit, Entry::operationAOTStringConstant, index);
}

static void returnNewStringConstant(StubAssembler& jit)
{
#if CPU(X86_64)
    const GPRReg string = jit.immediate();
    const GPRReg result = jit.result();
    constexpr GPRReg address = CCallHelpers::s_scratchRegister;
    const GPRReg index = jit.allocateScratch(StubRegister::T9);
    RELEASE_ASSERT(noOverlap(result, address, index, instanceGPR, cellCageBaseGPR) && string != address);
    JumpList cannot;

    jit.zeroExtend32ToWord(string, index);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.stringWords)), address);
    jit.load32(BaseIndex(address, index, CCallHelpers::TimesFour), result);
    createCellOfImageStringAndReturn(jit, result, address, std::nullopt, cannot);
    cannot.link(&jit);
    createConstantAndReturn(jit, Entry::operationAOTNewStringConstant, index);
#else
    createConstantAndReturn(jit, Entry::operationAOTNewStringConstant, jit.immediate());
#endif
}

static void returnKeyConstant(StubAssembler& jit)
{
    if (jit.options().useWideReferences) {
        returnCellFromTable(jit, AOT_INSTANCE_OFFSET(runtime.constants.wideKeyCells), Entry::operationAOTKeyConstant);
        return;
    }
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

    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.constants.keyCells)), table);
    jit.zeroExtend32ToWord(number, index);
    jit.load32(BaseIndex(table, index, CCallHelpers::TimesFour), result);
    Jump isNotCreated = jit.branchTest32(CCallHelpers::Zero, result);
    decodeCell32(jit, result, result);
    jit.returnToCaller();

    isNotCreated.link(&jit);
    createConstantAndReturn(jit, Entry::operationAOTKeyConstant, index);
}

// Behind a miss in the cache of such names, which the operation fills.
static void returnFunctionName(StubAssembler& jit)
{
    const GPRReg index = jit.allocateScratch(StubRegister::T9);
    RELEASE_ASSERT(noOverlap(index, instanceGPR, cellCageBaseGPR));
    jit.zeroExtend32ToWord(jit.immediate(), index);
    jit.or32(TrustedImm32(static_cast<int32_t>(ProgramStrings::isPositionOfFunctionName)), index);
    createConstantAndReturn(jit, Entry::operationAOTNewStringConstant, index);
}

void generateCreateConstant(StubAssembler& jit)
{
    switch (static_cast<CreatedConstant>(jit.key().form())) {
    case CreatedConstant::String:
        returnStringConstant(jit);
        return;
    case CreatedConstant::NewString:
        returnNewStringConstant(jit);
        return;
    case CreatedConstant::Key:
        returnKeyConstant(jit);
        return;
    case CreatedConstant::FunctionName:
        returnFunctionName(jit);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StubSignature signatureOfBuiltinCell(StubKey)
{
    StubSignature signature = signatureOfConstant();
    signature.boundImmediates = StubBoundImmediates::Every;
    return signature;
}

void generateBuiltinCell(StubAssembler& jit)
{
    constexpr int32_t offsetOfTable = AOT_INSTANCE_PREFIX_OFFSET(builtinCells);
    std::optional<uint32_t> index = jit.key().immediate();
    if (!index) {
        returnCellFromTable(jit, offsetOfTable, Entry::operationAOTBuiltinCell);
        return;
    }
    const GPRReg result = jit.result();

    jit.loadPtr(Address(instanceGPR, offsetOfTable), result);
    jit.load64(Address(result, safeCast<int32_t>(*index * sizeof(JSCell*))), result);
    Jump isNotCreated = jit.branchTest64(CCallHelpers::Zero, result);
    jit.returnToCaller();

    isNotCreated.link(&jit);
    jumpToUnboundStub(jit);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
