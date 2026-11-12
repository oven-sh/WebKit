/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsArrays.h"

#if ENABLE(AOT)

#include "AOTCompactTable.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsHashTables.h"
#include "Butterfly.h"
#include "IndexingType.h"
#include "JSCell.h"
#include "JSCellButterfly.h"
#include "JSGlobalObject.h"
#include "JSObject.h"
#include "JSMap.h"
#include "OperationResult.h"
#include <type_traits>

namespace JSC { namespace AOT {

static_assert(std::is_same_v<decltype(&operationAOTArrayPush), OperationReturnType<EncodedJSValue> (JIT_OPERATION_ATTRIBUTES *)(Instance*, JSCell*, EncodedJSValue)>);
static_assert(std::is_same_v<decltype(&operationAOTAppendElements), OperationReturnType<void> (JIT_OPERATION_ATTRIBUTES *)(Instance*, JSCell*, JSCell*)>);
static_assert(std::is_same_v<decltype(&operationAOTAppendMembers), OperationReturnType<void> (JIT_OPERATION_ATTRIBUTES *)(Instance*, JSCell*, JSCell*)>);
static_assert(std::is_same_v<decltype(&operationAOTAppendIterable), OperationReturnType<void> (JIT_OPERATION_ATTRIBUTES *)(Instance*, JSCell*, EncodedJSValue)>);
static_assert(std::is_same_v<decltype(&operationAOTDestructureIterable), OperationReturnType<void> (JIT_OPERATION_ATTRIBUTES *)(Instance*, EncodedJSValue, uint32_t, EncodedJSValue*)>);
static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1) && gprOf(StubRegister::A2) == operationArgumentGPR(2));

#if CPU(ARM64)
static constexpr StubRegister arrayPushSavedNumberRegister = StubRegister::T12;
static constexpr StubRegister arrayPushButterflyRegister = StubRegister::T13;
#elif CPU(X86_64)
static constexpr StubRegister arrayPushSavedNumberRegister = StubRegister::A5;
static constexpr StubRegister arrayPushButterflyRegister = StubRegister::A4;
#endif

static void emitSlowPathCall(StubAssembler& jit, Entry operation)
{
    jit.callOperationAndReturn(operation, { .returns = OperationCall::Returns::Value, .saves = OperationCall::Saves::CallerSavedRegisters });
}

void emitElementBarrierAndReturn(StubAssembler& jit, GPRReg array, GPRReg value)
{
    const StubKey target = rememberElement();
    const GPRReg rememberedArray = jit.reuseScratch(StubRegister::T9);
    const GPRReg rememberedValue = jit.reuseScratch(StubRegister::T11);
    checkStubOperands(target, { rememberedArray, rememberedValue });
    RELEASE_ASSERT(noOverlap(array, value, rememberedArray, rememberedValue));
    const GPRReg state = rememberedArray;

    Jump isNotCell = jit.branchIfNotCell(value);
    Jump needsBarrier = branchIfBarrierIsNeeded(jit, array, state);
    isNotCell.link(&jit);
    jit.returnToCaller();

    needsBarrier.link(&jit);
    jit.move(array, rememberedArray);
    jit.move(value, rememberedValue);
    jit.jumpToStub(target);
}

StubSignature signatureOfArrayPush(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2) }).fillingOperandInFront(1);
}

StubSignature signatureOfArrayPushOfKnownKind(StubKey)
{
    return StubSignature::withGPRResult({ gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(arrayPushKindRegister) });
}

void generateArrayPush(StubAssembler& jit)
{
    const GPRReg array = jit.operand(0);
    const GPRReg value = jit.operand(1);
    const GPRReg length = jit.result();
    const GPRReg kind = jit.allocateScratch(arrayPushKindRegister);
    const GPRReg savedNumber = jit.allocateScratch(arrayPushSavedNumberRegister);
    const GPRReg butterfly = jit.allocateScratch(arrayPushButterflyRegister);
    constexpr FPRReg number = FPRInfo::fpRegT0;
    JumpList slowPath;

    Label loadKind = jit.label();
    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), kind);
    jit.bindEntry(arrayPushOfKnownKind());
    jit.loadPtr(Address(array, JSObject::butterflyOffset()), butterfly);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
    Jump isFull = jit.branch32(CCallHelpers::AboveOrEqual, length, Address(butterfly, Butterfly::offsetOfVectorLength()));
    Jump notNumber = jit.branchIfNotNumber(value);
    Jump isDoubleShape = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(DoubleShape));
    Jump isContiguousShape = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(ContiguousShape));
    slowPath.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(Int32Shape)));
    slowPath.append(jit.branchIfNotInt32(value));
    isContiguousShape.link(&jit);
    jit.store64(value, BaseIndex(butterfly, length, CCallHelpers::TimesEight));
    Jump isStored = jit.jump();

    isDoubleShape.link(&jit);
    jit.saveFPR(number, savedNumber);
    Jump isInt32 = jit.branchIfInt32(value);
    unboxDouble(jit, value, kind, number);
    Jump isNaN = jit.branchIfNaN(number);
    Jump isDouble = jit.jump();
    isInt32.link(&jit);
    jit.convertInt32ToDouble(value, number);
    isDouble.link(&jit);
    jit.storeDouble(number, BaseIndex(butterfly, length, CCallHelpers::TimesEight));
    jit.restoreFPR(number, savedNumber);
    isStored.link(&jit);
    jit.add32(TrustedImm32(1), length);
    jit.store32(length, Address(butterfly, Butterfly::offsetOfPublicLength()));
    boxInt32(jit, length, length);
    jit.returnToCaller();

    notNumber.link(&jit);
    Jump holdsAnyValue = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(ContiguousShape));
    slowPath.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(Int32Shape)));
    widenStorageOfIntegersForAnyValue(jit, array, kind, slowPath);
    holdsAnyValue.link(&jit);
    jit.store64(value, BaseIndex(butterfly, length, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), length);
    jit.store32(length, Address(butterfly, Butterfly::offsetOfPublicLength()));
    boxInt32(jit, length, length);
    emitElementBarrierAndReturn(jit, array, value);

    isFull.link(&jit);
    growFullVector(jit, { array, butterfly, length, savedNumber, kind, jit.reuseScratch(StubRegister::T9) }, slowPath);
    jit.jump().linkTo(loadKind, &jit);

    isNaN.link(&jit);
    jit.restoreFPR(number, savedNumber);
    slowPath.link(&jit);
    emitSlowPathCall(jit, Entry::operationAOTArrayPush);
}

StubSignature signatureOfAppendElements(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::A1));
    signature.addOperand(gprOf(StubRegister::A2));
    return signature;
}

void generateAppendElements(StubAssembler& jit)
{
    const GPRReg array = jit.operand(0);
    const GPRReg source = jit.operand(1);
    const GPRReg newLength = jit.allocateScratch(StubRegister::T9);
    const GPRReg target = jit.allocateScratch(StubRegister::T10);
    const GPRReg elements = jit.allocateScratch(StubRegister::T11);
    const GPRReg kind = jit.allocateScratch(StubRegister::T12);
    const GPRReg position = jit.allocateScratch(StubRegister::T14);
    const GPRReg element = jit.allocateScratch(firstStubOperandRegister);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg numberScratch = jit.allocateScratch(FPRInfo::fpRegT1);
    RELEASE_ASSERT(noOverlap(array, source, newLength, target, elements, kind, position, element));
    const BaseIndex read(elements, position, CCallHelpers::TimesEight);
    const BaseIndex written(target, position, CCallHelpers::TimesEight);
    const Address lengthOfTarget(target, Butterfly::offsetOfPublicLength());
    JumpList slowPath;
    JumpList hasHole;

    auto loadKindOfSource = [&] {
        jit.load8(Address(source, JSCell::indexingTypeAndMiscOffset()), kind);
        jit.and32(TrustedImm32(IndexingShapeMask), kind);
    };
    auto forEachElement = [&](const auto& copyElement) {
        Label loop = jit.label();
        copyElement();
        jit.add32(TrustedImm32(1), position);
        jit.branch32(CCallHelpers::Below, position, newLength).linkTo(loop, &jit);
    };
    auto setLengthAndReturn = [&] {
        jit.store32(newLength, lengthOfTarget);
        jit.returnToCaller();
    };

    Label start = jit.label();
    jit.loadPtr(Address(source, JSObject::butterflyOffset()), elements);
    jit.load32(Address(elements, Butterfly::offsetOfPublicLength()), newLength);
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, newLength);
    slowPath.append(jit.branch32(CCallHelpers::Above, newLength, TrustedImm32(maxLengthForElementwiseCopy)));
    jit.loadPtr(Address(array, JSObject::butterflyOffset()), target);
    jit.load32(lengthOfTarget, position);
    jit.add32(position, newLength);
    Jump hasNoRoom = jit.branch32(CCallHelpers::Above, newLength, Address(target, Butterfly::offsetOfVectorLength()));
    jit.lshift64(position, TrustedImm32(3), element);
    jit.subPtr(element, elements);
    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), kind);
    Jump targetHoldsDoubles = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(DoubleShape));
    Jump targetHoldsInt32 = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(Int32Shape));
    slowPath.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(ContiguousShape)));

    loadKindOfSource();
    Jump boxesDoubles = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(DoubleShape));
    Jump copiesValues = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(ContiguousShape));
    slowPath.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(Int32Shape)));
    copiesValues.link(&jit);
    jit.store32(newLength, lengthOfTarget);
    forEachElement([&] {
        jit.load64(read, element);
        Jump isNoHole = jit.branchTest64(CCallHelpers::NonZero, element);
        jit.move(TrustedImm64(JSValue::ValueUndefined), element);
        isNoHole.link(&jit);
        jit.store64(element, written);
    });
    Jump holdsNoCells = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(Int32Shape));
    Jump needsBarrier = branchIfBarrierIsNeeded(jit, array, kind);
    holdsNoCells.link(&jit);
    jit.returnToCaller();

    static_assert(UndecidedShape < ContiguousShape && Int32Shape < ContiguousShape && DoubleShape < ContiguousShape && ArrayStorageShape > ContiguousShape && CopyOnWrite > ContiguousShape);
    isEmpty.link(&jit);
    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), kind);
    Jump mayRefuse = jit.branch32(CCallHelpers::Above, kind, TrustedImm32(ContiguousShape));
    jit.returnToCaller();

    mayRefuse.link(&jit);
    slowPath.append(jit.branchTest32(CCallHelpers::Zero, kind, TrustedImm32(CopyOnWrite)));
    loadStructure(jit, array, target);
    jit.load32(Address(target, Structure::bitFieldOffset()), kind);
    jit.urshift32(TrustedImm32(Structure::s_transitionKindShift), kind);
    jit.and32(TrustedImm32(Structure::s_transitionKindMask), kind);
    slowPath.append(jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(static_cast<int32_t>(TransitionKind::FreezeElementsInPlace))));
    jit.returnToCaller();

    needsBarrier.link(&jit);
    checkStubOperands(writeBarrier(), { element });
    jit.move(array, element);
    jit.jumpToStub(writeBarrier());

    boxesDoubles.link(&jit);
    jit.store32(newLength, lengthOfTarget);
    forEachElement([&] {
        JumpList isNotInt32;
        jit.loadDouble(read, number);
        Jump isHole;
        PureDouble read = PureDouble::branchIfNaN(jit, number, isHole);
        branchIfNotExactInt32(jit, number, element, isNotInt32, numberScratch);
        boxInt32(jit, element, element);
        Jump isBoxedInt32 = jit.jump();
        isNotInt32.link(&jit);
        boxDouble(jit, read, element);
        Jump isBoxedDouble = jit.jump();
        isHole.link(&jit);
        jit.move(TrustedImm64(JSValue::ValueUndefined), element);
        isBoxedInt32.link(&jit);
        isBoxedDouble.link(&jit);
        jit.store64(element, written);
    });
    jit.returnToCaller();

    targetHoldsInt32.link(&jit);
    loadKindOfSource();
    Jump takesInt32OutOfValues = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(ContiguousShape));
    slowPath.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(Int32Shape)));
    forEachElement([&] {
        jit.load64(read, element);
        hasHole.append(jit.branchTest64(CCallHelpers::Zero, element));
        jit.store64(element, written);
    });
    setLengthAndReturn();

    takesInt32OutOfValues.link(&jit);
    forEachElement([&] {
        jit.load64(read, element);
        hasHole.append(jit.branchIfNotInt32(element));
        jit.store64(element, written);
    });
    setLengthAndReturn();

    targetHoldsDoubles.link(&jit);
    loadKindOfSource();
    Jump convertsInt32 = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(Int32Shape));
    Jump convertsValues = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(ContiguousShape));
    slowPath.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(DoubleShape)));
    forEachElement([&] {
        jit.loadDouble(read, number);
        hasHole.append(jit.branchIfNaN(number));
        jit.storeDouble(number, written);
    });
    setLengthAndReturn();

    convertsInt32.link(&jit);
    forEachElement([&] {
        jit.load64(read, element);
        hasHole.append(jit.branchTest64(CCallHelpers::Zero, element));
        jit.convertInt32ToDouble(element, number);
        jit.storeDouble(number, written);
    });
    setLengthAndReturn();

    convertsValues.link(&jit);
    forEachElement([&] {
        jit.load64(read, element);
        Jump isInt32 = jit.branchIfInt32(element);
        hasHole.append(jit.branchIfNotNumber(element));
        unboxDouble(jit, element, kind, number);
        hasHole.append(jit.branchIfNaN(number));
        Jump isDouble = jit.jump();
        isInt32.link(&jit);
        jit.convertInt32ToDouble(element, number);
        isDouble.link(&jit);
        jit.storeDouble(number, written);
    });
    setLengthAndReturn();

    hasHole.link(&jit);
    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeMask), kind);
    jit.move(TrustedImm64(0), element);
    Jump holeIsEmptyValue = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(Int32Shape));
    jit.move(TrustedImm64(static_cast<int64_t>(PNaNAsBits)), element);
    holeIsEmptyValue.link(&jit);
    Label clear = jit.label();
    slowPath.append(jit.branch32(CCallHelpers::BelowOrEqual, position, lengthOfTarget));
    jit.sub32(TrustedImm32(1), position);
    jit.store64(element, written);
    jit.jump().linkTo(clear, &jit);

    hasNoRoom.link(&jit);
    growVectorFor(jit, { array, target, position, elements, kind, element }, newLength, slowPath);
    jit.jump().linkTo(start, &jit);

    slowPath.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTAppendElements, { .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters });
}

static void emitBranchUnlessPlainSet(StubAssembler& jit, GPRReg cell, GPRReg realm, GPRReg scratch, JumpList& isNoSuchSet)
{
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), realm);
    jit.load32(Address(cell, JSCell::structureIDOffset()), scratch);
    isNoSuchSet.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(realm, OBJECT_OFFSETOF(JSGlobalObject, m_setStructure))));
    isNoSuchSet.append(branchIfWatchpointSetIsNotWatched(jit, Address(realm, OBJECT_OFFSETOF(JSGlobalObject, m_setIteratorProtocolWatchpointSet)), scratch));
}

StubSignature signatureOfAppendIterable(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::A1));
    signature.addOperand(gprOf(StubRegister::A2));
    return signature.fillingOperandInFront(1);
}

void generateAppendIterable(StubAssembler& jit)
{
    const GPRReg array = jit.operand(0);
    const GPRReg iterable = jit.operand(1);
    JumpList callsOperation;

    callsOperation.append(jit.branchIfNotCell(iterable));
    Jump isNoArray = jit.branch8(CCallHelpers::NotEqual, Address(iterable, JSCell::typeInfoTypeOffset()), TrustedImm32(ArrayType));
    callsOperation.append(branchIfNotPristine(jit, iterable));
    checkStubOperands(appendElements(), { array, iterable });
    jit.jumpToStub(appendElements());

    isNoArray.link(&jit);
    emitBranchUnlessPlainSet(jit, iterable, jit.allocateScratch(StubRegister::T9), jit.allocateScratch(StubRegister::T10), callsOperation);
    checkStubOperands(appendMembers(), { array, iterable });
    jit.jumpToStub(appendMembers());

    callsOperation.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTAppendIterable, { .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters });
}

StubSignature signatureOfDestructureIterable(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::A1));
    signature.addOperand(gprOf(StubRegister::A2));
    signature.addOperand(gprOf(StubRegister::A3));
    return signature;
}

void generateDestructureIterable(StubAssembler& jit)
{
    const GPRReg iterable = jit.operand(0);
    const GPRReg count = jit.operand(1);
    const GPRReg results = jit.operand(2);
    const GPRReg elements = jit.allocateScratch(StubRegister::T9);
    const GPRReg length = jit.allocateScratch(StubRegister::T10);
    const GPRReg position = jit.allocateScratch(StubRegister::T11);
    const GPRReg element = jit.allocateScratch(StubRegister::T12);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    RELEASE_ASSERT(noOverlap(iterable, count, results, elements, length, position, element));
    const BaseIndex read(elements, position, CCallHelpers::TimesEight);
    JumpList callsOperation;

    auto takeElements = [&](const auto& loadElement) {
        JumpList isAbsent;
        Label loop = jit.label();
        Jump hasTakenAll = jit.branch32(CCallHelpers::AboveOrEqual, position, count);
        isAbsent.append(jit.branch32(CCallHelpers::AboveOrEqual, position, length));
        loadElement(isAbsent);
        Jump isPresent = jit.jump();
        isAbsent.link(&jit);
        jit.move(TrustedImm64(JSValue::ValueUndefined), element);
        isPresent.link(&jit);
        jit.store64(element, BaseIndex(results, position, CCallHelpers::TimesEight));
        jit.add32(TrustedImm32(1), position);
        jit.jump().linkTo(loop, &jit);
        hasTakenAll.link(&jit);
        jit.returnToCaller();
    };

    callsOperation.append(jit.branchIfNotCell(iterable));
    Jump isNoArray = jit.branch8(CCallHelpers::NotEqual, Address(iterable, JSCell::typeInfoTypeOffset()), TrustedImm32(ArrayType));
    loadPristineArrayKind(jit, iterable, element, IndexingShapeMask);
    jit.loadPtr(Address(iterable, JSObject::butterflyOffset()), elements);
    jit.move(TrustedImm32(0), position);
    Jump holdsDoubles = jit.branch32(CCallHelpers::Equal, element, TrustedImm32(DoubleShape));
    Jump holdsAnyValues = jit.branch32(CCallHelpers::Equal, element, TrustedImm32(ContiguousShape));
    callsOperation.append(jit.branch32(CCallHelpers::NotEqual, element, TrustedImm32(Int32Shape)));
    holdsAnyValues.link(&jit);
    jit.load32(Address(elements, Butterfly::offsetOfPublicLength()), length);
    takeElements([&](JumpList& isAbsent) {
        jit.load64(read, element);
        isAbsent.append(jit.branchTest64(CCallHelpers::Zero, element));
    });

    holdsDoubles.link(&jit);
    jit.load32(Address(elements, Butterfly::offsetOfPublicLength()), length);
    takeElements([&](JumpList& isAbsent) {
        jit.loadDouble(read, number);
        boxDouble(jit, PureDouble::branchIfNaN(jit, number, isAbsent), element);
    });

    isNoArray.link(&jit);
    static_assert(CompactTable::keysShiftMask == 31, "a shift by 32 bits drops the other flags");
    const GPRReg table = elements;
    const GPRReg entry = length;
    const GPRReg taken = position;
    const GPRReg key = element;
    const GPRReg flags = element;
    const GPRReg column = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(noOverlap(iterable, count, results, table, entry, taken, key, column, instanceGPR));
    const Address used(table, CompactTable::offsetOfUsed());
    const Address deletedPrefix(iterable, JSMap::offsetOfDeletedPrefix());
    emitBranchUnlessPlainSet(jit, iterable, table, key, callsOperation);

    jit.loadPtr(Address(iterable, offsetOfHashTableStorage()), table);
    JumpList hasNoEntry;
    JumpList hasNoFirstEntry;
    hasNoFirstEntry.append(jit.branchTestPtr(CCallHelpers::Zero, table));
    hasNoFirstEntry.append(jit.branchTest32(CCallHelpers::Zero, count));
    callsOperation.append(jit.branch8(CCallHelpers::Equal, Address(table, JSCell::typeInfoTypeOffset()), TrustedImm32(JSCellButterflyType)));
    jit.load8(Address(table, CompactTable::offsetOfKeysShift()), flags);
    callsOperation.append(jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(CompactTable::keysAreInImage)));
    jit.move(TrustedImm32(2), column);
    jit.lshift32(flags, column);
    jit.add64(table, column);
    jit.load32(deletedPrefix, entry);
    auto takeKeys = [&](const auto& loadKey, const auto& decodeKey) {
        Label scanForFirst = jit.label();
        hasNoFirstEntry.append(jit.branch32(CCallHelpers::AboveOrEqual, entry, used));
        loadKey();
        Jump isDeleted = jit.branchTest64(CCallHelpers::Zero, key);
        jit.store32(entry, deletedPrefix);
        decodeKey();
        jit.store64(key, Address(results));
        Jump takesMore = jit.branch32(CCallHelpers::Above, count, TrustedImm32(1));
        jit.returnToCaller();

        isDeleted.link(&jit);
        jit.add32(TrustedImm32(1), entry);
        jit.jump().linkTo(scanForFirst, &jit);

        takesMore.link(&jit);
        jit.move(TrustedImm32(1), taken);
        Label scan = jit.label();
        jit.add32(TrustedImm32(1), entry);
        hasNoEntry.append(jit.branch32(CCallHelpers::AboveOrEqual, entry, used));
        loadKey();
        jit.branchTest64(CCallHelpers::Zero, key).linkTo(scan, &jit);
        decodeKey();
        jit.store64(key, BaseIndex(results, taken, CCallHelpers::TimesEight));
        jit.add32(TrustedImm32(1), taken);
        jit.branch32(CCallHelpers::Below, taken, count).linkTo(scan, &jit);
        jit.returnToCaller();
    };
    Jump keysAreWide = jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(CompactTable::keysAreWide));
    takeKeys([&] {
        jit.load32(BaseIndex(column, entry, CCallHelpers::TimesFour, CompactTable::offsetOfIndex()), key);
    }, [&] {
        Jump isImmediate = jit.branch32(CCallHelpers::Below, key, TrustedImm32(minCell32));
        decodeCell32(jit, key, key);
        isImmediate.link(&jit);
    });
    keysAreWide.link(&jit);
    takeKeys([&] {
        jit.load64(BaseIndex(column, entry, CCallHelpers::TimesEight, CompactTable::offsetOfIndex()), key);
    }, [] { });

    hasNoFirstEntry.link(&jit);
    jit.move(TrustedImm32(0), taken);
    hasNoEntry.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), key);
    Label fill = jit.label();
    Jump isFilled = jit.branch32(CCallHelpers::AboveOrEqual, taken, count);
    jit.store64(key, BaseIndex(results, taken, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), taken);
    jit.jump().linkTo(fill, &jit);
    isFilled.link(&jit);
    jit.returnToCaller();

    callsOperation.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTDestructureIterable, { .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters });
}

StubSignature signatureOfAppendValuesOfMap(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::A1));
    signature.addOperand(gprOf(StubRegister::A2));
    return signature;
}

StubSignature signatureOfAppendMembers(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::A1));
    signature.addOperand(gprOf(StubRegister::A2));
    return signature;
}

enum class ColumnAppended : bool { Keys, Values };

static void generateAppendColumn(StubAssembler& jit, ColumnAppended appended)
{
    const bool takesValues = appended == ColumnAppended::Values;
    const GPRReg array = jit.operand(0);
    const GPRReg set = jit.operand(1);
    const GPRReg newLength = jit.allocateScratch(StubRegister::T9);
    const GPRReg target = jit.allocateScratch(StubRegister::T10);
    const GPRReg used = jit.allocateScratch(StubRegister::T11);
    const GPRReg position = jit.allocateScratch(StubRegister::T12);
    const GPRReg table = jit.allocateScratch(StubRegister::T13);
    const GPRReg column = jit.allocateScratch(StubRegister::T14);
    const GPRReg member = jit.allocateScratch(firstStubOperandRegister);
    const GPRReg entry = table;
    const GPRReg kind = used;
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    RELEASE_ASSERT(noOverlap(array, set, newLength, target, used, position, table, column, member));
    const Address flags(table, CompactTable::offsetOfKeysShift());
    const BaseIndex written(target, position, CCallHelpers::TimesEight);
    const Address lengthOfTarget(target, Butterfly::offsetOfPublicLength());
    JumpList slowPath;
    JumpList findsOtherKind;

    Label start = jit.label();
    jit.loadPtr(Address(set, offsetOfHashTableStorage()), table);
    Jump hasNoTable = jit.branchTestPtr(CCallHelpers::Zero, table);
    slowPath.append(jit.branch8(CCallHelpers::Equal, Address(table, JSCell::typeInfoTypeOffset()), TrustedImm32(JSCellButterflyType)));
    slowPath.append(jit.branchTest8(CCallHelpers::NonZero, flags, TrustedImm32(CompactTable::keysAreInImage)));
    jit.load32(Address(table, CompactTable::offsetOfAliveCount()), newLength);
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, newLength);
    if (takesValues) {
        slowPath.append(jit.branch32(CCallHelpers::NotEqual, newLength, Address(table, CompactTable::offsetOfUsed())));
        slowPath.append(jit.branchTest8(CCallHelpers::Zero, Address(table, CompactTable::offsetOfValueWidth()), TrustedImm32(static_cast<int32_t>(CompactTable::Width::Narrow) | static_cast<int32_t>(CompactTable::Width::Wide))));
    }
    jit.loadPtr(Address(array, JSObject::butterflyOffset()), target);
    jit.load32(lengthOfTarget, position);
    jit.add32(position, newLength);
    Jump hasNoRoom = jit.branch32(CCallHelpers::Above, newLength, Address(target, Butterfly::offsetOfVectorLength()));
    jit.load8(flags, member);
    jit.and32(TrustedImm32(CompactTable::keysShiftMask), member);
    jit.move(TrustedImm32(2), column);
    jit.lshift64(member, column);
    jit.add64(table, column);
    jit.addPtr(TrustedImm32(CompactTable::offsetOfIndex()), column);
    if (takesValues) {
        jit.load32(Address(table, CompactTable::offsetOfCapacity()), member);
        Jump keysAreWide = jit.branchTest8(CCallHelpers::NonZero, flags, TrustedImm32(CompactTable::keysAreWide));
        jit.lshift64(member, TrustedImm32(2), member);
        Jump isBehindKeys = jit.jump();
        keysAreWide.link(&jit);
        jit.lshift64(member, TrustedImm32(3), member);
        isBehindKeys.link(&jit);
        jit.add64(member, column);
    }
    auto branchIfColumnIs = [&](bool wide) {
        CCallHelpers::ResultCondition asked = wide ? CCallHelpers::NonZero : CCallHelpers::Zero;
        if (takesValues)
            return jit.branchTest8(asked, Address(table, CompactTable::offsetOfValueWidth()), TrustedImm32(static_cast<int32_t>(CompactTable::Width::Wide)));
        return jit.branchTest8(asked, flags, TrustedImm32(CompactTable::keysAreWide));
    };

    auto forEachMember = [&](bool keysAreWide, NOESCAPE const Invocable<void()> auto& store, NOESCAPE const Invocable<void()> auto& finish) {
        Jump hasDeletedEntries;
        if (!takesValues) {
            jit.load32(Address(table, CompactTable::offsetOfUsed()), used);
            hasDeletedEntries = jit.branch32(CCallHelpers::NotEqual, used, Address(table, CompactTable::offsetOfAliveCount()));
        }
        jit.lshift64(position, TrustedImm32(keysAreWide ? 3 : 2), member);
        jit.subPtr(member, column);
        Label copy = jit.label();
        if (keysAreWide)
            jit.load64(BaseIndex(column, position, CCallHelpers::TimesEight), member);
        else
            jit.load32(BaseIndex(column, position, CCallHelpers::TimesFour), member);
        store();
        jit.add32(TrustedImm32(1), position);
        jit.branch32(CCallHelpers::Below, position, newLength).linkTo(copy, &jit);
        finish();
        if (takesValues)
            return;

        hasDeletedEntries.link(&jit);
        jit.move(TrustedImm32(0), entry);
        Label loop = jit.label();
        if (keysAreWide)
            jit.load64(BaseIndex(column, entry, CCallHelpers::TimesEight), member);
        else
            jit.load32(BaseIndex(column, entry, CCallHelpers::TimesFour), member);
        jit.add32(TrustedImm32(1), entry);
        Jump isDeleted = jit.branchTest64(CCallHelpers::Zero, member);
        store();
        jit.add32(TrustedImm32(1), position);
        isDeleted.link(&jit);
        jit.branch32(CCallHelpers::Below, entry, used).linkTo(loop, &jit);
        finish();
    };
    auto setLengthAndReturn = [&] {
        jit.store32(newLength, lengthOfTarget);
        jit.returnToCaller();
    };
    JumpList needsBarrier;
    auto rememberAndReturn = [&] {
        needsBarrier.append(branchIfBarrierIsNeeded(jit, array, member));
        jit.returnToCaller();
    };

    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), kind);
    Jump targetHoldsDoubles = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(DoubleShape));
    Jump targetHoldsInt32 = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(Int32Shape));
    slowPath.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(ContiguousShape)));
    jit.store32(newLength, lengthOfTarget);
    Jump keysAreWide = branchIfColumnIs(true);
    forEachMember(false, [&] {
        Jump isImmediate = jit.branch32(CCallHelpers::Below, member, TrustedImm32(minCell32));
        decodeCell32(jit, member, member);
        isImmediate.link(&jit);
        jit.store64(member, written);
    }, rememberAndReturn);
    keysAreWide.link(&jit);
    forEachMember(true, [&] {
        jit.store64(member, written);
    }, rememberAndReturn);

    needsBarrier.link(&jit);
    checkStubOperands(writeBarrier(), { member });
    jit.move(array, member);
    jit.jumpToStub(writeBarrier());

    targetHoldsInt32.link(&jit);
    slowPath.append(branchIfColumnIs(false));
    forEachMember(true, [&] {
        findsOtherKind.append(jit.branchIfNotInt32(member));
        jit.store64(member, written);
    }, setLengthAndReturn);

    targetHoldsDoubles.link(&jit);
    slowPath.append(branchIfColumnIs(false));
    forEachMember(true, [&] {
        Jump isInt32 = jit.branchIfInt32(member);
        findsOtherKind.append(jit.branchIfNotNumber(member));
        unboxDouble(jit, member, member, number);
        findsOtherKind.append(jit.branchIfNaN(number));
        Jump isDouble = jit.jump();
        isInt32.link(&jit);
        jit.convertInt32ToDouble(member, number);
        isDouble.link(&jit);
        jit.storeDouble(number, written);
    }, setLengthAndReturn);

    hasNoTable.link(&jit);
    isEmpty.link(&jit);
    jit.returnToCaller();

    findsOtherKind.link(&jit);
    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeMask), kind);
    jit.move(TrustedImm64(0), member);
    Jump holeIsEmptyValue = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(Int32Shape));
    jit.move(TrustedImm64(static_cast<int64_t>(PNaNAsBits)), member);
    holeIsEmptyValue.link(&jit);
    Label clear = jit.label();
    slowPath.append(jit.branch32(CCallHelpers::BelowOrEqual, position, lengthOfTarget));
    jit.sub32(TrustedImm32(1), position);
    jit.store64(member, written);
    jit.jump().linkTo(clear, &jit);

    hasNoRoom.link(&jit);
    growVectorFor(jit, { array, target, position, column, used, member }, newLength, slowPath);
    jit.jump().linkTo(start, &jit);

    slowPath.link(&jit);
    jit.callOperationAndReturn(takesValues ? Entry::operationAOTAppendValuesOfMap : Entry::operationAOTAppendMembers, { .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedRegisters });
}

void generateAppendMembers(StubAssembler& jit)
{
    generateAppendColumn(jit, ColumnAppended::Keys);
}

void generateAppendValuesOfMap(StubAssembler& jit)
{
    generateAppendColumn(jit, ColumnAppended::Values);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
