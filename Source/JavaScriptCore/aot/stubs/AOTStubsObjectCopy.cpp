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
#include "AOTObjectWords.h"
#include "AOTRuntimeTable.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsField.h"
#include "AOTStubsOperations.h"
#include "AOTStubsPropertyExtraProperties.h"
#include "AOTVector.h"
#include "Allocator.h"
#include "CompleteSubspace.h"
#include "JSCell.h"
#include "JSObject.h"
#include "MarkedBlockCage.h"
#include "MarkedSpace.h"
#include "Structure.h"
#include <bit>

namespace JSC { namespace AOT {

static void copyChunkAtOffset(StubAssembler& jit, GPRReg source, GPRReg target, GPRReg offset, GPRReg firstScratch, GPRReg secondScratch)
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

StubSignature signatureOfCopyObjectFrontEnd(StubKey)
{
    return StubSignature::withGPRResult({ operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3), operationArgumentGPR(4) });
}

StubSignature signatureOfAssignToNewObjectFrontEnd(StubKey key)
{
    return signatureOfCopyObjectFrontEnd(key);
}

enum class ObjectIsCopiedBy : bool { Definitions, Stores };

static void generateFrontEndOfObjectCopy(StubAssembler& jit, ObjectIsCopiedBy copiedBy)
{
    const GPRReg source = jit.operand(0);
    const GPRReg excludedRun = jit.clobberOperand(1);
    const GPRReg count = jit.clobberOperand(2);
    const GPRReg kept = jit.clobberOperand(3);
    const GPRReg copy = jit.result();
    const GPRReg word = jit.allocateScratch(StubRegister::T9);
    const GPRReg size = jit.allocateScratch(StubRegister::T11);
    const GPRReg structure = jit.allocateScratch(StubRegister::T12);
    const GPRReg secondScratchOfChunk = isARM64() ? jit.allocateScratch(StubRegister::T10) : InvalidGPRReg;
    const GPRReg allocator = structure;
    const GPRReg header = excludedRun;
    const GPRReg shapeWord = count;
    RELEASE_ASSERT(noOverlap(source, excludedRun, count, kept, copy, word, size, structure, instanceGPR));
    RELEASE_ASSERT(secondScratchOfChunk == InvalidGPRReg || noOverlap(secondScratchOfChunk, source, excludedRun, count, kept, copy, word, size, structure, instanceGPR));
    JumpList callsOperation;
    JumpList callsOperationWithoutExclusions;

    constexpr int32_t wordSize = sizeof(uint64_t);
    constexpr int32_t chunkSize = 2 * wordSize;
    static_assert(offsetOfFirstField == chunkSize && JSFinalObject::offsetOfInlineStorage() == chunkSize && MarkedSpace::sizeStep == chunkSize);
    static_assert(JSCell::cellStateOffset() == wordSize - 1 && !JSCell::structureIDOffset() && std::endian::native == std::endian::little);

    auto allocateAndCopy = [&](bool isRecord, const Invocable<void()> auto& finish) {
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
        copyChunkAtOffset(jit, source, copy, size, word, allocator);
        jit.branch32(CCallHelpers::Above, size, TrustedImm32(chunkSize)).linkTo(copyNextChunk, &jit);
        isCopied.link(&jit);
        jit.load64(Address(source, wordSize), word);
        jit.store64(word, Address(copy, wordSize));
        if (isRecord)
            jit.store32(shapeWord, Address(copy, offsetOfShapeWord));
        jit.store64(header, Address(copy));
        finish();
        fenceAfterAllocation(jit);
        jit.returnToCaller();
    };
    auto finishNothing = [] { };

    auto copyStorageOutsideCell = [&](GPRReg object) {
        const GPRReg named = word;
        const GPRReg storage = count;
        const GPRReg scratch = structure;
        JumpList& cannot = callsOperationWithoutExclusions;
        constexpr int32_t maxCapacityCopied = static_cast<int32_t>(std::bit_floor(MarkedSpace::largeCutoff / sizeof(EncodedJSValue)));
        static_assert(outOfLineGrowthFactor == 2 && MarkedSpace::sizeStep == 2 * sizeof(EncodedJSValue) && sizeof(Allocator) == sizeof(void*) && sizeof(IndexingHeader) == sizeof(EncodedJSValue));
        static_assert(Structure::greatestMaxOffsetOutsideRareData >= firstOutOfLineOffset + maxCapacityCopied && !(initialOutOfLineCapacity % 2));
        jit.loadPtr(Address(object, JSObject::butterflyOffset()), storage);
        jit.load16(Address(named, Structure::maxOffsetOffset()), size);
        jit.move(TrustedImm32(0), kept);
        Jump hasStorage = jit.branch32(CCallHelpers::AboveOrEqual, size, TrustedImm32(firstOutOfLineOffset));
        cannot.append(jit.branchTestPtr(CCallHelpers::NonZero, storage));
        Jump hasNoStorage = jit.jump();
        Label hasDoubledMoreThanTwice = jit.label();
        cannot.append(jit.branch32(CCallHelpers::AboveOrEqual, size, TrustedImm32(firstOutOfLineOffset + maxCapacityCopied)));
        cannot.append(jit.branchTestPtr(CCallHelpers::Zero, storage));
        jit.sub32(TrustedImm32(firstOutOfLineOffset), size);
        jit.lshift32(TrustedImm32(3), size);
        jit.move(TrustedImm32(4 * initialOutOfLineCapacity * sizeof(EncodedJSValue)), kept);
        Label growStorage = jit.label();
        jit.lshift32(TrustedImm32(1), kept);
        jit.branch32(CCallHelpers::AboveOrEqual, size, kept).linkTo(growStorage, &jit);
        JumpList hasBytes;
        hasBytes.append(jit.jump());
        hasStorage.link(&jit);
        jit.branch32(CCallHelpers::AboveOrEqual, size, TrustedImm32(firstOutOfLineOffset + 4 * initialOutOfLineCapacity)).linkTo(hasDoubledMoreThanTwice, &jit);
        cannot.append(jit.branchTestPtr(CCallHelpers::Zero, storage));
        jit.sub32(TrustedImm32(firstOutOfLineOffset), size);
        jit.move(TrustedImm32(initialOutOfLineCapacity * sizeof(EncodedJSValue)), kept);
        hasBytes.append(jit.branch32(CCallHelpers::Below, size, TrustedImm32(initialOutOfLineCapacity)));
        jit.lshift32(TrustedImm32(1), kept);
        hasBytes.append(jit.branch32(CCallHelpers::Below, size, TrustedImm32(2 * initialOutOfLineCapacity)));
        jit.lshift32(TrustedImm32(1), kept);
        hasBytes.link(&jit);
        jit.move(kept, size);
        jit.urshift32(TrustedImm32(1), kept);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), scratch);
        jit.loadPtr(BaseIndex(scratch, kept, CCallHelpers::TimesOne, CompleteSubspace::offsetOfAllocatorForSizeStep()), scratch);
        cannot.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
        allocateCell(jit, copy, scratch, kept, cannot);
        jit.subPtr(size, storage);
        jit.subPtr(TrustedImm32(sizeof(IndexingHeader)), storage);
        jit.getEffectiveAddress(BaseIndex(copy, size, CCallHelpers::TimesOne, sizeof(IndexingHeader)), kept);
        Label copyNextChunk = jit.label();
        jit.sub32(TrustedImm32(chunkSize), size);
        copyChunkAtOffset(jit, storage, copy, size, scratch, secondScratchOfChunk);
        jit.branchTest32(CCallHelpers::NonZero, size).linkTo(copyNextChunk, &jit);
        hasNoStorage.link(&jit);
    };

    auto loadWordsOfRecord = [&] {
        jit.load32(Address(size, OBJECT_OFFSETOF(LayoutData, shape)), shapeWord);
        jit.neg32(shapeWord);
        jit.load32(Address(size, OBJECT_OFFSETOF(LayoutData, size)), size);
        jit.load64(Address(source), header);
        jit.lshift64(TrustedImm32(8), header);
        jit.urshift64(TrustedImm32(8), header);
        jit.move(TrustedImm64(static_cast<int64_t>(static_cast<uint64_t>(CellState::DefinitelyWhite) << 56)), word);
        jit.or64(word, header);
    };

    if (copiedBy == ObjectIsCopiedBy::Stores) {
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.objectPrototype)), structure);
        loadStructure(jit, structure, structure);
        callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_hasReadOnlyOrGetterSetterPropertiesExcludingProtoBits)));
    }
    Jump isNotCell = jit.branchIfNotCell(source);
    jit.move(count, word);
    callsOperation.append(jit.branchOr32(CCallHelpers::NonZero, excludedRun, word));
    JumpList isNotCompiledObject = branchIfNotCompiledObjectOfThisImage(jit, source);
    callsOperation.append(branchIfCellBearsMark(jit, source));

    static_assert(CompiledStructure::offsetOfHeldKeyOrder() == CompiledStructure::offsetOfFlags() + 1);
    constexpr int32_t keyOrderStaysBehind = 0xff00;
    constexpr int32_t isCopiedByWords = static_cast<uint8_t>(CompiledStructure::Flag::IsCopiedByWords);
    constexpr int32_t hasExtraProperties = static_cast<uint8_t>(CompiledStructure::Flag::HasExtraProperties);
    constexpr int32_t hasOnlyFieldsBelowObjectPrototype = static_cast<uint8_t>(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype);
    Jump hasOtherBits = jit.branchTest32(CCallHelpers::NonZero, Address(source, offsetOfOrderWord), TrustedImm32(~orderAndCountMask));
    loadStructure(jit, source, structure);
    jit.load16(Address(structure, CompiledStructure::offsetOfFlags()), word);
    jit.and32(TrustedImm32(isCopiedByWords | hasOnlyFieldsBelowObjectPrototype | hasExtraProperties | keyOrderStaysBehind), word);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(isCopiedByWords | hasOnlyFieldsBelowObjectPrototype)));

    LayoutData testedBits { };
    testedBits.kindBits = (1 << 4) - 1;
    testedBits.hasExtensionData = true;
    LayoutData bitsOfRecord { };
    bitsOfRecord.kindBits = static_cast<uint32_t>(LayoutKind::Record);
    LayoutData bitsOfRecordWithExtension = bitsOfRecord;
    bitsOfRecordWithExtension.hasExtensionData = true;
    jit.loadPtr(Address(structure, CompiledStructure::offsetOfLayout()), size);
    jit.load32(Address(size), word);
    jit.and32(TrustedImm32(static_cast<int32_t>(testedBits.firstWord())), word);
    Jump hasOtherLayout = jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(static_cast<int32_t>(bitsOfRecord.firstWord())));
    callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));
    jit.loadPtr(Address(structure, Structure::realmOffset()), word);
    callsOperation.append(jit.branchPtr(CCallHelpers::NotEqual, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject))));

    loadWordsOfRecord();
    allocateAndCopy(true, finishNothing);

    isNotCompiledObject.link(&jit);
    callsOperation.append(jit.branch8(CCallHelpers::NotEqual, Address(source, JSCell::typeInfoTypeOffset()), TrustedImm32(FinalObjectType)));
    Jump hasButterfly = jit.branchTestPtr(CCallHelpers::NonZero, Address(source, JSObject::butterflyOffset()));
    constexpr uint32_t bitsThatForbid = Structure::s_dictionaryKindBits | Structure::s_hasAnyKindOfGetterSetterPropertiesBits | Structure::s_hasReadOnlyOrGetterSetterPropertiesExcludingProtoBits
        | Structure::s_hasNonEnumerablePropertiesBits | Structure::s_anyImmutablePropertiesBits | Structure::s_mayBePrototypeBits | Structure::s_didPreventExtensionsBits | Structure::s_hasBeenFlattenedBeforeBits
        | Structure::s_hasBeenDictionaryBits | Structure::s_hasUnderscoreProtoPropertyExcludingOriginalProtoBits | Structure::s_hasNonConfigurablePropertiesBits;
    constexpr uint32_t bitsThatAreNeeded = Structure::s_isQuickPropertyAccessAllowedForEnumerationBits | Structure::s_didTransitionBits;
    static_assert(!TypeInfoBlob::indexingModeIncludingHistoryOffset() && sizeof(TypeInfoBlob) == sizeof(uint32_t) && JSCell::indexingTypeAndMiscOffset() == sizeof(uint32_t));
    auto testStructureOfOrdinaryObject = [&](JumpList& hasOtherBitsOfStructure) {
        loadStructure(jit, source, structure);
        jit.load32(Address(structure, Structure::bitFieldOffset()), word);
        jit.and32(TrustedImm32(static_cast<int32_t>(bitsThatForbid | bitsThatAreNeeded)), word);
        hasOtherBitsOfStructure.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(static_cast<int32_t>(bitsThatAreNeeded))));
        Label hasTestedBits = jit.label();
        callsOperation.append(jit.branch8(CCallHelpers::NotEqual, Address(structure, Structure::variantOffset()), TrustedImm32(static_cast<int32_t>(Structure::StructureVariant::Normal))));
        jit.loadPtr(Address(structure, Structure::realmOffset()), word);
        callsOperation.append(jit.branchPtr(CCallHelpers::NotEqual, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject))));
        jit.load64(Address(structure, Structure::prototypeOffset()), word);
        callsOperation.append(jit.branch64(CCallHelpers::NotEqual, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.objectPrototype))));
        return hasTestedBits;
    };
    auto loadWordsOfOrdinaryObject = [&] {
        jit.load8(Address(structure, Structure::inlineCapacityOffset()), size);
        jit.lshift32(TrustedImm32(3), size);
        jit.add32(TrustedImm32(static_cast<int32_t>(JSFinalObject::allocationSize(0))), size);
        jit.load32(Address(structure, Structure::indexingModeIncludingHistoryOffset()), header);
        jit.lshift64(TrustedImm32(32), header);
        jit.load32(Address(source, JSCell::structureIDOffset()), word);
        jit.or64(word, header);
    };
    JumpList mayBeEmpty;
    Label hasTestedBits = testStructureOfOrdinaryObject(mayBeEmpty);
    loadWordsOfOrdinaryObject();
    allocateAndCopy(false, finishNothing);

    mayBeEmpty.link(&jit);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(static_cast<int32_t>(bitsThatAreNeeded & ~Structure::s_didTransitionBits))));
    jit.branchTestPtr(CCallHelpers::Zero, Address(structure, Structure::seenPropertiesOffset())).linkTo(hasTestedBits, &jit);
    callsOperation.append(jit.jump());

    hasButterfly.link(&jit);
    testStructureOfOrdinaryObject(callsOperation);
    callsOperation.append(jit.branchTest8(CCallHelpers::NonZero, Address(source, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(IndexingShapeMask)));
    jit.move(structure, word);
    copyStorageOutsideCell(source);
    loadStructure(jit, source, structure);
    loadWordsOfOrdinaryObject();
    allocateAndCopy(false, [&] {
        jit.storePtr(kept, Address(copy, JSObject::butterflyOffset()));
    });

    // A cell is whole before the next allocation can fail: the collector may find what is left behind.
    hasOtherLayout.link(&jit);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(static_cast<int32_t>(bitsOfRecordWithExtension.firstWord()))));
    hasOtherBits.link(&jit);
    {
        const bool isNarrow = !jit.options().useWideReferences;
        const GPRReg extraProperties = excludedRun;
        const GPRReg storage = count;
        const GPRReg named = word;
        const GPRReg scratch = structure;
        JumpList& cannot = callsOperationWithoutExclusions;
        callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, Address(source, offsetOfOrderWord), TrustedImm32(~(orderAndCountMask | orderWordHasExtraProperties))));
        loadStructure(jit, source, structure);
        constexpr int32_t otherFlags = static_cast<uint8_t>(CompiledStructure::Flag::HasOtherPrototype) | keyOrderStaysBehind;
        jit.load16(Address(structure, CompiledStructure::offsetOfFlags()), word);
        callsOperation.append(jit.branchTest32(CCallHelpers::Zero, word, TrustedImm32(hasExtraProperties | hasOnlyFieldsBelowObjectPrototype)));
        jit.and32(TrustedImm32(isCopiedByWords | otherFlags), word);
        callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(isCopiedByWords)));
        callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));
        jit.loadPtr(Address(structure, Structure::realmOffset()), word);
        callsOperation.append(jit.branchPtr(CCallHelpers::NotEqual, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject))));
        LayoutData bitsOfAnyRecord = testedBits;
        bitsOfAnyRecord.hasExtensionData = false;
        jit.loadPtr(Address(structure, CompiledStructure::offsetOfLayout()), word);
        jit.load32(Address(word), word);
        jit.and32(TrustedImm32(static_cast<int32_t>(bitsOfAnyRecord.firstWord())), word);
        callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(static_cast<int32_t>(bitsOfRecord.firstWord()))));
        jit.move(TrustedImm32(0), kept);
        Jump hasNoExtraProperties = jit.branchTest32(CCallHelpers::Zero, Address(source, offsetOfOrderWord), TrustedImm32(orderWordHasExtraProperties));
        jit.load32(Address(structure, CompiledStructure::offsetOfExtraPropertiesSlot()), word);
        jit.and32(TrustedImm32(SlotData::maxOffset), word);
        callsOperation.append(jit.branchTest32(CCallHelpers::Zero, word));

        loadExtraProperties(jit, source, word, extraProperties, cannot);
        jit.load32(Address(extraProperties, JSCell::structureIDOffset()), named);
        cannot.append(jit.branch32(CCallHelpers::NotEqual, named, Address(structure, CompiledStructure::offsetOfExtraPropertiesStructureID())));
        decodeStructureID(jit, named);
        jit.load32(Address(named, Structure::bitFieldOffset()), scratch);
        jit.and32(TrustedImm32(static_cast<int32_t>(bitsThatForbid | bitsThatAreNeeded)), scratch);
        cannot.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(static_cast<int32_t>(bitsThatAreNeeded))));
        cannot.append(jit.branch8(CCallHelpers::NotEqual, Address(named, Structure::variantOffset()), TrustedImm32(static_cast<int32_t>(Structure::StructureVariant::Normal))));
        cannot.append(jit.branchTest8(CCallHelpers::NonZero, Address(extraProperties, JSCell::indexingTypeAndMiscOffset()), TrustedImm32(IndexingShapeMask)));

        copyStorageOutsideCell(extraProperties);

        loadStructure(jit, source, scratch);
        jit.load16(Address(scratch, CompiledStructure::offsetOfExtraPropertiesCellSize()), size);
        jit.urshift32(size, TrustedImm32(1), storage);
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), scratch);
        jit.loadPtr(BaseIndex(scratch, storage, CCallHelpers::TimesOne, CompleteSubspace::offsetOfAllocatorForSizeStep()), scratch);
        cannot.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
        allocateCell(jit, copy, scratch, storage, cannot);
        Jump hasNoInlineStorage = jit.branch32(CCallHelpers::BelowOrEqual, size, TrustedImm32(chunkSize));
        Label copyNextChunkOfCell = jit.label();
        jit.sub32(TrustedImm32(chunkSize), size);
        copyChunkAtOffset(jit, extraProperties, copy, size, scratch, storage);
        jit.branch32(CCallHelpers::Above, size, TrustedImm32(chunkSize)).linkTo(copyNextChunkOfCell, &jit);
        hasNoInlineStorage.link(&jit);
        jit.load32(Address(named, Structure::indexingModeIncludingHistoryOffset()), scratch);
        jit.lshift64(TrustedImm32(32), scratch);
        jit.load32(Address(extraProperties, JSCell::structureIDOffset()), storage);
        jit.or64(storage, scratch);
        jit.storePtr(kept, Address(copy, JSObject::butterflyOffset()));
        jit.store64(scratch, Address(copy));
        if (isNarrow) {
            encodeCell32(jit, copy, kept);
            jit.zeroExtend32ToWord(kept, kept);
        } else
            jit.move(copy, kept);
        hasNoExtraProperties.link(&jit);

        {
            const GPRReg from = word;
            const GPRReg units = size;
            const GPRReg allocatorOfExtension = excludedRun;
            loadExtensionWordOffset(jit, source, from);
            Jump hasNoSlot = jit.branchTest32(CCallHelpers::Zero, from);
            if (!isNarrow)
                cannot.append(jit.branchTest64(CCallHelpers::NonZero, BaseIndex(source, from, CCallHelpers::TimesOne)));
            else {
                jit.load32(BaseIndex(source, from, CCallHelpers::TimesOne), from);
                Jump hasNone = jit.branchTest32(CCallHelpers::Zero, from);
                decodeCell32(jit, from, from);
                jit.addPtr(TrustedImm32(offsetOfEmptyExtensionInCage), from);
                jit.load64(Address(from, offsetOfExtensionGranuleBits), allocatorOfExtension);
                jit.countPopulation64(allocatorOfExtension, scratch, CCallHelpers::fpTempRegister);
                cannot.append(jit.branch32(CCallHelpers::Above, scratch, TrustedImm32(maxGranulesOfExtensionFromStub)));
                loadUnitsOfExtensionWith(jit, allocatorOfExtension, units, scratch);
                jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cagedAuxiliarySpace)), allocatorOfExtension);
                jit.loadPtr(BaseIndex(allocatorOfExtension, units, CCallHelpers::ScalePtr, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocatorOfExtension);
                cannot.append(jit.branchTestPtr(CCallHelpers::Zero, allocatorOfExtension));
                allocateCell(jit, copy, allocatorOfExtension, scratch, cannot);
                static_assert(minExtensionSize == chunkSize);
                jit.lshift32(TrustedImm32(4), units);
                Label copyNextChunkOfExtension = jit.label();
                jit.sub32(TrustedImm32(chunkSize), units);
                copyChunkAtOffset(jit, from, copy, units, allocatorOfExtension, scratch);
                jit.branchTest32(CCallHelpers::NonZero, units).linkTo(copyNextChunkOfExtension, &jit);
                encodeCell32(jit, copy, copy);
                jit.sub32(TrustedImm32(offsetOfEmptyExtensionInCage >> cell32Shift), copy);
                jit.lshift64(TrustedImm32(32), copy);
                jit.or64(copy, kept);
                hasNone.link(&jit);
            }
            hasNoSlot.link(&jit);
        }

        loadStructure(jit, source, structure);
        jit.loadPtr(Address(structure, CompiledStructure::offsetOfLayout()), size);
        loadWordsOfRecord();
        allocateAndCopy(true, [&] {
            loadStructure(jit, source, word);
            jit.load32(Address(word, CompiledStructure::offsetOfExtraPropertiesSlot()), size);
            jit.and32(TrustedImm32(SlotData::maxOffset), size);
            if (!isNarrow) {
                Jump hasNone = jit.branchTest64(CCallHelpers::Zero, kept);
                jit.store64(kept, BaseIndex(copy, size, CCallHelpers::TimesOne));
                hasNone.link(&jit);
                return;
            }
            Jump hasNone = jit.branchTest32(CCallHelpers::Zero, kept);
            jit.store32(kept, BaseIndex(copy, size, CCallHelpers::TimesOne));
            hasNone.link(&jit);
            jit.urshift64(TrustedImm32(32), kept);
            Jump hasNoExtension = jit.branchTest32(CCallHelpers::Zero, kept);
            jit.load32(Address(word, CompiledStructure::offsetOfExtensionSlot()), size);
            jit.and32(TrustedImm32(SlotData::maxOffset), size);
            jit.store32(kept, BaseIndex(copy, size, CCallHelpers::TimesOne));
            hasNoExtension.link(&jit);
        });
    }

    isNotCell.link(&jit);
    constexpr unsigned kind = static_cast<unsigned>(InlineAllocation::DefaultFinalObject);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].allocator)), allocator);
    callsOperation.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, copy, allocator, word, callsOperation);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].header)), word);
    jit.store64(word, Address(copy));
    for (size_t offset = wordSize; offset < JSFinalObject::allocationSize(JSFinalObject::defaultInlineCapacity); offset += wordSize)
        jit.store64(TrustedImm32(0), Address(copy, offset));
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    callsOperationWithoutExclusions.link(&jit);
    jit.move(TrustedImm32(0), excludedRun);
    jit.move(TrustedImm32(0), count);
    callsOperation.link(&jit);
    jit.jumpToCallOfOperation(copiedBy == ObjectIsCopiedBy::Stores ? Entry::operationAOTAssignToNewObject : Entry::operationAOTCopyObject);
}

void generateCopyObjectFrontEnd(StubAssembler& jit)
{
    generateFrontEndOfObjectCopy(jit, ObjectIsCopiedBy::Definitions);
}

void generateAssignToNewObjectFrontEnd(StubAssembler& jit)
{
    generateFrontEndOfObjectCopy(jit, ObjectIsCopiedBy::Stores);
}

StubSignature signatureOfCopyAllDataProperties(StubKey)
{
    return StubSignature::withoutResult({ operationArgumentGPR(1), operationArgumentGPR(2) });
}

void generateCopyAllDataProperties(StubAssembler& jit)
{
    Jump isCell = jit.branchIfCell(jit.operand(1));
    jit.returnToCaller();
    isCell.link(&jit);
    for (StubRegister argument : { StubRegister::A3, StubRegister::A4, StubRegister::A5 })
        jit.move(TrustedImm32(0), jit.allocateScratch(argument));
    jit.jumpToStub(callOperationFor(Entry::operationAOTCopyDataProperties, OperationCall::Returns::Void, OperationCall::FirstArgument::Instance).withImmediate(offsetInRuntimeTable(Entry::operationAOTCopyDataProperties)));
}

struct MoveWalk {
    GPRReg target;
    GPRReg source;
    GPRReg copy;
    GPRReg endOfRun;
    GPRReg offset;
    GPRReg bits;
    GPRReg scratch;
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
    jit.loadPtr(BaseIndex(instanceGPR, place, CCallHelpers::ScalePtr, static_cast<int32_t>(InstanceLayout::offsetOfCopiesWithoutFrame)), copy);
    return jit.branchTestPtr(CCallHelpers::Zero, copy);
}

static void copyByFieldMap(StubAssembler& jit, GPRReg target, GPRReg source, GPRReg excludedRun, GPRReg count, GPRReg excludedKeys, GPRReg structureIDOfTarget, GPRReg copy, GPRReg word, JumpList& takesFrame, JumpList& keepsNoCopyOfSource, const Invocable<void()> auto& leaveWithCopyMade)
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
    keepsNoCopyOfSource.append(jit.branch32(CCallHelpers::NotEqual, word, Address(copy, OBJECT_OFFSETOF(MappedCopy, structureOfSource))));
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
    GPRReg structure;
    GPRReg word;
    GPRReg orderWord;
};

static void copyFieldWords(StubAssembler& jit, const ChunkCopy& copy)
{
    const auto [target, source, excludedRun, count, structure, word, orderWord] = copy;
    const GPRReg layout = structure;
    const GPRReg fieldBytes = structure;
    constexpr int32_t wordSize = sizeof(uint64_t);
    constexpr int32_t chunkSize = 2 * wordSize;
    static_assert(offsetOfFirstField == chunkSize);
    const GPRReg secondWord = excludedRun;
    UNUSED_VARIABLE(count);
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
    copyChunkAtOffset(jit, source, target, fieldBytes, word, secondWord);
    jit.branchSub32(CCallHelpers::NonZero, TrustedImm32(chunkSize), fieldBytes).linkTo(copyNextChunk, &jit);
    isCopied.link(&jit);
    jit.and32(TrustedImm32(orderAndCountMask), orderWord);
    jit.store32(orderWord, Address(target, offsetOfOrderWord));
}

static uint32_t firstWordOfRecordLayoutWith(bool hasExtensionData)
{
    LayoutData bits { };
    bits.kindBits = static_cast<uint32_t>(LayoutKind::Record);
    bits.hasExtensionData = hasExtensionData;
    return bits.firstWord();
}

static void copyFieldWordsOfRecord(StubAssembler& jit, const ChunkCopy& copy, JumpList& callsOperation, JumpList& hasOtherLayout)
{
    const GPRReg layout = copy.structure;
    const GPRReg word = copy.word;
    LayoutData testedBits { };
    testedBits.kindBits = (1 << 4) - 1;
    testedBits.hasExtensionData = true;
    jit.loadPtr(Address(copy.structure, CompiledStructure::offsetOfLayout()), layout);
    jit.load32(Address(layout), word);
    jit.and32(TrustedImm32(static_cast<int32_t>(testedBits.firstWord())), word);
    hasOtherLayout.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(static_cast<int32_t>(firstWordOfRecordLayoutWith(false)))));
    callsOperation.append(branchIfBarrierIsNeeded(jit, copy.target, word));
    copyFieldWords(jit, copy);
}

static void copyByChunks(StubAssembler& jit, const ChunkCopy& copy, JumpList& callsOperation, JumpList& hasOtherLayout)
{
    const auto [target, source, excludedRun, count, structure, word, orderWord] = copy;
    callsOperation.append(branchIfNotCompiledObjectOfThisImage(jit, target));
    jit.move(count, word);
    callsOperation.append(jit.branchOr32(CCallHelpers::NonZero, excludedRun, word));
    loadShapeWord(jit, target, word);
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
    copyFieldWordsOfRecord(jit, copy, callsOperation, hasOtherLayout);
}

static void branchUnlessSourceOfSameLayoutHasUnnumberedKeyOrder(StubAssembler& jit, GPRReg target, GPRReg source, GPRReg scratch, JumpList& otherwise)
{
    loadShapeWord(jit, target, scratch);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(source, offsetOfShapeWord)));
    jit.load32(Address(source, offsetOfOrderWord), scratch);
    jit.and32(TrustedImm32(~(orderWordIntegrityBits | orderWordCountBits)), scratch);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(outOfLineOrderWord)));
}

static void takeStructureOfSource(StubAssembler& jit, const ChunkCopy& copy)
{
    jit.load32(Address(copy.source, JSCell::structureIDOffset()), copy.word);
    jit.store32(copy.word, Address(copy.target, JSCell::structureIDOffset()));
}

static void copyByChunksWithStructure(StubAssembler& jit, const ChunkCopy& copy, JumpList& callsOperation, JumpList& hasOtherLayout)
{
    const auto [target, source, excludedRun, count, structure, word, orderWord] = copy;
    const GPRReg structureOfSource = word;
    const GPRReg scratch = orderWord;
    callsOperation.append(branchIfNotCompiledObjectOfThisImage(jit, target));
    callsOperation.append(jit.branchIfNotType(source, CompiledObjectType));
    jit.move(count, word);
    callsOperation.append(jit.branchOr32(CCallHelpers::NonZero, excludedRun, word));
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, Address(target, offsetOfOrderWord), TrustedImm32(unorderedOrderWord)));

    static_assert(std::endian::native == std::endian::little && CompiledStructure::offsetOfHeldKeyOrder() == CompiledStructure::offsetOfFlags() + 1);
    constexpr int32_t hasOnlyPlainSlots = static_cast<uint8_t>(CompiledStructure::Flag::HasOnlyPlainSlots);
    constexpr int32_t hasOutOfLineKeyOrder = static_cast<uint8_t>(CompiledStructure::Flag::HasOutOfLineKeyOrder);
    constexpr int32_t otherFlags = static_cast<uint8_t>(CompiledStructure::Flag::HasExtraProperties) | static_cast<uint8_t>(CompiledStructure::Flag::HasOtherPrototype);
    decodeStructureID(jit, structure);
    jit.load8(Address(structure, CompiledStructure::offsetOfFlags()), scratch);
    jit.and32(TrustedImm32(hasOnlyPlainSlots | hasOutOfLineKeyOrder | otherFlags), scratch);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(hasOnlyPlainSlots)));
    callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, Address(structure, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));

    loadStructure(jit, source, structureOfSource);
    jit.load16(Address(structureOfSource, CompiledStructure::offsetOfFlags()), scratch);
    jit.and32(TrustedImm32(hasOutOfLineKeyOrder | otherFlags | 0xff00), scratch);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(hasOutOfLineKeyOrder)));
    callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, Address(structureOfSource, Structure::bitFieldOffset()), TrustedImm32(Structure::s_mayBePrototypeBits)));
    jit.loadPtr(Address(structureOfSource, Structure::realmOffset()), scratch);
    callsOperation.append(jit.branchPtr(CCallHelpers::NotEqual, scratch, Address(structure, Structure::realmOffset())));
    jit.load64(Address(structureOfSource, Structure::prototypeOffset()), scratch);
    callsOperation.append(jit.branch64(CCallHelpers::NotEqual, scratch, Address(structure, Structure::prototypeOffset())));

    jit.load32(Address(source, offsetOfOrderWord), orderWord);
    jit.and32(TrustedImm32(orderAndCountMask), orderWord);
    copyFieldWordsOfRecord(jit, copy, callsOperation, hasOtherLayout);
    takeStructureOfSource(jit, copy);
}

static void copyByChunksWithExtension(StubAssembler& jit, const ChunkCopy& copy, JumpList& callsOperation)
{
    const auto [target, source, excludedRun, count, structure, word, orderWord] = copy;
    const GPRReg from = word;
    const GPRReg units = excludedRun;
    const GPRReg made = count;
    const GPRReg allocator = orderWord;
    const GPRReg scratch = structure;
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(static_cast<int32_t>(firstWordOfRecordLayoutWith(true)))));
    if (jit.options().useWideReferences) {
        callsOperation.append(jit.jump());
        return;
    }
    callsOperation.append(branchIfBarrierIsNeeded(jit, target, word));

    loadExtensionWordOffset(jit, source, from);
    jit.load32(BaseIndex(source, from, CCallHelpers::TimesOne), from);
    Jump hasNone = jit.branchTest32(CCallHelpers::Zero, from);
    decodeCell32(jit, from, from);
    jit.addPtr(TrustedImm32(offsetOfEmptyExtensionInCage), from);
    JumpList givesUp;
    jit.load64(Address(from, offsetOfExtensionGranuleBits), allocator);
    jit.countPopulation64(allocator, scratch, CCallHelpers::fpTempRegister);
    givesUp.append(jit.branch32(CCallHelpers::Above, scratch, TrustedImm32(maxGranulesOfExtensionFromStub)));
    loadUnitsOfExtensionWith(jit, allocator, units, scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cagedAuxiliarySpace)), allocator);
    jit.loadPtr(BaseIndex(allocator, units, CCallHelpers::ScalePtr, CompleteSubspace::offsetOfAllocatorForSizeStep()), allocator);
    givesUp.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    allocateCell(jit, made, allocator, scratch, givesUp);
    static_assert(minExtensionSize == 2 * sizeof(uint64_t));
    jit.lshift32(TrustedImm32(4), units);
    Label copyNextChunk = jit.label();
    jit.sub32(TrustedImm32(minExtensionSize), units);
    copyChunkAtOffset(jit, from, made, units, allocator, scratch);
    jit.branchTest32(CCallHelpers::NonZero, units).linkTo(copyNextChunk, &jit);
    fenceAfterAllocation(jit);
    encodeCell32(jit, made, made);
    jit.sub32(TrustedImm32(offsetOfEmptyExtensionInCage >> cell32Shift), made);
    hasNone.link(&jit);

    loadStructure(jit, target, structure);
    jit.loadPtr(Address(structure, CompiledStructure::offsetOfLayout()), structure);
    jit.load32(Address(source, offsetOfOrderWord), orderWord);
    copyFieldWords(jit, copy);
    loadExtensionWordOffset(jit, target, word);
    jit.store32(made, BaseIndex(target, word, CCallHelpers::TimesOne));
    Jump isCopied = jit.jump();

    givesUp.link(&jit);
    jit.move(TrustedImm32(0), units);
    jit.move(TrustedImm32(0), made);
    callsOperation.append(jit.jump());
    isCopied.link(&jit);
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
    const ChunkCopy copy { target, source, excludedRun, count, structure, word, orderWord };
    auto allocateScratches = [&] {
        for (StubRegister scratch : { StubRegister::R0, StubRegister::T9, StubRegister::T12 })
            jit.allocateScratch(scratch);
    };
    JumpList callsOperation;
    callsOperation.append(jit.branchIfNotCell(source));

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
        if (!isX86_64())
            saved.append(excludedRun);
        jit.preserving(saved.span(), [&] {
            allocateScratches();
            JumpList isLeftToOperation;
            JumpList hasOtherLayout;
            JumpList isCopied;
            jit.move(immediate, structure);
            copyByChunks(jit, copy, isLeftToOperation, hasOtherLayout);
            jit.move(TrustedImm32(0), immediate);
            isCopied.append(jit.jump());
            hasOtherLayout.link(&jit);
            jit.preserving({ excludedRun, count }, [&] {
                JumpList isNotCopied;
                copyByChunksWithExtension(jit, copy, isNotCopied);
                jit.move(TrustedImm32(0), immediate);
                Jump isDecided = jit.jump();
                isNotCopied.link(&jit);
                jit.move(TrustedImm32(1), immediate);
                isDecided.link(&jit);
            });
            isCopied.append(jit.jump());
            isLeftToOperation.link(&jit);
            jit.move(TrustedImm32(1), immediate);
            isCopied.link(&jit);
        });
        Jump isNotCopied = jit.branchTest32(CCallHelpers::NonZero, immediate);
        jit.returnToCaller();

        isNotCopied.link(&jit);
        structuresDiffer.link(&jit);
        JumpList keepsNoCopyOfSource;
        callsOperation.append(loadCopyWithoutFrame(jit, target, source, excludedRun, immediate));
        jit.load32(Address(immediate, OBJECT_OFFSETOF(MappedCopy, structureOfSource)), immediate);
        keepsNoCopyOfSource.append(jit.branch32(CCallHelpers::NotEqual, immediate, Address(source, JSCell::structureIDOffset())));
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
            copyByFieldMap(jit, target, source, excludedRun, count, excludedKeys, structure, word, orderWord, leavesToTakeFrame, leavesToTakeFrame, [&] {
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

        keepsNoCopyOfSource.link(&jit);
        branchUnlessSourceOfSameLayoutHasUnnumberedKeyOrder(jit, target, source, immediate, callsOperation);
        Vector<GPRReg, 5> savedForStructure;
        for (GPRReg written : { word, orderWord, structure, excludedRun, count }) {
            if (written != immediate)
                savedForStructure.append(written);
        }
        jit.preserving(savedForStructure.span(), [&] {
            JumpList isLeftToOperation;
            JumpList hasOtherLayout;
            JumpList isDecided;
            jit.load32(Address(target, JSCell::structureIDOffset()), structure);
            copyByChunksWithStructure(jit, copy, isLeftToOperation, hasOtherLayout);
            jit.move(TrustedImm32(0), immediate);
            isDecided.append(jit.jump());
            hasOtherLayout.link(&jit);
            copyByChunksWithExtension(jit, copy, isLeftToOperation);
            takeStructureOfSource(jit, copy);
            jit.move(TrustedImm32(0), immediate);
            isDecided.append(jit.jump());
            isLeftToOperation.link(&jit);
            jit.move(TrustedImm32(1), immediate);
            isDecided.link(&jit);
        });
        callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, immediate));
        jit.returnToCaller();

        callsOperation.link(&jit);
        return PreludeExit::CallsOperation;
    }

    allocateScratches();
    jit.recordClobbers(RegisterSet(excludedRun, count, excludedKeys));
    jit.load32(Address(target, JSCell::structureIDOffset()), structure);
    Jump structuresDiffer = jit.branch32(CCallHelpers::NotEqual, structure, Address(source, JSCell::structureIDOffset()));
    JumpList isNotCopiedByChunks;
    JumpList hasOtherLayout;
    copyByChunks(jit, copy, isNotCopiedByChunks, hasOtherLayout);
    jit.returnToCaller();

    hasOtherLayout.link(&jit);
    copyByChunksWithExtension(jit, copy, isNotCopiedByChunks);
    jit.returnToCaller();

    isNotCopiedByChunks.link(&jit);
    jit.load32(Address(target, JSCell::structureIDOffset()), structure);
    structuresDiffer.link(&jit);
    JumpList keepsNoCopyOfSource;
    copyByFieldMap(jit, target, source, excludedRun, count, excludedKeys, structure, word, orderWord, callsOperation, keepsNoCopyOfSource, [&] {
        jit.returnToCaller();
    });
    jit.jumpToOperationCallOf(Entry::operationAOTCopyDataPropertiesByFieldMap);

    keepsNoCopyOfSource.link(&jit);
    JumpList hasOtherLayoutAndStructure;
    branchUnlessSourceOfSameLayoutHasUnnumberedKeyOrder(jit, target, source, word, callsOperation);
    copyByChunksWithStructure(jit, copy, callsOperation, hasOtherLayoutAndStructure);
    jit.returnToCaller();

    hasOtherLayoutAndStructure.link(&jit);
    copyByChunksWithExtension(jit, copy, callsOperation);
    takeStructureOfSource(jit, copy);
    jit.returnToCaller();

    callsOperation.link(&jit);
    return PreludeExit::CallsOperation;
}

StubSignature signatureOfCopyObjectAndDefineFrontEnd(StubKey)
{
    return operationFrontEndSignature(4);
}

void generateCopyObjectAndDefineFrontEnd(StubAssembler& jit)
{
    const GPRReg source = jit.operand(0);
    const GPRReg firstName = jit.clobberOperand(1);
    const GPRReg count = jit.clobberOperand(2);
    const GPRReg list = jit.clobberOperand(3);
    const GPRReg copy = jit.result();
    const GPRReg record = jit.allocateScratch(StubRegister::T9);
    const GPRReg size = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T12);
    const GPRReg secondScratchOfChunk = isARM64() ? jit.allocateScratch(StubRegister::T10) : InvalidGPRReg;
    const GPRReg butterfly = firstName;
    RELEASE_ASSERT(noOverlap(source, firstName, count, list, copy, record, size, scratch, instanceGPR));
    RELEASE_ASSERT(secondScratchOfChunk == InvalidGPRReg || noOverlap(secondScratchOfChunk, source, firstName, count, list, copy, record, size, scratch, instanceGPR));
    JumpList callsOperation;
    JumpList callsOperationWithList;

    constexpr int32_t wordSize = sizeof(uint64_t);
    constexpr int32_t chunkSize = 2 * wordSize;
    constexpr int32_t offsetOfNames = sizeof(CopyWithNames);
    static_assert(JSFinalObject::offsetOfInlineStorage() == chunkSize && MarkedSpace::sizeStep == chunkSize && !JSCell::structureIDOffset() && std::endian::native == std::endian::little);
    static_assert(sizeof(NameInCopy) == sizeof(EncodedJSValue) && MarkedSpace::sizeStep == 2 * sizeof(Allocator) && sizeof(IndexingHeader) == sizeof(EncodedJSValue));
    static_assert(hasOneBitSet(numberOfPlacesOfCopiesWithNames));

    callsOperation.append(jit.branchIfNotCell(source));
    jit.load32(Address(source, JSCell::structureIDOffset()), scratch);
    jit.urshift32(scratch, TrustedImm32(copyWithNamesHashShift), record);
    jit.add32(firstName, record);
    jit.and32(TrustedImm32(numberOfPlacesOfCopiesWithNames - 1), record);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(objects.copiesWithNames)), size);
    jit.load32(BaseIndex(size, record, CCallHelpers::TimesFour), record);
    callsOperation.append(jit.branchTest32(CCallHelpers::Zero, record));
    jit.addPtr(size, record);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(record, OBJECT_OFFSETOF(CopyWithNames, structureOfSource))));
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, firstName, Address(record, OBJECT_OFFSETOF(CopyWithNames, firstName))));
    jit.load16(Address(record, OBJECT_OFFSETOF(CopyWithNames, count)), size);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, size, count));
    Jump hasOneName = jit.branch32(CCallHelpers::Equal, size, TrustedImm32(1));
    Label compareNextName = jit.label();
    jit.sub32(TrustedImm32(1), size);
    jit.load32(BaseIndex(record, size, CCallHelpers::TimesEight, offsetOfNames + OBJECT_OFFSETOF(NameInCopy, name)), scratch);
    jit.load32(BaseIndex(list, size, CCallHelpers::TimesEight, wordSize), copy);
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, scratch, copy));
    jit.branch32(CCallHelpers::Above, size, TrustedImm32(1)).linkTo(compareNextName, &jit);
    hasOneName.link(&jit);

    jit.move(TrustedImm32(0), butterfly);
    jit.load16(Address(record, OBJECT_OFFSETOF(CopyWithNames, storageSize)), size);
    Jump hasNoStorage = jit.branchTest32(CCallHelpers::Zero, size);
    jit.urshift32(size, TrustedImm32(1), scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.auxiliarySpace)), count);
    jit.loadPtr(BaseIndex(count, scratch, CCallHelpers::TimesOne, CompleteSubspace::offsetOfAllocatorForSizeStep()), scratch);
    callsOperationWithList.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
    allocateCell(jit, copy, scratch, count, callsOperationWithList);
    jit.getEffectiveAddress(BaseIndex(copy, size, CCallHelpers::TimesOne, sizeof(IndexingHeader)), butterfly);
    jit.load16(Address(record, OBJECT_OFFSETOF(CopyWithNames, storageSizeOfSource)), scratch);
    jit.sub32(scratch, size);
    Jump hasNoMoreStorage = jit.branchTest32(CCallHelpers::Zero, size);
    Label zeroNextChunk = jit.label();
    jit.sub32(TrustedImm32(chunkSize), size);
    jit.store64(TrustedImm32(0), BaseIndex(copy, size, CCallHelpers::TimesOne));
    jit.store64(TrustedImm32(0), BaseIndex(copy, size, CCallHelpers::TimesOne, wordSize));
    jit.branchTest32(CCallHelpers::NonZero, size).linkTo(zeroNextChunk, &jit);
    hasNoMoreStorage.link(&jit);
    Jump sourceHasNoStorage = jit.branchTest32(CCallHelpers::Zero, scratch);
    jit.move(scratch, size);
    jit.loadPtr(Address(source, JSObject::butterflyOffset()), count);
    jit.subPtr(size, count);
    jit.subPtr(TrustedImm32(sizeof(IndexingHeader)), count);
    jit.move(butterfly, copy);
    jit.subPtr(size, copy);
    jit.subPtr(TrustedImm32(sizeof(IndexingHeader)), copy);
    Label copyNextChunkOfStorage = jit.label();
    jit.sub32(TrustedImm32(chunkSize), size);
    copyChunkAtOffset(jit, count, copy, size, scratch, secondScratchOfChunk);
    jit.branchTest32(CCallHelpers::NonZero, size).linkTo(copyNextChunkOfStorage, &jit);
    sourceHasNoStorage.link(&jit);
    hasNoStorage.link(&jit);

    jit.load16(Address(record, OBJECT_OFFSETOF(CopyWithNames, cellSize)), size);
    jit.urshift32(size, TrustedImm32(1), scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.cellSpace)), count);
    jit.loadPtr(BaseIndex(count, scratch, CCallHelpers::TimesOne, CompleteSubspace::offsetOfAllocatorForSizeStep()), scratch);
    callsOperationWithList.append(jit.branchTestPtr(CCallHelpers::Zero, scratch));
    allocateCell(jit, copy, scratch, count, callsOperationWithList);
    Jump hasNoInlineStorage = jit.branch32(CCallHelpers::Equal, size, TrustedImm32(chunkSize));
    Label copyNextChunkOfCell = jit.label();
    jit.sub32(TrustedImm32(chunkSize), size);
    copyChunkAtOffset(jit, source, copy, size, scratch, count);
    jit.branch32(CCallHelpers::Above, size, TrustedImm32(chunkSize)).linkTo(copyNextChunkOfCell, &jit);
    hasNoInlineStorage.link(&jit);
    jit.storePtr(butterfly, Address(copy, JSObject::butterflyOffset()));
    jit.load64(Address(record, OBJECT_OFFSETOF(CopyWithNames, header)), scratch);
    jit.store64(scratch, Address(copy));

    jit.load16(Address(record, OBJECT_OFFSETOF(CopyWithNames, count)), count);
    jit.getEffectiveAddress(BaseIndex(list, count, CCallHelpers::TimesEight, wordSize), list);
    Label storeNextValue = jit.label();
    jit.sub32(TrustedImm32(1), count);
    jit.load32(BaseIndex(record, count, CCallHelpers::TimesEight, offsetOfNames + OBJECT_OFFSETOF(NameInCopy, offset)), size);
    jit.load64(BaseIndex(list, count, CCallHelpers::TimesEight), scratch);
    Jump isOutsideCell = jit.branch32(CCallHelpers::LessThan, size, TrustedImm32(0));
    jit.store64(scratch, BaseIndex(copy, size, CCallHelpers::TimesOne));
    Jump isStored = jit.jump();
    isOutsideCell.link(&jit);
    jit.signExtend32ToPtr(size, size);
    jit.store64(scratch, BaseIndex(butterfly, size, CCallHelpers::TimesOne));
    isStored.link(&jit);
    jit.branchTest32(CCallHelpers::NonZero, count).linkTo(storeNextValue, &jit);
    fenceAfterAllocation(jit);
    jit.move(TrustedImm32(0), jit.secondResult());
    jit.returnToCaller();

    callsOperationWithList.link(&jit);
    jit.load32(Address(list), count);
    jit.load32(Address(list, wordSize), firstName);
    callsOperation.link(&jit);
    jit.tailCallOperation(Entry::RawCopyObjectAndDefine, OperationCall::FirstArgument::Instance);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
