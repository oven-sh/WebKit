/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsBoxedCalls.h"

#if ENABLE(AOT)

#include "AOTAsyncAwaitConditions.h"
#include "AOTBuiltinCells.h"
#include "AOTBuiltinOperationSupport.h"
#include "AOTBuiltinTable.h"
#include "AOTCellFormat.h"
#include "AOTCompactTable.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTOperationData.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsArrays.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsBoxedCallsOfLanguage.h"
#include "AOTStubsBoxedCallsOfStrings.h"
#include "AOTStubsCallHost.h"
#include "AOTStubsCalls.h"
#include "AOTStubsCollections.h"
#include "AOTStubsCoroutines.h"
#include "AOTStubsHashTables.h"
#include "AOTStubsOperations.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTStubsRegExp.h"
#include "AOTStubsStrings.h"
#include "AOTVector.h"
#include "IndexingType.h"
#include "JSArray.h"
#include "JSArrayBufferView.h"
#include "JSBoundFunction.h"
#include "JSCallee.h"
#include "JSGlobalObject.h"
#include "JSMap.h"
#include "JSObject.h"
#include "JSPromise.h"
#include "JSSet.h"
#include "JSString.h"
#include "JSType.h"
#include "JSTypeInfo.h"
#include "JSWeakMap.h"
#include "JSWeakSet.h"
#include "NativeExecutable.h"
#include "PureNaN.h"
#include <algorithm>
#include <array>
#include <bit>
#include <iterator>
#include <limits>
#include <optional>
#include <span>

namespace JSC { namespace AOT {

static constexpr JSTypeRange arrayTypes { ArrayType, DerivedArrayType };
static_assert(DerivedArrayType == ArrayType + 1);
static constexpr JSTypeRange typedArrayTypes { static_cast<JSType>(FirstTypedArrayType), static_cast<JSType>(LastTypedArrayTypeExcludingDataView) };

constexpr int32_t hasNoBoxedCallEntry = UINT8_MAX;
static_assert(StubWords::numberOfBoxedCallEntries < hasNoBoxedCallEntry);
constexpr int32_t offsetOfBoxedCallTable = Instance::offsetOfBoxedCalls();
constexpr StubRegister boxedCallEntryRegister = StubRegister::T9;
constexpr int32_t mostValuesMovedByShiftInStub = 16;
constexpr int32_t mostValuesMovedBySpliceInStub = 32;
constexpr int32_t mostWordsSearchedInStub = 64;

void emitBoxedCallDispatch(StubAssembler& jit, const CallRegisters& registers, unsigned count, JumpList& hasNoBoxedCall)
{
    if (count < BoxedCallEntry::minArgumentCount || count > BoxedCallEntry::maxArgumentCount)
        return;
    static_assert(BoxedCallEntry::maxArgumentCount == 4);
    const GPRReg executable = registers.scratch;
    const GPRReg entry = jit.allocateScratch(boxedCallEntryRegister);
    RELEASE_ASSERT(noOverlap(executable, registers.word, entry, instanceGPR, calleeGPR, thisGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3)));
    constexpr unsigned numberOfForms = descriptorOf(StubFamily::CallBoxedBuiltin).numberOfForms;

    const Address indexOfBoxedCall(executable, NativeExecutable::offsetOfIndexOfBoxedCall());
    Label isLookedUp = jit.label();
    hasNoBoxedCall.append(jit.branch8(CCallHelpers::Equal, indexOfBoxedCall, TrustedImm32(hasNoBoxedCallEntry)));
    jit.load8(indexOfBoxedCall, entry);
    Jump isNotLookedUp = jit.branchTest32(CCallHelpers::Zero, entry);
    std::array<StubKey, numberOfForms> handlers;
    for (unsigned form = 0; form < numberOfForms; ++form)
        handlers[form] = callBoxedBuiltin(static_cast<BoxedCallForm>(form), count);
    jit.lshift32(TrustedImm32(BoxedCallEntry::sizeShift), entry);
    jit.jumpThroughHandlerTable(BaseIndex(instanceGPR, entry, CCallHelpers::TimesOne, offsetOfBoxedCallTable - static_cast<int32_t>(sizeof(BoxedCallEntry)) + static_cast<int32_t>(BoxedCallEntry::offsetOfTarget(count))), handlers);

    jit.addOutOfLinePart([=](StubAssembler& jit) {
        isNotLookedUp.link(&jit);
        jit.emitFunctionPrologue();
        jit.callStub(findBoxedCall());
        jit.emitFunctionEpilogue();
        jit.jump().linkTo(isLookedUp, &jit);
    });
}

StubSignature signatureOfFindBoxedCall(StubKey) { return callConventionSignature({ }); }

void generateFindBoxedCall(StubAssembler& jit)
{
    static_assert(BoxedCallEntry::sizeShift > StubWords::boxedCallHashShift);
    const CallRegisters registers = CallRegisters::allocate(jit);
    const GPRReg executable = registers.scratch;
    const GPRReg nativeFunction = registers.word;
    const GPRReg entry = jit.allocateScratch(boxedCallEntryRegister);
    constexpr int32_t offsetMask = (StubWords::numberOfBoxedCallEntries - 1) << BoxedCallEntry::sizeShift;
    const BaseIndex candidate(instanceGPR, entry, CCallHelpers::TimesOne, offsetOfBoxedCallTable + static_cast<int32_t>(BoxedCallEntry::offsetOfNativeFunction()));

    jit.loadPtr(Address(executable, NativeExecutable::offsetOfNativeFunctionFor(CodeSpecializationKind::CodeForCall)), nativeFunction);
    jit.lshift64(nativeFunction, TrustedImm32(BoxedCallEntry::sizeShift - StubWords::boxedCallHashShift), entry);
    Label next = jit.label();
    jit.and32(TrustedImm32(offsetMask), entry);
    Jump isFound = jit.branchPtr(CCallHelpers::Equal, candidate, nativeFunction);
    Jump isFree = jit.branchTestPtr(CCallHelpers::Zero, candidate);
    jit.add32(TrustedImm32(1 << BoxedCallEntry::sizeShift), entry);
    jit.jump().linkTo(next, &jit);

    isFound.link(&jit);
    jit.urshift32(TrustedImm32(BoxedCallEntry::sizeShift), entry);
    jit.add32(TrustedImm32(1), entry);
    Jump remember = jit.jump();
    isFree.link(&jit);
    jit.move(TrustedImm32(hasNoBoxedCallEntry), entry);
    remember.link(&jit);
    jit.store8(entry, Address(executable, NativeExecutable::offsetOfIndexOfBoxedCall()));
    jit.returnToCaller();
}

static StubSignature receiverOperationSignature(unsigned numberOfArguments)
{
    StubSignature signature = StubSignature::withGPRResult({ thisGPR });
    for (unsigned i = 0; i < numberOfArguments; ++i)
        signature.addOperand(argumentGPR(i));
    return signature;
}

StubSignature signatureOfCallBoxedBuiltin(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfWeakMapSet(StubKey) { return receiverOperationSignature(2); }
StubSignature signatureOfStringStartsWith(StubKey) { return receiverOperationSignature(1); }
StubSignature signatureOfStringIndexOf(StubKey) { return receiverOperationSignature(1); }

static bool isCalledByName(const StubAssembler& jit) { return jit.key().family() == StubFamily::CallMethodOfBuiltinClass || jit.key().family() == StubFamily::TryCallMethodOfBuiltinClass || jit.key().family() == StubFamily::CallMethodOfBuiltinClassWithSharedPattern || jit.key().family() == StubFamily::CallMethodThatMayBeUnread; }
static bool receiverOfBoxedCallIsObject(BoxedCallForm);

static void checkCalleeRealm(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    if (isCalledByName(jit)) {
        if (!receiverOfBoxedCallIsObject(static_cast<BoxedCallForm>(jit.key().form())))
            return;
        loadStructure(jit, thisGPR, scratch);
        jit.loadPtr(Address(scratch, Structure::realmOffset()), scratch);
        callsHostFunction.append(jit.branchPtr(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject))));
        return;
    }
    JumpList isOfThisInstance;
    if (jit.key().family() == StubFamily::CallBoxedBuiltin)
        isOfThisInstance.append(jit.branch64(CCallHelpers::Below, calleeGPR, TrustedImm32(limitOfNumbersOfBuiltinCells)));
    jit.loadPtr(Address(calleeGPR, JSCallee::offsetOfScopeChain()), scratch);
    callsHostFunction.append(jit.branchPtr(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject))));
    isOfThisInstance.link(&jit);
}

static void returnBoxedBoolean(StubAssembler& jit, bool value)
{
    jit.move(TrustedImm32(value ? JSValue::ValueTrue : JSValue::ValueFalse), jit.result());
    jit.returnToCaller();
}

static void emitArrayIsArray(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    Jump notCell = jit.branchIfNotCell(argumentGPR(0));
    jit.load8(Address(argumentGPR(0), JSCell::typeInfoTypeOffset()), scratch);
    callsHostFunction.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(ProxyObjectType)));
    jit.sub32(TrustedImm32(arrayTypes.first), scratch);
    Jump isArray = jit.branch32(CCallHelpers::BelowOrEqual, scratch, TrustedImm32(arrayTypes.last - arrayTypes.first));
    notCell.link(&jit);
    returnBoxedBoolean(jit, false);
    isArray.link(&jit);
    returnBoxedBoolean(jit, true);
}

static void emitObjectIs(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    constexpr GPRReg first = argumentGPR(0);
    constexpr GPRReg second = argumentGPR(1);
    Jump hasSameBits = jit.branch64(CCallHelpers::Equal, first, second);
    callsHostFunction.append(jit.branchIfNumber(first));
    JumpList differs;
    differs.append(jit.branchIfNotCell(first));
    differs.append(jit.branchIfNotCell(second));
    jit.load8(Address(first, JSCell::typeInfoTypeOffset()), scratch);
    callsHostFunction.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(StringType)));
    callsHostFunction.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(HeapBigIntType)));
    differs.link(&jit);
    returnBoxedBoolean(jit, false);
    hasSameBits.link(&jit);
    returnBoxedBoolean(jit, true);
}

static void emitObjectHasOwnProperty(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    const StubSignature target = signatureOf(hasOwnByValCachedBoxed());
    const GPRReg base = target.operands[0].gpr();
    const GPRReg key = target.operands[1].gpr();
    callsHostFunction.append(jit.branchIfNotCell(thisGPR));
    callsHostFunction.append(jit.branchIfNotObject(thisGPR));
    Jump keyIsNoCell = jit.branchIfNotCell(argumentGPR(0));
    callsHostFunction.append(jit.branchIfObject(argumentGPR(0)));
    keyIsNoCell.link(&jit);
    checkCalleeRealm(jit, scratch, callsHostFunction);
    jit.clobberRegister(base);
    jit.clobberRegister(key);
    jit.shuffleRegisters<GPRReg, 2>({ thisGPR, argumentGPR(0) }, { base, key });
    jit.jumpToStub(hasOwnByValCachedBoxed());
}

static void emitIsNaN(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    constexpr uint64_t infinityWithoutSign = std::bit_cast<uint64_t>(std::numeric_limits<double>::infinity()) << 1;
    constexpr GPRReg value = argumentGPR(0);
    Jump isInt32 = jit.branchIfInt32(value);
    callsHostFunction.append(jit.branchIfNotNumber(value));
    jit.move(value, scratch);
    jit.sub64(TrustedImm64(JSValue::DoubleEncodeOffset), scratch);
    jit.lshift64(TrustedImm32(1), scratch);
    Jump isNaN = jit.branch64(CCallHelpers::Above, scratch, TrustedImm64(static_cast<int64_t>(infinityWithoutSign)));
    isInt32.link(&jit);
    returnBoxedBoolean(jit, false);
    isNaN.link(&jit);
    returnBoxedBoolean(jit, true);
}

static void emitMathExtremum(StubAssembler& jit, GPRReg, JumpList& callsHostFunction)
{
    bool takesLarger = static_cast<BoxedCallForm>(jit.key().form()) == BoxedCallForm::MathMax;
    callsHostFunction.append(jit.branchIfNotInt32(argumentGPR(0)));
    callsHostFunction.append(jit.branchIfNotInt32(argumentGPR(1)));
    Jump isFirst = jit.branch32(takesLarger ? CCallHelpers::GreaterThanOrEqual : CCallHelpers::LessThanOrEqual, argumentGPR(0), argumentGPR(1));
    jit.move(argumentGPR(1), jit.result());
    jit.returnToCaller();
    isFirst.link(&jit);
    jit.move(argumentGPR(0), jit.result());
    jit.returnToCaller();
}

static void emitTableSize(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    const GPRReg table = scratch;
    jit.loadPtr(Address(thisGPR, offsetOfHashTableStorage()), table);
    Jump hasNoStorage = jit.branchTestPtr(CCallHelpers::Zero, table);
    callsHostFunction.append(jit.branchIfType(table, JSCellButterflyType));
    jit.load32(Address(table, CompactTable::offsetOfAliveCount()), table);
    boxInt32(jit, table, jit.result());
    jit.returnToCaller();

    hasNoStorage.link(&jit);
    jit.move(TrustedImm64(JSValue::encode(jsNumber(0))), jit.result());
    jit.returnToCaller();
}

static void emitTypedArrayLength(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    const GPRReg length = scratch;
    callsHostFunction.append(jit.branchTest8(CCallHelpers::NonZero, Address(thisGPR, JSArrayBufferView::offsetOfMode()), TrustedImm32(resizabilityAndAutoLengthMask)));
    jit.load64(Address(thisGPR, JSArrayBufferView::offsetOfLength()), length);
    callsHostFunction.append(jit.branch64(CCallHelpers::Above, length, TrustedImm32(std::numeric_limits<int32_t>::max())));
    boxInt32(jit, length, jit.result());
    jit.returnToCaller();
}

struct AccessOfDataView {
    BoxedCallForm form;
    TypedArrayType elements;
    bool stores;
};

static constexpr AccessOfDataView accessesOfDataView[] = {
    { BoxedCallForm::DataViewGetFloat32, TypeFloat32, false },
    { BoxedCallForm::DataViewGetFloat64, TypeFloat64, false },
    { BoxedCallForm::DataViewGetInt16, TypeInt16, false },
    { BoxedCallForm::DataViewGetInt32, TypeInt32, false },
    { BoxedCallForm::DataViewGetInt8, TypeInt8, false },
    { BoxedCallForm::DataViewGetUint16, TypeUint16, false },
    { BoxedCallForm::DataViewGetUint32, TypeUint32, false },
    { BoxedCallForm::DataViewGetUint8, TypeUint8, false },
    { BoxedCallForm::DataViewSetFloat32, TypeFloat32, true },
    { BoxedCallForm::DataViewSetFloat64, TypeFloat64, true },
    { BoxedCallForm::DataViewSetInt16, TypeInt16, true },
    { BoxedCallForm::DataViewSetInt32, TypeInt32, true },
    { BoxedCallForm::DataViewSetInt8, TypeInt8, true },
    { BoxedCallForm::DataViewSetUint16, TypeUint16, true },
    { BoxedCallForm::DataViewSetUint32, TypeUint32, true },
    { BoxedCallForm::DataViewSetUint8, TypeUint8, true },
};

static void emitDataViewAccess(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    const AccessOfDataView& access = *std::ranges::find(accessesOfDataView, static_cast<BoxedCallForm>(jit.key().form()), &AccessOfDataView::form);
    const unsigned size = elementSize(access.elements);
    const bool holdsFloat = isFloat(access.elements);
    const GPRReg order = argumentGPR(access.stores ? 2 : 1);
    const bool namesOrder = jit.key().count() > (access.stores ? 2u : 1u);
    const GPRReg vector = scratch;
    const GPRReg offset = jit.allocateScratch(StubRegister::T11);
    const GPRReg bits = jit.allocateScratch(StubRegister::T13);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    RELEASE_ASSERT(noOverlap(vector, offset, bits, calleeGPR, thisGPR, instanceGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2)));

    auto turnUnlessLittleEndian = [&] {
        if (size == 1)
            return;
        Jump isLittleEndian;
        if (namesOrder)
            isLittleEndian = jit.branch64(CCallHelpers::Equal, order, TrustedImm32(JSValue::ValueTrue));
        if (size == 2)
            jit.byteSwap16(bits);
        else if (size == 4)
            jit.byteSwap32(bits);
        else
            jit.byteSwap64(bits);
        if (namesOrder)
            isLittleEndian.link(&jit);
    };

    callsHostFunction.append(jit.branchIfNotInt32(argumentGPR(0)));
    callsHostFunction.append(jit.branch32(CCallHelpers::LessThan, argumentGPR(0), TrustedImm32(0)));
    if (access.stores)
        callsHostFunction.append(holdsFloat ? jit.branchIfNotNumber(argumentGPR(1)) : jit.branchIfNotInt32(argumentGPR(1)));
    if (namesOrder)
        callsHostFunction.append(jit.branchIfNotBoolean(order, bits));
    callsHostFunction.append(jit.branchTest8(CCallHelpers::NonZero, Address(thisGPR, JSArrayBufferView::offsetOfMode()), TrustedImm32(resizabilityAndAutoLengthMask)));
    jit.zeroExtend32ToWord(argumentGPR(0), offset);
    jit.add64(TrustedImm32(size), offset, bits);
    callsHostFunction.append(jit.branch64(CCallHelpers::Above, bits, Address(thisGPR, JSArrayBufferView::offsetOfLength())));
    jit.loadPtr(Address(thisGPR, JSArrayBufferView::offsetOfVector()), vector);
    const BaseIndex bytes(vector, offset, CCallHelpers::TimesOne);

    if (access.stores) {
        if (!holdsFloat)
            jit.move(argumentGPR(1), bits);
        else {
            toDouble(jit, argumentGPR(1), bits, number);
            if (size == 4) {
                jit.convertDoubleToFloat(number, number);
                jit.moveFloatTo32(number, bits);
            } else
                jit.moveDoubleTo64(number, bits);
        }
        turnUnlessLittleEndian();
        if (size == 1)
            jit.store8(bits, bytes);
        else if (size == 2)
            jit.store16(bits, bytes);
        else if (size == 4)
            jit.store32(bits, bytes);
        else
            jit.store64(bits, bytes);
        jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
        jit.returnToCaller();
        return;
    }

    if (size == 1)
        jit.load8(bytes, bits);
    else if (size == 2)
        jit.load16(bytes, bits);
    else if (size == 4)
        jit.load32(bytes, bits);
    else
        jit.load64(bytes, bits);
    turnUnlessLittleEndian();
    if (holdsFloat) {
        if (size == 4) {
            jit.move32ToFloat(bits, number);
            jit.convertFloatToDouble(number, number);
        } else
            jit.move64ToDouble(bits, number);
        boxDouble(jit, PureDouble::purify(jit, number, number), jit.result());
        jit.returnToCaller();
        return;
    }
    if (access.elements == TypeInt8)
        jit.signExtend8To32(bits, bits);
    else if (access.elements == TypeInt16)
        jit.signExtend16To32(bits, bits);
    else if (access.elements == TypeUint32) {
        Jump fitsInt32 = jit.branch32(CCallHelpers::GreaterThanOrEqual, bits, TrustedImm32(0));
        jit.zeroExtend32ToWord(bits, bits);
        boxDouble(jit, PureDouble::convertInt64(jit, bits, number), jit.result());
        jit.returnToCaller();
        fitsInt32.link(&jit);
    }
    boxInt32(jit, bits, jit.result());
    jit.returnToCaller();
}

static void emitObjectGetPrototypeOf(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    constexpr GPRReg object = argumentGPR(0);
    const GPRReg prototype = scratch;
    callsHostFunction.append(jit.branchIfNotCell(object));
    callsHostFunction.append(jit.branchIfNotObject(object));
    callsHostFunction.append(jit.branchTest8(CCallHelpers::NonZero, Address(object, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetPrototype)));
    loadStructure(jit, object, prototype);
    jit.load64(Address(prototype, Structure::prototypeOffset()), prototype);
    Jump hasPolyProto = jit.branchTest64(CCallHelpers::Zero, prototype);
    jit.move(prototype, jit.result());
    jit.returnToCaller();

    hasPolyProto.link(&jit);
    jit.load64(Address(object, offsetRelativeToBase(knownPolyProtoOffset)), jit.result());
    jit.returnToCaller();
}

static void emitJSONStringify(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    constexpr GPRReg value = argumentGPR(0);
    if (jit.key().count() == 2)
        callsHostFunction.append(jit.branchIfNotOther(argumentGPR(1), scratch));
    Jump isNotCell = jit.branchIfNotCell(value);
    callsHostFunction.append(jit.branchIfNotType(value, StringType));
    Label hasText = jit.label();
    checkCalleeRealm(jit, scratch, callsHostFunction);
    jit.callOperationAndReturn(Entry::operationAOTJSONStringifyPrimitive, { .secondArgumentIn = value });

    isNotCell.link(&jit);
    jit.branch64(CCallHelpers::NotEqual, value, TrustedImm64(JSValue::ValueUndefined)).linkTo(hasText, &jit);
    jit.move(value, jit.result());
    jit.returnToCaller();
}

static void emitStringTrim(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    constexpr int32_t firstKeptCharacter = 0x21;
    constexpr int32_t lastKeptCharacter = 0x7e;
    const GPRReg characters = scratch;
    const GPRReg length = jit.allocateScratch(StubRegister::T11);
    const GPRReg lastCharacter = length;
    const GPRReg firstCharacter = characters;
    RELEASE_ASSERT(noOverlap(characters, length, calleeGPR, thisGPR, instanceGPR));

    JumpList mayChange;
    auto leaveUnlessKept = [&](GPRReg character) {
        jit.sub32(TrustedImm32(firstKeptCharacter), character);
        mayChange.append(jit.branch32(CCallHelpers::Above, character, TrustedImm32(lastKeptCharacter - firstKeptCharacter)));
    };
    jit.loadPtr(Address(thisGPR, JSString::offsetOfValue()), characters);
    mayChange.append(jit.branchIfRopeStringImpl(characters));
    mayChange.append(jit.branchTest32(CCallHelpers::Zero, Address(characters, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit())));
    jit.load32(Address(characters, StringImpl::lengthMemoryOffset()), length);
    mayChange.append(jit.branchTest32(CCallHelpers::Zero, length));
    jit.loadPtr(Address(characters, StringImpl::dataOffset()), characters);
    jit.load8(BaseIndex(characters, length, CCallHelpers::TimesOne, -1), lastCharacter);
    leaveUnlessKept(lastCharacter);
    jit.load8(Address(characters), firstCharacter);
    leaveUnlessKept(firstCharacter);
    jit.move(thisGPR, jit.result());
    jit.returnToCaller();

    mayChange.link(&jit);
    checkCalleeRealm(jit, scratch, callsHostFunction);
    OperationCall call { .secondArgumentIn = thisGPR };
    call.constantArguments[0] = OperationCall::ConstantArgument { operationArgumentGPR(2), static_cast<uint32_t>(TrimmedEnds::Both) };
    jit.callOperationAndReturn(Entry::operationAOTStringTrim, call);
}




static void emitArraySearch(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    constexpr GPRReg searched = argumentGPR(0);
    const GPRReg cursor = jit.reuseScratch(StubRegister::A1);
    const GPRReg flags = jit.reuseScratch(StubRegister::A2);
    const GPRReg element = jit.reuseScratch(StubRegister::A3);
    const GPRReg searchedText = jit.allocateScratch(StubRegister::T11);
    const GPRReg text = jit.allocateScratch(StubRegister::T13);
    const GPRReg shape = scratch;
    const GPRReg searchedLength = scratch;
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg numberScratch = jit.allocateScratch(FPRInfo::fpRegT1);
    RELEASE_ASSERT(noOverlap(searched, cursor, flags, element, searchedText, text, scratch, calleeGPR, thisGPR, instanceGPR));

    auto loadSearchedText = [&] {
        jit.loadPtr(Address(searched, JSString::offsetOfValue()), searchedText);
    };
    auto loadSearchedLength = [&] {
        jit.load32(Address(searchedText, StringImpl::lengthMemoryOffset()), searchedLength);
    };

    callsHostFunction.append(branchIfNotPristine(jit, thisGPR));
    jit.load8(Address(thisGPR, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask), shape);
    jit.move(TrustedImm32(0), searchedText);

    JumpList isReady;
    Jump isInt32 = jit.branchIfInt32(searched);
    Jump holdsNoValues = jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(ContiguousShape));
    callsHostFunction.append(jit.branchIfNumber(searched));
    callsHostFunction.append(jit.branch64(CCallHelpers::Equal, searched, TrustedImm64(JSValue::ValueUndefined)));
    isReady.append(jit.branchIfNotCell(searched));
    jit.load8(Address(searched, JSCell::typeInfoTypeOffset()), element);
    callsHostFunction.append(jit.branch32(CCallHelpers::Equal, element, TrustedImm32(HeapBigIntType)));
    isReady.append(jit.branch32(CCallHelpers::NotEqual, element, TrustedImm32(StringType)));
    loadSearchedText();
    callsHostFunction.append(jit.branchIfRopeStringImpl(searchedText));
    loadSearchedLength();
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), cursor);
    Jump seeksTextAmongAnyNumber = jit.jump();
    isInt32.link(&jit);
    Jump isAmongInt32s = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(Int32Shape));
    Jump seeksInt32AmongNoValues = jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(ContiguousShape));
    isAmongInt32s.link(&jit);
    isReady.link(&jit);
    Label seeksWord = jit.label();

    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), cursor);
    callsHostFunction.append(jit.branch32(CCallHelpers::Above, Address(cursor, Butterfly::offsetOfPublicLength()), TrustedImm32(mostWordsSearchedInStub)));
    seeksTextAmongAnyNumber.link(&jit);

    const GPRReg end = flags;
    auto loadEnd = [&] {
        jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), element);
        jit.load32(Address(element, Butterfly::offsetOfPublicLength()), end);
        jit.getEffectiveAddress(BaseIndex(element, end, CCallHelpers::TimesEight), end);
    };
    JumpList isFound;
    JumpList isAtEnd;
    loadEnd();
    isAtEnd.append(jit.branchPtr(CCallHelpers::AboveOrEqual, cursor, end));
    Jump seeksText = jit.branchTestPtr(CCallHelpers::NonZero, searchedText);
    Jump seeksNoInt32 = jit.branchIfNotInt32(searched);
    Jump seeksAmongValues = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(ContiguousShape));
    seeksNoInt32.link(&jit);

    Label nextWord = jit.label();
    isFound.append(jit.branch64(CCallHelpers::Equal, Address(cursor), searched));
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), cursor);
    jit.branchPtr(CCallHelpers::Below, cursor, end).linkTo(nextWord, &jit);
    isAtEnd.append(jit.jump());

    seeksAmongValues.link(&jit);
    const GPRReg searchedAsDouble = searchedText;
    jit.signExtend32To64(searched, text);
    callsHostFunction.append(jit.branchTest32(CCallHelpers::Zero, text));
    boxDouble(jit, PureDouble::convertInt64(jit, text, CCallHelpers::fpTempRegister), searchedAsDouble);
    Label nextValue = jit.label();
    jit.load64(Address(cursor), element);
    isFound.append(jit.branch64(CCallHelpers::Equal, element, searched));
    isFound.append(jit.branch64(CCallHelpers::Equal, element, searchedAsDouble));
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), cursor);
    jit.branchPtr(CCallHelpers::Below, cursor, end).linkTo(nextValue, &jit);
    isAtEnd.append(jit.jump());

    seeksText.link(&jit);
    JumpList isAnother;
    Label nextText = jit.label();
    jit.load64(Address(cursor), element);
    isAnother.append(jit.branchIfNotCell(element));
    isAnother.append(jit.branchTest64(CCallHelpers::Zero, element));
    isAnother.append(jit.branchIfNotType(element, StringType));
    jit.loadPtr(Address(element, JSString::offsetOfValue()), text);
    callsHostFunction.append(jit.branchIfRopeStringImpl(text));
    Jump hasSameLength = jit.branch32(CCallHelpers::Equal, Address(text, StringImpl::lengthMemoryOffset()), searchedLength);
    isAnother.link(&jit);
    Label passOver = jit.label();
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), cursor);
    jit.branchPtr(CCallHelpers::Below, cursor, end).linkTo(nextText, &jit);
    isAtEnd.append(jit.jump());

    hasSameLength.link(&jit);
    isFound.append(jit.branchPtr(CCallHelpers::Equal, text, searchedText));
    JumpList differs;
    jit.load32(Address(text, StringImpl::flagsOffset()), flags);
    jit.and32(Address(searchedText, StringImpl::flagsOffset()), flags);
    differs.append(jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(StringImpl::flagIsAtom())));
    branchIfCharactersDiffer(jit, { text, searchedText, searchedLength, flags, element }, differs, callsHostFunction);
    isFound.append(jit.jump());
    differs.link(&jit);
    loadSearchedText();
    loadSearchedLength();
    loadEnd();
    jit.jump().linkTo(passOver, &jit);

    static_assert(!NoIndexingShape && UndecidedShape < Int32Shape && Int32Shape < DoubleShape && DoubleShape < ContiguousShape && ContiguousShape < ArrayStorageShape);
    auto leaveSparseKindsToHostFunction = [&] {
        callsHostFunction.append(jit.branch32(CCallHelpers::Above, shape, TrustedImm32(DoubleShape)));
        callsHostFunction.append(jit.branchTest32(CCallHelpers::Zero, shape));
    };
    auto startAtFirstOfFew = [&] {
        jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), cursor);
        callsHostFunction.append(jit.branch32(CCallHelpers::Above, Address(cursor, Butterfly::offsetOfPublicLength()), TrustedImm32(mostWordsSearchedInStub)));
        loadEnd();
        isAtEnd.append(jit.branchPtr(CCallHelpers::AboveOrEqual, cursor, end));
    };
    auto goOnAt = [&](Label next) {
        jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), cursor);
        jit.branchPtr(CCallHelpers::Below, cursor, end).linkTo(next, &jit);
        isAtEnd.append(jit.jump());
    };

    holdsNoValues.link(&jit);
    leaveSparseKindsToHostFunction();
    Jump seeksUndefined = jit.branch64(CCallHelpers::Equal, searched, TrustedImm64(JSValue::ValueUndefined));
    isAtEnd.append(jit.branchIfNotNumber(searched));
    isAtEnd.append(jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(UndecidedShape)));
    unboxDouble(jit, searched, element, number);
    Jump seeksAmongFractions = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(DoubleShape));
    jit.branchConvertDoubleToInt32(number, element, isAtEnd, numberScratch, false);
    boxInt32(jit, element, searched);
    jit.jump().linkTo(seeksWord, &jit);

    seeksInt32AmongNoValues.link(&jit);
    leaveSparseKindsToHostFunction();
    isAtEnd.append(jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(UndecidedShape)));
    jit.convertInt32ToDouble(searched, number);
    seeksAmongFractions.link(&jit);
    startAtFirstOfFew();
    Label nextFraction = jit.label();
    jit.loadDouble(Address(cursor), numberScratch);
    isFound.append(jit.branchDouble(CCallHelpers::DoubleEqualAndOrdered, numberScratch, number));
    goOnAt(nextFraction);

    seeksUndefined.link(&jit);
    if (static_cast<BoxedCallForm>(jit.key().form()) == BoxedCallForm::ArrayIndexOf)
        isAtEnd.append(jit.jump());
    else {
        startAtFirstOfFew();
        isFound.append(jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(UndecidedShape)));
        Jump seeksHoleAmongFractions = jit.branch32(CCallHelpers::Equal, shape, TrustedImm32(DoubleShape));
        Label nextWholeNumber = jit.label();
        isFound.append(jit.branchTest64(CCallHelpers::Zero, Address(cursor)));
        goOnAt(nextWholeNumber);
        seeksHoleAmongFractions.link(&jit);
        Label nextPlace = jit.label();
        jit.loadDouble(Address(cursor), number);
        isFound.append(jit.branchDouble(CCallHelpers::DoubleNotEqualOrUnordered, number, number));
        goOnAt(nextPlace);
    }

    if (static_cast<BoxedCallForm>(jit.key().form()) == BoxedCallForm::ArrayIncludes) {
        isAtEnd.link(&jit);
        returnBoxedBoolean(jit, false);
        isFound.link(&jit);
        returnBoxedBoolean(jit, true);
        return;
    }
    isAtEnd.link(&jit);
    jit.move(TrustedImm64(JSValue::encode(jsNumber(-1))), jit.result());
    jit.returnToCaller();
    isFound.link(&jit);
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), element);
    jit.subPtr(element, cursor);
    jit.urshiftPtr(TrustedImm32(3), cursor);
    boxInt32(jit, cursor, jit.result());
    jit.returnToCaller();
}

static void emitObjectSetPrototypeOf(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(sizeof(PrototypeTransition) == 16 && hasOneBitSet(numberOfPrototypeTransitions));
    static_assert(indexOfPrototypeTransition(5 << prototypeTransitionHashShift, 3 << prototypeTransitionHashShift) == 6);
    constexpr GPRReg object = argumentGPR(0);
    constexpr GPRReg prototype = argumentGPR(1);
    const GPRReg transition = scratch;
    const GPRReg structureID = jit.allocateScratch(StubRegister::T11);
    RELEASE_ASSERT(noOverlap(object, prototype, transition, structureID, calleeGPR, thisGPR, instanceGPR));

    callsHostFunction.append(jit.branchIfNotCell(object));
    jit.load32(Address(object, JSCell::structureIDOffset()), structureID);
    jit.xor32(prototype, structureID, transition);
    jit.urshift32(TrustedImm32(prototypeTransitionHashShift), transition);
    jit.and32(TrustedImm32(numberOfPrototypeTransitions - 1), transition);
    jit.lshift32(TrustedImm32(4), transition);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(objects.prototypeTransitions)), transition);
    JumpList isNotInFirstHalf;
    isNotInFirstHalf.append(jit.branch64(CCallHelpers::NotEqual, Address(transition, OBJECT_OFFSETOF(PrototypeTransition, prototype)), prototype));
    isNotInFirstHalf.append(jit.branch32(CCallHelpers::NotEqual, Address(transition, OBJECT_OFFSETOF(PrototypeTransition, before)), structureID));
    callsHostFunction.append(jit.branchTest32(CCallHelpers::NonZero, Address(object, offsetOfOrderWord), TrustedImm32(static_cast<int32_t>(orderWordIsNotExtensible))));
    jit.load32(Address(transition, OBJECT_OFFSETOF(PrototypeTransition, after)), structureID);
    jit.store32(structureID, Address(object, JSCell::structureIDOffset()));
    jit.load8(Address(object, JSCell::typeInfoFlagsOffset()), structureID);
    jit.or32(TrustedImm32(IsPristine), structureID);
    jit.store8(structureID, Address(object, JSCell::typeInfoFlagsOffset()));
    Jump isLinked = jit.jump();

    isNotInFirstHalf.link(&jit);
    jit.addPtr(TrustedImm32(numberOfPrototypeTransitions * sizeof(PrototypeTransition)), transition);
    callsHostFunction.append(jit.branch64(CCallHelpers::NotEqual, Address(transition, OBJECT_OFFSETOF(PrototypeTransition, prototype)), prototype));
    callsHostFunction.append(jit.branch32(CCallHelpers::NotEqual, Address(transition, OBJECT_OFFSETOF(PrototypeTransition, before)), structureID));
    callsHostFunction.append(jit.branchTest32(CCallHelpers::NonZero, Address(object, offsetOfOrderWord), TrustedImm32(static_cast<int32_t>(orderWordIsNotExtensible))));
    jit.load32(Address(transition, OBJECT_OFFSETOF(PrototypeTransition, after)), structureID);
    jit.store32(structureID, Address(object, JSCell::structureIDOffset()));

    isLinked.link(&jit);
    Jump needsBarrier = branchIfBarrierIsNeeded(jit, object, scratch);
    jit.move(object, jit.result());
    jit.returnToCaller();

    needsBarrier.link(&jit);
    jit.emitFunctionPrologue();
    jit.move(object, gprOf(firstStubOperandRegister));
    jit.callStub(writeBarrier());
    jit.move(object, jit.result());
    jit.emitFunctionEpilogue();
    jit.ret();
}

static void pushSeveralOntoPlainStorageWithRoom(StubAssembler& jit, GPRReg kind)
{
    const unsigned count = jit.key().count();
    const GPRReg butterfly = jit.allocateScratch(StubRegister::T11);
    const GPRReg length = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(noOverlap(kind, butterfly, length, thisGPR, calleeGPR, instanceGPR));
    RELEASE_ASSERT(noOverlap(butterfly, length, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3)));
    JumpList isLeftToOperation;
    jit.load8(Address(thisGPR, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), kind);
    Jump holdsValues = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(ContiguousShape));
    isLeftToOperation.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(Int32Shape)));
    for (unsigned i = 0; i < count; ++i)
        isLeftToOperation.append(jit.branchIfNotInt32(argumentGPR(i)));
    Label store = jit.label();
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), butterfly);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
    jit.add32(TrustedImm32(static_cast<int32_t>(count)), length);
    isLeftToOperation.append(jit.branch32(CCallHelpers::Above, length, Address(butterfly, Butterfly::offsetOfVectorLength())));
    for (unsigned i = 0; i < count; ++i)
        jit.store64(argumentGPR(i), BaseIndex(butterfly, length, CCallHelpers::TimesEight, (static_cast<int32_t>(i) - static_cast<int32_t>(count)) * static_cast<int32_t>(sizeof(EncodedJSValue))));
    jit.store32(length, Address(butterfly, Butterfly::offsetOfPublicLength()));
    boxInt32(jit, length, jit.result());
    jit.returnToCaller();

    holdsValues.link(&jit);
    JumpList holdsCell;
    for (unsigned i = 0; i < count; ++i)
        holdsCell.append(jit.branchIfCell(argumentGPR(i)));
    jit.jump().linkTo(store, &jit);
    holdsCell.link(&jit);
    isLeftToOperation.append(branchIfBarrierIsNeeded(jit, thisGPR, kind));
    jit.jump().linkTo(store, &jit);
    isLeftToOperation.link(&jit);
}

static void pushOntoPlainStorageWithRoom(StubAssembler& jit, GPRReg kind)
{
    const GPRReg value = argumentGPR(0);
    const GPRReg butterfly = jit.reuseScratch(StubRegister::A1);
    const GPRReg length = jit.reuseScratch(StubRegister::A2);
    RELEASE_ASSERT(noOverlap(kind, butterfly, length, thisGPR, value));
    JumpList isLeftToStub;
    jit.load8(Address(thisGPR, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), kind);
    Jump holdsValues = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(ContiguousShape));
    Jump holdsNoIntegers = jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(Int32Shape));
    isLeftToStub.append(jit.branchIfNotInt32(value));
    Label store = jit.label();
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), butterfly);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
    isLeftToStub.append(jit.branch32(CCallHelpers::AboveOrEqual, length, Address(butterfly, Butterfly::offsetOfVectorLength())));
    jit.store64(value, BaseIndex(butterfly, length, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), length);
    jit.store32(length, Address(butterfly, Butterfly::offsetOfPublicLength()));
    boxInt32(jit, length, jit.result());
    jit.returnToCaller();

    holdsNoIntegers.link(&jit);
    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    isLeftToStub.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(DoubleShape)));
    Jump isInt32 = jit.branchIfInt32(value);
    isLeftToStub.append(jit.branchIfNotNumber(value));
    unboxDouble(jit, value, kind, number);
    isLeftToStub.append(jit.branchIfNaN(number));
    Jump isUnboxed = jit.jump();
    isInt32.link(&jit);
    jit.convertInt32ToDouble(value, number);
    isUnboxed.link(&jit);
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), butterfly);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
    isLeftToStub.append(jit.branch32(CCallHelpers::AboveOrEqual, length, Address(butterfly, Butterfly::offsetOfVectorLength())));
    jit.storeDouble(number, BaseIndex(butterfly, length, CCallHelpers::TimesEight));
    jit.add32(TrustedImm32(1), length);
    jit.store32(length, Address(butterfly, Butterfly::offsetOfPublicLength()));
    boxInt32(jit, length, jit.result());
    jit.returnToCaller();

    holdsValues.link(&jit);
    jit.branchIfNotCell(value).linkTo(store, &jit);
    isLeftToStub.append(branchIfBarrierIsNeeded(jit, thisGPR, kind));
    jit.jump().linkTo(store, &jit);
    isLeftToStub.link(&jit);
}

static void emitArrayPush(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(noOverlap(gprOf(StubRegister::A2), thisGPR) && noOverlap(gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(arrayPushKindRegister)));

    if (pushesSeveralValues(jit.key().count()))
        pushSeveralOntoPlainStorageWithRoom(jit, scratch);
    if (!pushesSeveralValues(jit.key().count()))
        pushOntoPlainStorageWithRoom(jit, scratch);
    checkCalleeRealm(jit, scratch, callsHostFunction);
    if (jit.key().count() > 1)
        return emitArrayPushOfSeveral(jit);

    const GPRReg array = jit.reuseScratch(StubRegister::A1);
    const GPRReg value = jit.reuseScratch(StubRegister::A2);
    const GPRReg kind = jit.reuseScratch(arrayPushKindRegister);
    checkStubOperands(arrayPushOfKnownKind(), { array, value, kind });
    jit.move(argumentGPR(0), value);
    jit.move(thisGPR, array);
    jit.load8(Address(array, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), kind);
    jit.jumpToStub(arrayPushOfKnownKind());
}

static void emitArraySlice(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(argumentGPR(1) == gprOf(StubRegister::A1) && noOverlap(gprOf(StubRegister::A2), gprOf(StubRegister::A3), thisGPR, argumentGPR(0), argumentGPR(1)));
    const unsigned count = jit.key().count();

    callsHostFunction.append(branchIfNotPristine(jit, thisGPR));
    checkCalleeRealm(jit, scratch, callsHostFunction);
    if (count >= 1)
        callsHostFunction.append(jit.branchIfNotInt32(argumentGPR(0)));
    if (count == 2)
        callsHostFunction.append(jit.branchIfNotInt32(argumentGPR(1)));

    const GPRReg array = jit.reuseScratch(StubRegister::A1);
    const GPRReg start = jit.reuseScratch(StubRegister::A2);
    const GPRReg end = jit.reuseScratch(StubRegister::A3);
    checkStubOperands(arraySlice(), { array, start, end });
    if (count == 2)
        jit.signExtend32ToPtr(argumentGPR(1), end);
    else
        jit.move(TrustedImm64(arraySliceToEnd), end);
    if (count >= 1)
        jit.signExtend32ToPtr(argumentGPR(0), start);
    else
        jit.move(TrustedImm32(0), start);
    jit.move(thisGPR, array);
    jit.jumpToStub(arraySlice());
}

static void emitEnterCompiledSort(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(Int32Shape + 2 == DoubleShape && DoubleShape + 2 == ContiguousShape);
    const GPRReg word = jit.clobberRegister(StubRegister::T11);
    const GPRReg code = jit.clobberRegister(StubRegister::T13);
    RELEASE_ASSERT(noOverlap(word, code, scratch, calleeGPR, thisGPR, argumentGPR(0), instanceGPR));
    callsHostFunction.append(jit.branchIfNotCell(argumentGPR(0)));
    Jump isFunction = jit.branchIfType(argumentGPR(0), jsFunctionTypes);
    callsHostFunction.append(jit.branchIfNotType(argumentGPR(0), slimClosureTypes));
    isFunction.link(&jit);
    callsHostFunction.append(jit.branchIfNotType(thisGPR, ArrayType));
    callsHostFunction.append(branchIfNotPristine(jit, thisGPR));
    loadStructure(jit, thisGPR, scratch);
    callsHostFunction.append(branchIfNotOfThisRealm(jit, scratch));
    checkCalleeRealm(jit, scratch, callsHostFunction);
    jit.load8(Address(thisGPR, JSCell::indexingTypeAndMiscOffset()), scratch);
    jit.and32(TrustedImm32(IndexingShapeMask), scratch);
    jit.sub32(TrustedImm32(Int32Shape), scratch);
    callsHostFunction.append(jit.branch32(CCallHelpers::Above, scratch, TrustedImm32(ContiguousShape - Int32Shape)));
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(stubs.wordOfCompiledSort)), word);
    callsHostFunction.append(jit.branchTest64(CCallHelpers::Zero, word));
    emitFillMissingArgumentRegisters(jit, 1);
    jit.move(TrustedImm32(1), countGPR);
    toCodeAddress(jit, word, code, StackIsTested::No);
    jit.farJump(word, JSEntryPtrTag);
}

static void emitArraySort(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    if (jit.key().count() == 1) {
        Jump isInDefaultOrder = jit.branch64(CCallHelpers::Equal, argumentGPR(0), TrustedImm64(JSValue::ValueUndefined));
        emitEnterCompiledSort(jit, scratch, callsHostFunction);
        isInDefaultOrder.link(&jit);
    }
    jit.load8(Address(thisGPR, JSCell::indexingTypeAndMiscOffset()), scratch);
    jit.and32(TrustedImm32(IndexingShapeMask), scratch);
    callsHostFunction.append(jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(ContiguousShape)));
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), scratch);
    callsHostFunction.append(jit.branch32(CCallHelpers::Above, Address(scratch, Butterfly::offsetOfPublicLength()), TrustedImm32(maxLengthOfInsertionSortedArray)));
    callsHostFunction.append(jit.branchTest32(CCallHelpers::Zero, Address(scratch, Butterfly::offsetOfPublicLength())));
    jit.load64(Address(scratch), scratch);
    callsHostFunction.append(jit.branchTest64(CCallHelpers::Zero, scratch));
    callsHostFunction.append(jit.branchIfNotCell(scratch));
    callsHostFunction.append(jit.branchIfNotType(scratch, StringType));
    checkCalleeRealm(jit, scratch, callsHostFunction);
    jit.callOperationAndReturn(Entry::operationAOTArraySortInDefaultOrder, { .secondArgumentIn = thisGPR });
}

static void emitArraySplice(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(argumentGPR(0) == operationArgumentGPR(0) && argumentGPR(1) == operationArgumentGPR(1) && argumentGPR(2) == operationArgumentGPR(2));
    static_assert(noOverlap(thisGPR, operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3), operationArgumentGPR(4)));
    static constexpr std::array operands { StubRegister::A1, StubRegister::A2, StubRegister::A3, StubRegister::A4 };
    static_assert(gprOf(operands[0]) == operationArgumentGPR(1) && gprOf(operands[1]) == operationArgumentGPR(2) && gprOf(operands[2]) == operationArgumentGPR(3) && gprOf(operands[3]) == operationArgumentGPR(4));
    const unsigned count = jit.key().count();
    RELEASE_ASSERT(count == 2 || count == 3);
    checkCalleeRealm(jit, scratch, callsHostFunction);

    static_assert(areDistinctOnEveryISA(StubRegister::A0, StubRegister::A1, StubRegister::A2, StubRegister::A3, StubRegister::A5, StubRegister::T13));
    const bool inserts = count == 3;
    constexpr GPRReg start = argumentGPR(0);
    constexpr GPRReg deleteCount = argumentGPR(1);
    constexpr GPRReg item = argumentGPR(2);
    const GPRReg kind = jit.clobberRegister(calleeGPR);
    const GPRReg from = jit.reuseScratch(StubRegister::A3);
    const GPRReg butterfly = jit.reuseScratch(StubRegister::A5);
    const GPRReg length = jit.clobberRegister(StubRegister::T13);
    const Address publicLength(butterfly, Butterfly::offsetOfPublicLength());
    RELEASE_ASSERT(noOverlap(kind, from, butterfly, length, scratch, thisGPR, start, deleteCount, item, instanceGPR));

    JumpList isLeftToOperation;
    isLeftToOperation.append(jit.branch64(CCallHelpers::NotEqual, deleteCount, TrustedImm64(JSValue::encode(jsNumber(inserts ? 0 : 1)))));
    isLeftToOperation.append(jit.branchIfNotInt32(start));
    isLeftToOperation.append(branchIfNotPristine(jit, thisGPR));
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), butterfly);
    jit.load32(publicLength, length);
    if (inserts)
        isLeftToOperation.append(jit.branch32(CCallHelpers::AboveOrEqual, length, Address(butterfly, Butterfly::offsetOfVectorLength())));
    jit.signExtend32ToPtr(start, from);
    Jump countsFromFront = jit.branch64(CCallHelpers::GreaterThanOrEqual, from, TrustedImm32(0));
    jit.add64(length, from);
    Jump liesInList = jit.branch64(CCallHelpers::GreaterThanOrEqual, from, TrustedImm32(0));
    jit.move(TrustedImm32(0), from);
    liesInList.link(&jit);
    countsFromFront.link(&jit);
    jit.move(length, scratch);
    jit.sub64(from, scratch);
    if (!inserts)
        jit.sub64(TrustedImm32(1), scratch);
    isLeftToOperation.append(jit.branch64(CCallHelpers::Above, scratch, TrustedImm32(mostValuesMovedBySpliceInStub)));

    jit.load8(Address(thisGPR, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), kind);
    Jump holdsValues = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(ContiguousShape));
    isLeftToOperation.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(Int32Shape)));
    if (inserts)
        isLeftToOperation.append(jit.branchIfNotInt32(item));
    Jump holdsIntegers = jit.jump();
    holdsValues.link(&jit);
    isLeftToOperation.append(jit.branchTest8(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(mutatorShouldBeFenced))));
    if (inserts) {
        Jump isNoCell = jit.branchIfNotCell(item);
        isLeftToOperation.append(branchIfBarrierIsNeeded(jit, thisGPR, scratch));
        isNoCell.link(&jit);
    }
    holdsIntegers.link(&jit);
    if (!jit.options().hasOneInstance) {
        loadStructure(jit, thisGPR, scratch);
        isLeftToOperation.append(branchIfNotOfThisRealm(jit, scratch));
    }
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), scratch);
    isLeftToOperation.append(branchIfWatchpointSetIsNotWatched(jit, Address(scratch, OBJECT_OFFSETOF(JSGlobalObject, m_arraySpeciesWatchpointSet)), scratch));

    if (inserts) {
        const GPRReg moved = jit.reuseScratch(StubRegister::A1);
        jit.add32(TrustedImm32(1), length, scratch);
        jit.store32(scratch, publicLength);
        Jump comesLast = jit.branch32(CCallHelpers::Equal, from, length);
        Label moveNext = jit.label();
        jit.load64(BaseIndex(butterfly, length, CCallHelpers::TimesEight, -static_cast<int32_t>(sizeof(EncodedJSValue))), moved);
        jit.store64(moved, BaseIndex(butterfly, length, CCallHelpers::TimesEight));
        jit.sub32(TrustedImm32(1), length);
        jit.branch32(CCallHelpers::Above, length, from).linkTo(moveNext, &jit);
        comesLast.link(&jit);
        jit.store64(item, BaseIndex(butterfly, from, CCallHelpers::TimesEight));
        jit.jumpToStub(arrayLiteral(0));
    } else {
        jit.load64(BaseIndex(butterfly, from, CCallHelpers::TimesEight), scratch);
        isLeftToOperation.append(jit.branchTest64(CCallHelpers::Zero, scratch));

        const GPRReg moved = jit.reuseScratch(StubRegister::A0);
        const GPRReg removed = jit.reuseScratch(StubRegister::A1);
        checkStubOperands(arrayLiteral(1), { removed });
        checkStubOperands(arrayLiteralWithInt32(1), { removed });
        jit.move(scratch, removed);
        jit.sub32(TrustedImm32(1), length);
        Jump wasLast = jit.branch32(CCallHelpers::Equal, from, length);
        Label moveNext = jit.label();
        jit.load64(BaseIndex(butterfly, from, CCallHelpers::TimesEight, sizeof(EncodedJSValue)), moved);
        jit.store64(moved, BaseIndex(butterfly, from, CCallHelpers::TimesEight));
        jit.add32(TrustedImm32(1), from);
        jit.branch32(CCallHelpers::Below, from, length).linkTo(moveNext, &jit);
        wasLast.link(&jit);
        jit.store64(TrustedImm32(0), BaseIndex(butterfly, length, CCallHelpers::TimesEight));
        jit.store32(length, publicLength);
        jit.linkToStub(jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(Int32Shape)), arrayLiteralWithInt32(1));
        jit.jumpToStub(arrayLiteral(1));
    }

    isLeftToOperation.link(&jit);
    for (unsigned i = count; i--;)
        jit.move(argumentGPR(i), jit.reuseScratch(operands[i + 1]));
    jit.move(thisGPR, jit.reuseScratch(operands[0]));
    jit.jumpToCallOfOperation(count == 3 ? Entry::operationAOTArraySpliceItemGeneric : Entry::operationAOTArraySpliceGeneric);
}

static void emitFunctionBind(StubAssembler& jit, GPRReg allocator, JumpList& callsHostFunction)
{
    constexpr unsigned kind = static_cast<unsigned>(InlineAllocation::BoundFunction);
    constexpr unsigned maxBoundArguments = JSBoundFunction::maxEmbeddedArgs;
    static_assert(BoxedCallEntry::maxArgumentCount == 1 + maxBoundArguments && BoxedCallEntry::maxArgumentCount <= numberOfArgumentGPRs);
    static_assert(JSBoundFunction::offsetOfCanConstruct() == JSBoundFunction::offsetOfBoundArgsLength() + sizeof(uint32_t) && JSBoundFunction::offsetOfCanConstruct() + sizeof(uint32_t) == sizeof(JSBoundFunction));
    const unsigned count = jit.key().count();
    const unsigned numberOfBoundArguments = count ? count - 1 : 0;
    const GPRReg bound = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(noOverlap(bound, scratch, allocator, calleeGPR, thisGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3)));

    callsHostFunction.append(jit.branchIfNotCell(thisGPR));
    checkCalleeRealm(jit, scratch, callsHostFunction);
    jit.load32(Address(thisGPR, JSCell::structureIDOffset()), scratch);
    auto [isSlim, isNeither] = branchIfNotPlainClosureHeader(jit, scratch, allocator);
    callsHostFunction.append(isNeither);
    jit.load64(Address(thisGPR, offsetOfClosureEntry), scratch);
    callsHostFunction.append(jit.branchTest64(CCallHelpers::NonZero, scratch, TrustedImm64(static_cast<int64_t>(1ULL << ClosureWord::isNotCallableBit))));
    isSlim.link(&jit);

    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].allocator)), allocator);
    Jump isNotPrepared = jit.branchTestPtr(CCallHelpers::Zero, allocator);
    allocateCell(jit, bound, allocator, scratch, callsHostFunction);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[kind].header)), scratch);
    jit.store64(scratch, Address(bound));
    jit.store64(TrustedImm32(0), Address(bound, JSObject::butterflyOffset()));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), scratch);
    jit.storePtr(scratch, Address(bound, JSCallee::offsetOfScopeChain()));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.boundFunctionExecutable)), scratch);
    jit.storePtr(scratch, Address(bound, JSFunction::offsetOfExecutableOrRareData()));
    jit.storePtr(thisGPR, Address(bound, JSBoundFunction::offsetOfTargetFunction()));
    if (count)
        jit.store64(argumentGPR(0), Address(bound, JSBoundFunction::offsetOfBoundThis()));
    else
        jit.store64(TrustedImm64(JSValue::ValueUndefined), Address(bound, JSBoundFunction::offsetOfBoundThis()));
    for (unsigned i = 0; i < maxBoundArguments; ++i) {
        const Address argument(bound, JSBoundFunction::offsetOfBoundArgs() + i * sizeof(WriteBarrier<Unknown>));
        if (i < numberOfBoundArguments)
            jit.store64(argumentGPR(i + 1), argument);
        else
            jit.store64(TrustedImm32(0), argument);
    }
    jit.store64(TrustedImm32(0), Address(bound, JSBoundFunction::offsetOfNameMayBeNull()));
    jit.store64(TrustedImm64(std::bit_cast<int64_t>(PNaN)), Address(bound, JSBoundFunction::offsetOfLength()));
    jit.store32(TrustedImm32(numberOfBoundArguments), Address(bound, JSBoundFunction::offsetOfBoundArgsLength()));
    jit.store32(TrustedImm32(static_cast<uint8_t>(TriState::Indeterminate)), Address(bound, JSBoundFunction::offsetOfCanConstruct()));
    fenceAfterAllocation(jit);
    jit.move(bound, jit.result());
    jit.returnToCaller();

    isNotPrepared.link(&jit);
    jit.preservingCallerSavedRegisters(false, [&](unsigned) {
        jit.move(instanceGPR, jit.reuseScratch(StubRegister::A0));
        jit.callOperation(Entry::operationAOTPrepareBoundFunctionAllocation, jit.reuseScratch(StubRegister::Operation));
    });
    callsHostFunction.append(jit.jump());
}

enum class ArgumentGuard : uint8_t { None, IsString, IsObject, IsFunction, IsRegExp, IsUntouchedRegExp };

struct CallWithoutHostFunction {
    std::array<ArgumentGuard, 2> arguments;
    std::optional<Entry> operation;
    StubKey stub { };
    std::optional<uint32_t> wordBehindArguments { };
    bool takesCallee { false };
};

struct BoxedCallHandler {
    BoxedCallForm form;
    unsigned minCount;
    unsigned maxCount;
    std::optional<JSTypeRange> receiverTypes;
    void (*emit)(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);
    StubKey target { };
    ArgumentGuard firstArgument { ArgumentGuard::None };
    std::span<const CallWithoutHostFunction> calls { };
    ArgumentGuard receiver { ArgumentGuard::None };
};

static constexpr JSTypeRange exactly(JSType type) { return { type, type }; }
static void emitJumpToTarget(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);
static void emitFirstCallThatFits(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);
static void emitPromiseReaction(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);
static void emitPromiseResolve(StubAssembler&, GPRReg scratch, JumpList& callsHostFunction);

static void emitArrayPop(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(noOverlap(gprOf(StubRegister::A1), gprOf(StubRegister::A2), thisGPR));
    const GPRReg kind = scratch;
    const GPRReg element = scratch;
    const GPRReg butterfly = jit.reuseScratch(StubRegister::A1);
    const GPRReg length = jit.reuseScratch(StubRegister::A2);
    RELEASE_ASSERT(noOverlap(scratch, butterfly, length, thisGPR));
    JumpList isLeftToOperation;
    jit.load8(Address(thisGPR, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), kind);
    Jump holdsValues = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(ContiguousShape));
    Jump holdsNoIntegers = jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(Int32Shape));
    holdsValues.link(&jit);
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), butterfly);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, length);
    jit.sub32(TrustedImm32(1), length);
    jit.load64(BaseIndex(butterfly, length, CCallHelpers::TimesEight), element);
    isLeftToOperation.append(jit.branchTest64(CCallHelpers::Zero, element));
    jit.store64(TrustedImm32(0), BaseIndex(butterfly, length, CCallHelpers::TimesEight));
    jit.store32(length, Address(butterfly, Butterfly::offsetOfPublicLength()));
    jit.move(element, jit.result());
    jit.returnToCaller();

    isEmpty.link(&jit);
    Label returnsUndefined = jit.label();
    jit.move(TrustedImm64(JSValue::encode(jsUndefined())), jit.result());
    jit.returnToCaller();

    holdsNoIntegers.link(&jit);
    isLeftToOperation.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(DoubleShape)));
    const GPRReg hole = jit.reuseScratch(StubRegister::A3);
    RELEASE_ASSERT(noOverlap(hole, element, butterfly, length, thisGPR, calleeGPR, instanceGPR));
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), butterfly);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), length);
    jit.branchTest32(CCallHelpers::Zero, length).linkTo(returnsUndefined, &jit);
    jit.sub32(TrustedImm32(1), length);
    jit.load64(BaseIndex(butterfly, length, CCallHelpers::TimesEight), element);
    PureDoubleBits popped = PureDoubleBits::branchIfNotFinite(jit, element, hole, isLeftToOperation);
    jit.move(TrustedImm64(static_cast<int64_t>(PNaNAsBits)), hole);
    jit.store64(hole, BaseIndex(butterfly, length, CCallHelpers::TimesEight));
    jit.store32(length, Address(butterfly, Butterfly::offsetOfPublicLength()));
    boxDoubleBits(jit, popped);
    jit.move(element, jit.result());
    jit.returnToCaller();

    isLeftToOperation.link(&jit);
    emitFirstCallThatFits(jit, scratch, callsHostFunction);
}

static void emitArrayShift(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(areDistinctOnEveryISA(StubRegister::A1, StubRegister::A2, StubRegister::A3, StubRegister::A5, StubRegister::T9));
    const GPRReg element = scratch;
    const GPRReg butterfly = jit.reuseScratch(StubRegister::A1);
    const GPRReg last = jit.reuseScratch(StubRegister::A2);
    const GPRReg kind = jit.reuseScratch(StubRegister::A3);
    const GPRReg index = kind;
    const GPRReg moved = jit.reuseScratch(StubRegister::A5);
    RELEASE_ASSERT(noOverlap(element, butterfly, last, kind, moved, thisGPR, calleeGPR, instanceGPR));
    JumpList isLeftToOperation;
    jit.load8(Address(thisGPR, JSCell::indexingTypeAndMiscOffset()), kind);
    jit.and32(TrustedImm32(IndexingShapeAndWritabilityMask), kind);
    Jump holdsValues = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(ContiguousShape));
    isLeftToOperation.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(Int32Shape)));
    holdsValues.link(&jit);
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), butterfly);
    jit.load32(Address(butterfly, Butterfly::offsetOfPublicLength()), last);
    Jump isEmpty = jit.branchTest32(CCallHelpers::Zero, last);
    jit.sub32(TrustedImm32(1), last);
    Jump isLong = jit.branch32(CCallHelpers::Above, last, TrustedImm32(mostValuesMovedByShiftInStub));
    jit.load64(Address(butterfly), element);
    isLeftToOperation.append(jit.branchTest64(CCallHelpers::Zero, element));
    Jump isAlone = jit.branchTest32(CCallHelpers::Zero, last);
    auto leaveToOperationUnlessValuesMayMove = [&] {
        isLeftToOperation.append(branchIfNotPristine(jit, thisGPR));
        Jump holdsIntegers = jit.branch32(CCallHelpers::Equal, kind, TrustedImm32(Int32Shape));
        isLeftToOperation.append(jit.branchTest8(CCallHelpers::NonZero, Address(instanceGPR, AOT_INSTANCE_OFFSET(mutatorShouldBeFenced))));
        holdsIntegers.link(&jit);
    };
    leaveToOperationUnlessValuesMayMove();

    jit.move(TrustedImm32(0), index);
    Label moveNext = jit.label();
    jit.add32(TrustedImm32(1), index);
    jit.load64(BaseIndex(butterfly, index, CCallHelpers::TimesEight), moved);
    jit.store64(moved, BaseIndex(butterfly, index, CCallHelpers::TimesEight, -static_cast<int32_t>(sizeof(EncodedJSValue))));
    jit.branch32(CCallHelpers::Below, index, last).linkTo(moveNext, &jit);

    isAlone.link(&jit);
    jit.store64(TrustedImm32(0), BaseIndex(butterfly, last, CCallHelpers::TimesEight));
    jit.store32(last, Address(butterfly, Butterfly::offsetOfPublicLength()));
    jit.move(element, jit.result());
    jit.returnToCaller();

    isLong.link(&jit);
    isLeftToOperation.append(jit.branch32(CCallHelpers::AboveOrEqual, last, TrustedImm32(JSArray::shiftThreshold)));
    jit.load64(Address(butterfly), element);
    isLeftToOperation.append(jit.branchTest64(CCallHelpers::Zero, element));
    leaveToOperationUnlessValuesMayMove();
    const GPRReg target = jit.reuseScratch(StubRegister::A0);
    RELEASE_ASSERT(target == leafArgumentGPR(0) && butterfly == leafArgumentGPR(1) && last == leafArgumentGPR(2));
    jit.store32(last, Address(butterfly, Butterfly::offsetOfPublicLength()));
    jit.getEffectiveAddress(BaseIndex(butterfly, last, CCallHelpers::TimesEight), moved);
    jit.pushPair(element, moved);
    jit.move(butterfly, target);
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), butterfly);
    jit.lshift64(TrustedImm32(CCallHelpers::TimesEight), last);
    jit.callLeaf(Entry::operationAOTMoveBytes, 2 * sizeof(CPURegister));
    jit.popPair(jit.result(), moved);
    jit.store64(TrustedImm32(0), Address(moved));
    jit.returnToCaller();

    isEmpty.link(&jit);
    jit.move(TrustedImm64(JSValue::encode(jsUndefined())), jit.result());
    jit.returnToCaller();

    isLeftToOperation.link(&jit);
    emitFirstCallThatFits(jit, scratch, callsHostFunction);
}

static void emitTableClear(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    const GPRReg table = scratch;
    jit.loadPtr(Address(thisGPR, offsetOfHashTableStorage()), table);
    Jump hasNoStorage = jit.branchTestPtr(CCallHelpers::Zero, table);
    Jump hasStorageOfEngine = jit.branchIfType(table, JSCellButterflyType);
    Jump holdsEntries = jit.branchTest32(CCallHelpers::NonZero, Address(table, CompactTable::offsetOfAliveCount()));
    hasNoStorage.link(&jit);
    jit.move(TrustedImm64(JSValue::encode(jsUndefined())), jit.result());
    jit.returnToCaller();

    hasStorageOfEngine.link(&jit);
    holdsEntries.link(&jit);
    emitFirstCallThatFits(jit, scratch, callsHostFunction);
}

static void emitStringEndsWith(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(areDistinctOnEveryISA(StubRegister::A0, StubRegister::A1, StubRegister::A2, StubRegister::A3, StubRegister::A5, StubRegister::T9));
    JumpList isNotThere;
    JumpList needsOperation;
    needsOperation.append(jit.branchIfNotCell(argumentGPR(0)));
    needsOperation.append(jit.branchIfNotType(argumentGPR(0), StringType));
    branchUnlessTextStandsAtPlace(jit, { thisGPR, argumentGPR(0), jit.reuseScratch(StubRegister::A1), jit.reuseScratch(StubRegister::A2), jit.reuseScratch(StubRegister::A3), scratch, jit.reuseScratch(StubRegister::A5) }, PlaceOfSoughtText::IsEnd, isNotThere, needsOperation);
    jit.move(TrustedImm32(JSValue::ValueTrue), jit.result());
    jit.returnToCaller();

    isNotThere.link(&jit);
    jit.move(TrustedImm32(JSValue::ValueFalse), jit.result());
    jit.returnToCaller();

    needsOperation.link(&jit);
    emitFirstCallThatFits(jit, scratch, callsHostFunction);
}

static constexpr uint32_t noSplitLimit = UINT32_MAX;
static constexpr auto anything = ArgumentGuard::None;
static constexpr auto aText = ArgumentGuard::IsString;
static constexpr auto aFunction = ArgumentGuard::IsFunction;
static constexpr auto aPattern = ArgumentGuard::IsUntouchedRegExp;
static constexpr CallWithoutHostFunction arrayJoinCalls[] = { { { anything, anything }, Entry::operationAOTArrayJoinOfAnyReceiver, { }, std::nullopt, true } };
static constexpr CallWithoutHostFunction arrayToStringCalls[] = { { { anything, anything }, Entry::operationAOTArrayToStringOfAnyReceiver, { }, std::nullopt, true } };
static constexpr CallWithoutHostFunction arrayPopCalls[] = { { { anything, anything }, Entry::operationAOTArrayPopGeneric } };
static constexpr CallWithoutHostFunction arrayShiftCalls[] = { { { anything, anything }, Entry::operationAOTArrayShiftGeneric } };
static constexpr CallWithoutHostFunction arrayUnshiftCalls[] = { { { anything, anything }, Entry::operationAOTArrayUnshiftGeneric } };
static constexpr CallWithoutHostFunction mapClearCalls[] = { { { anything, anything }, Entry::operationAOTMapClearGeneric } };
static constexpr CallWithoutHostFunction setClearCalls[] = { { { anything, anything }, Entry::operationAOTSetClearGeneric } };
static constexpr CallWithoutHostFunction regExpExecCalls[] = { { { aText, anything }, std::nullopt, regExpMatch(RegExpMatchResult::Array) } };
static constexpr CallWithoutHostFunction regExpTestCalls[] = { { { aText, anything }, std::nullopt, regExpMatch(RegExpMatchResult::BoxedBoolean) } };
static constexpr CallWithoutHostFunction stringEndsWithCalls[] = { { { aText, anything }, Entry::operationAOTStringEndsWith } };
static constexpr CallWithoutHostFunction stringIncludesCalls[] = { { { aText, anything }, Entry::operationAOTStringIncludes } };
static constexpr CallWithoutHostFunction stringMatchCalls[] = { { { aPattern, anything }, std::nullopt, regExpMatch(RegExpMatchResult::ArrayForStringMatch) } };
static constexpr CallWithoutHostFunction stringReplaceCalls[] = {
    { { aPattern, aText }, std::nullopt, stringReplaceRegExpMayNotMatch() },
    { { aPattern, aFunction }, std::nullopt, stringReplaceRegExpMayNotMatch() },
    { { aText, aText }, Entry::operationAOTStringReplaceString },
};
static constexpr CallWithoutHostFunction stringReplaceAllCalls[] = { { { aText, aText }, Entry::operationAOTStringReplaceAllString } };
static constexpr CallWithoutHostFunction stringSplitCalls[] = {
    { { aText, anything }, Entry::operationAOTStringSplitString, { }, noSplitLimit },
    { { aPattern, anything }, Entry::operationAOTStringSplitRegExp, { }, noSplitLimit },
};
static constexpr CallWithoutHostFunction stringToLowerCaseCalls[] = { { { anything, anything }, Entry::operationAOTStringToLowerCase } };
static constexpr CallWithoutHostFunction stringToUpperCaseCalls[] = { { { anything, anything }, Entry::operationAOTStringToUpperCase } };
static constexpr CallWithoutHostFunction stringTrimEndCalls[] = { { { anything, anything }, Entry::operationAOTStringTrim, { }, static_cast<uint32_t>(TrimmedEnds::End) } };
static constexpr CallWithoutHostFunction stringTrimStartCalls[] = { { { anything, anything }, Entry::operationAOTStringTrim, { }, static_cast<uint32_t>(TrimmedEnds::Start) } };

static constexpr BoxedCallHandler boxedCallHandlers[] = {
    { BoxedCallForm::ArrayIncludes, 1, 1, arrayTypes, emitArraySearch },
    { BoxedCallForm::ArrayIndexOf, 1, 1, arrayTypes, emitArraySearch },
    { BoxedCallForm::ArrayIsArray, 1, BoxedCallEntry::maxArgumentCount, std::nullopt, emitArrayIsArray },
    { BoxedCallForm::ArrayJoin, 1, 1, arrayTypes, emitFirstCallThatFits, { }, anything, arrayJoinCalls },
    { BoxedCallForm::ArrayPop, 0, 0, arrayTypes, emitArrayPop, { }, anything, arrayPopCalls },
    { BoxedCallForm::ArrayPush, 1, 1, arrayTypes, emitArrayPush },
    { BoxedCallForm::ArrayShift, 0, 0, arrayTypes, emitArrayShift, { }, anything, arrayShiftCalls },
    { BoxedCallForm::ArraySlice, 0, 2, arrayTypes, emitArraySlice },
    { BoxedCallForm::ArraySort, 0, 1, arrayTypes, emitArraySort },
    { BoxedCallForm::ArraySplice, 2, 3, arrayTypes, emitArraySplice },
    { BoxedCallForm::ArrayToString, 0, 0, arrayTypes, emitFirstCallThatFits, { }, anything, arrayToStringCalls },
    { BoxedCallForm::ArrayUnshift, 1, 1, arrayTypes, emitFirstCallThatFits, { }, anything, arrayUnshiftCalls },
    { BoxedCallForm::DataViewGetFloat32, 1, 2, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::DataViewGetFloat64, 1, 2, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::DataViewGetInt16, 1, 2, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::DataViewGetInt32, 1, 2, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::DataViewGetInt8, 1, 1, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::DataViewGetUint16, 1, 2, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::DataViewGetUint32, 1, 2, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::DataViewGetUint8, 1, 1, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::DataViewSetFloat32, 2, 3, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::DataViewSetFloat64, 2, 3, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::DataViewSetInt16, 2, 3, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::DataViewSetInt32, 2, 3, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::DataViewSetInt8, 2, 2, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::DataViewSetUint16, 2, 3, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::DataViewSetUint32, 2, 3, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::DataViewSetUint8, 2, 2, exactly(DataViewType), emitDataViewAccess },
    { BoxedCallForm::FunctionBind, 0, 1 + JSBoundFunction::maxEmbeddedArgs, exactly(SlimClosureType), emitFunctionBind },
    { BoxedCallForm::IsNaN, 1, BoxedCallEntry::maxArgumentCount, std::nullopt, emitIsNaN },
    { BoxedCallForm::JSONStringify, 1, 2, std::nullopt, emitJSONStringify },
    { BoxedCallForm::MapClear, 0, 0, exactly(JSMapType), emitTableClear, { }, anything, mapClearCalls },
    { BoxedCallForm::MapDelete, 1, 1, exactly(JSMapType), emitJumpToTarget, mapDelete(KeyComparison::AnyValue) },
    { BoxedCallForm::MapGet, 1, 1, exactly(JSMapType), emitJumpToTarget, mapGet(KeyComparison::AnyValue) },
    { BoxedCallForm::MapHas, 1, 1, exactly(JSMapType), emitJumpToTarget, mapHas(KeyComparison::AnyValue) },
    { BoxedCallForm::MapSet, 2, 2, exactly(JSMapType), emitJumpToTarget, mapSet(KeyComparison::AnyValue) },
    { BoxedCallForm::MapSize, 0, 0, exactly(JSMapType), emitTableSize },
    { BoxedCallForm::MathMax, 2, 2, std::nullopt, emitMathExtremum },
    { BoxedCallForm::MathMin, 2, 2, std::nullopt, emitMathExtremum },
    { BoxedCallForm::ObjectGetPrototypeOf, 1, 1, std::nullopt, emitObjectGetPrototypeOf },
    { BoxedCallForm::ObjectHasOwnProperty, 1, 1, std::nullopt, emitObjectHasOwnProperty },
    { BoxedCallForm::ObjectIs, 2, 2, std::nullopt, emitObjectIs },
    { BoxedCallForm::ObjectSetPrototypeOf, 2, 2, std::nullopt, emitObjectSetPrototypeOf },
    { BoxedCallForm::PromiseCatch, 1, 1, exactly(JSPromiseType), emitPromiseReaction },
    { BoxedCallForm::PromiseResolve, 0, 1, std::nullopt, emitPromiseResolve },
    { BoxedCallForm::PromiseThen, 1, 2, exactly(JSPromiseType), emitPromiseReaction },
    { BoxedCallForm::RegExpExec, 1, 1, std::nullopt, emitFirstCallThatFits, { }, anything, regExpExecCalls, aPattern },
    { BoxedCallForm::RegExpTest, 1, 1, std::nullopt, emitFirstCallThatFits, { }, anything, regExpTestCalls, aPattern },
    { BoxedCallForm::SetAdd, 1, 1, exactly(JSSetType), emitJumpToTarget, setAdd(KeyComparison::AnyValue) },
    { BoxedCallForm::SetClear, 0, 0, exactly(JSSetType), emitTableClear, { }, anything, setClearCalls },
    { BoxedCallForm::SetDelete, 1, 1, exactly(JSSetType), emitJumpToTarget, setDelete(KeyComparison::AnyValue) },
    { BoxedCallForm::SetHas, 1, 1, exactly(JSSetType), emitJumpToTarget, setHas(KeyComparison::AnyValue) },
    { BoxedCallForm::SetSize, 0, 0, exactly(JSSetType), emitTableSize },
    { BoxedCallForm::StringAt, 1, 1, exactly(StringType), emitStringCharacterAt },
    { BoxedCallForm::StringCharAt, 1, 1, exactly(StringType), emitStringCharacterAt },
    { BoxedCallForm::StringCharCodeAt, 1, 1, exactly(StringType), emitStringCodeUnitAt },
    { BoxedCallForm::StringCodePointAt, 1, 1, exactly(StringType), emitStringCodeUnitAt },
    { BoxedCallForm::StringEndsWith, 1, 1, exactly(StringType), emitStringEndsWith, { }, anything, stringEndsWithCalls },
    { BoxedCallForm::StringIncludes, 1, 1, exactly(StringType), emitFirstCallThatFits, { }, anything, stringIncludesCalls },
    { BoxedCallForm::StringIndexOf, 1, 1, exactly(StringType), emitStringSearchFromPosition },
    { BoxedCallForm::StringIsWellFormed, 0, 0, exactly(StringType), emitStringIsWellFormed },
    { BoxedCallForm::StringLastIndexOf, 1, 1, exactly(StringType), emitStringSearchFromPosition },
    { BoxedCallForm::StringMatch, 1, 1, exactly(StringType), emitFirstCallThatFits, { }, anything, stringMatchCalls },
    { BoxedCallForm::StringReplace, 2, 2, exactly(StringType), emitFirstCallThatFits, { }, anything, stringReplaceCalls },
    { BoxedCallForm::StringReplaceAll, 2, 2, exactly(StringType), emitFirstCallThatFits, { }, anything, stringReplaceAllCalls },
    { BoxedCallForm::StringSlice, 1, 2, exactly(StringType), emitStringPiece },
    { BoxedCallForm::StringSplit, 1, 1, exactly(StringType), emitFirstCallThatFits, { }, anything, stringSplitCalls },
    { BoxedCallForm::StringStartsWith, 1, 1, exactly(StringType), emitStringSearchFromPosition },
    { BoxedCallForm::StringSubstr, 1, 2, exactly(StringType), emitStringPiece },
    { BoxedCallForm::StringSubstring, 1, 2, exactly(StringType), emitStringPiece },
    { BoxedCallForm::StringToLowerCase, 0, 0, exactly(StringType), emitFirstCallThatFits, { }, anything, stringToLowerCaseCalls },
    { BoxedCallForm::StringToUpperCase, 0, 0, exactly(StringType), emitFirstCallThatFits, { }, anything, stringToUpperCaseCalls },
    { BoxedCallForm::StringTrim, 0, 0, exactly(StringType), emitStringTrim },
    { BoxedCallForm::StringTrimEnd, 0, 0, exactly(StringType), emitFirstCallThatFits, { }, anything, stringTrimEndCalls },
    { BoxedCallForm::StringTrimStart, 0, 0, exactly(StringType), emitFirstCallThatFits, { }, anything, stringTrimStartCalls },
    { BoxedCallForm::TypedArrayLength, 0, 0, typedArrayTypes, emitTypedArrayLength },
    { BoxedCallForm::WeakMapGet, 1, 1, exactly(JSWeakMapType), emitJumpToTarget, weakMapGet() },
    { BoxedCallForm::WeakMapHas, 1, 1, exactly(JSWeakMapType), emitJumpToTarget, weakMapHas() },
    { BoxedCallForm::WeakMapSet, 2, 2, exactly(JSWeakMapType), emitJumpToTarget, weakMapSet(), ArgumentGuard::IsObject },
    { BoxedCallForm::WeakSetHas, 1, 1, exactly(JSWeakSetType), emitJumpToTarget, weakSetHas() },
};
static_assert(std::size(boxedCallHandlers) == numberOfBuiltinBoxedStubs);
static_assert(std::ranges::all_of(boxedCallHandlers, [](const BoxedCallHandler& handler) { return &handler == &boxedCallHandlers[static_cast<unsigned>(handler.form)]; }));

static void branchUnlessPasses(StubAssembler& jit, ArgumentGuard guard, GPRReg value, GPRReg scratch, JumpList& fails)
{
    if (guard == ArgumentGuard::None)
        return;
    fails.append(jit.branchIfNotCell(value));
    switch (guard) {
    case ArgumentGuard::None:
        RELEASE_ASSERT_NOT_REACHED();
    case ArgumentGuard::IsString:
        fails.append(jit.branchIfNotType(value, StringType));
        return;
    case ArgumentGuard::IsObject:
        fails.append(jit.branchIfNotObject(value));
        return;
    case ArgumentGuard::IsFunction: {
        Jump isFunction = jit.branchIfType(value, jsFunctionTypes);
        fails.append(jit.branchIfNotType(value, slimClosureTypes));
        isFunction.link(&jit);
        return;
    }
    case ArgumentGuard::IsRegExp:
        fails.append(jit.branchIfNotType(value, RegExpObjectType));
        return;
    case ArgumentGuard::IsUntouchedRegExp:
        jit.load32(Address(value, JSCell::structureIDOffset()), scratch);
        fails.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, Instance::offsetOfReceiverStructureID(Receiver::RegExp))));
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), scratch);
        fails.append(branchIfWatchpointSetIsNotWatched(jit, Address(scratch, OBJECT_OFFSETOF(JSGlobalObject, m_regExpPrimordialPropertiesWatchpointSet)), scratch));
        return;
    }
}

static void emitJumpToTarget(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    std::array registers { thisGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3) };
    static_assert(registers.size() == BoxedCallEntry::maxArgumentCount + 1);
    const BoxedCallHandler& handler = boxedCallHandlers[jit.key().form()];
    registers[0] = signatureOf(handler.target).operands[0].gpr();
    checkStubOperands(handler.target, std::span(registers).first(jit.key().count() + 1));
    branchUnlessPasses(jit, handler.firstArgument, argumentGPR(0), scratch, callsHostFunction);
    if (registers[0] != thisGPR) {
        jit.recordClobbers(RegisterSet { registers[0] });
        jit.move(thisGPR, registers[0]);
    }
    jit.jumpToStub(handler.target);
}

static void emitPromiseResolve(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    const bool hasValue = jit.key().count() == 1;
    constexpr GPRReg value = argumentGPR(0);
    const GPRReg made = jit.clobberRegister(StubRegister::A5);
    const GPRReg allocator = jit.clobberRegister(StubRegister::A2);
    const GPRReg word = jit.clobberRegister(StubRegister::A3);
    RELEASE_ASSERT(noOverlap(made, allocator, word, scratch, thisGPR, calleeGPR, value, instanceGPR));

    checkCalleeRealm(jit, scratch, callsHostFunction);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), word);
    callsHostFunction.append(jit.branchPtr(CCallHelpers::NotEqual, Address(word, OBJECT_OFFSETOF(JSGlobalObject, m_promiseConstructor)), thisGPR));
    JumpList makesPromise;
    if (hasValue) {
        makesPromise.append(jit.branchIfNotCell(value));
        makesPromise.append(jit.branchIfNotObject(value));
        jit.load32(Address(value, JSCell::structureIDOffset()), scratch);
        Jump isOtherObject = jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, offsetOfPromiseHeader));
        callsHostFunction.append(branchIfWatchpointSetIsNotWatched(jit, Address(word, OBJECT_OFFSETOF(JSGlobalObject, m_promiseSpeciesWatchpointSet)), scratch));
        jit.move(value, jit.result());
        jit.returnToCaller();

        isOtherObject.link(&jit);
        emitBranchIfMayBeThenable(jit, value, word, scratch, callsHostFunction);
    }

    makesPromise.link(&jit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Promise)].allocator)), allocator);
    callsHostFunction.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    auto refill = allocateCellWithLateRefill(jit, made, allocator, word, callsHostFunction);
    jit.load64(Address(instanceGPR, offsetOfPromiseHeader), word);
    jit.store64(word, Address(made));
    jit.move(TrustedImm64(static_cast<int64_t>(fulfilledPromiseBits)), word);
    jit.store64(word, Address(made, JSPromise::offsetOfPacked()));
    jit.move(TrustedImm32(0), word);
    jit.store64(word, Address(made, JSObject::butterflyOffset()));
    if (hasValue)
        jit.store64(value, Address(made, JSPromise::offsetOfSlot()));
    else {
        jit.move(TrustedImm64(JSValue::ValueUndefined), word);
        jit.store64(word, Address(made, JSPromise::offsetOfSlot()));
    }
    fenceAfterAllocation(jit);
    jit.move(made, jit.result());
    jit.returnToCaller();

    emitLateRefill(jit, refill, callsHostFunction);
}

static void emitPromiseReaction(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    const bool isCatch = static_cast<BoxedCallForm>(jit.key().form()) == BoxedCallForm::PromiseCatch;
    const bool takesBoth = jit.key().count() == 2;
    const StubKey target = promiseThen(isCatch ? PromiseHandler::OnRejected : PromiseHandler::OnFulfilled);
    constexpr GPRReg handler = argumentGPR(0);
    constexpr GPRReg second = argumentGPR(1);
    const GPRReg made = jit.clobberRegister(StubRegister::A5);
    const GPRReg allocator = jit.clobberRegister(StubRegister::A2);
    const GPRReg word = jit.clobberRegister(StubRegister::A3);
    RELEASE_ASSERT(noOverlap(made, allocator, word, scratch, thisGPR, calleeGPR, handler, second, instanceGPR));

    checkCalleeRealm(jit, scratch, callsHostFunction);
    branchUnlessPasses(jit, ArgumentGuard::IsFunction, handler, scratch, callsHostFunction);
    if (takesBoth)
        branchUnlessPasses(jit, ArgumentGuard::IsFunction, second, scratch, callsHostFunction);
    jit.load32(Address(thisGPR, JSCell::structureIDOffset()), scratch);
    callsHostFunction.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, offsetOfPromiseHeader)));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), word);
    callsHostFunction.append(branchIfWatchpointSetIsNotWatched(jit, Address(word, OBJECT_OFFSETOF(JSGlobalObject, m_promiseThenWatchpointSet)), scratch));
    callsHostFunction.append(branchIfWatchpointSetIsNotWatched(jit, Address(word, OBJECT_OFFSETOF(JSGlobalObject, m_promiseSpeciesWatchpointSet)), scratch));

    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::Promise)].allocator)), allocator);
    callsHostFunction.append(jit.branchTestPtr(CCallHelpers::Zero, allocator));
    auto refill = allocateCellWithLateRefill(jit, made, allocator, word, callsHostFunction);
    jit.load64(Address(instanceGPR, offsetOfPromiseHeader), word);
#if CPU(ARM64)
    jit.storePair64(word, ARM64Registers::zr, made, TrustedImm32(0));
    jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, made, TrustedImm32(JSPromise::offsetOfPacked()));
#else
    jit.store64(word, Address(made));
    jit.store64(TrustedImm32(0), Address(made, JSObject::butterflyOffset()));
    jit.store64(TrustedImm32(0), Address(made, JSPromise::offsetOfPacked()));
    jit.store64(TrustedImm32(0), Address(made, JSPromise::offsetOfSlot()));
#endif
    fenceAfterAllocation(jit);

    const StubSignature signature = signatureOf(target);
    const GPRReg promise = jit.clobberRegister(signature.operands[0].gpr());
    const GPRReg taken = jit.clobberRegister(signature.operands[1].gpr());
    const GPRReg derived = jit.clobberRegister(signature.operands[2].gpr());
    RELEASE_ASSERT(noOverlap(promise, taken, derived, made, thisGPR, handler, instanceGPR));
    if (takesBoth) {
        const GPRReg onRejected = jit.clobberRegister(StubRegister::A3);
        RELEASE_ASSERT(promise == operationArgumentGPR(1) && taken == operationArgumentGPR(2) && onRejected == operationArgumentGPR(3) && derived == operationArgumentGPR(4));
        RELEASE_ASSERT(noOverlap(onRejected, taken, derived, made, thisGPR, handler, second, instanceGPR));
        jit.move(second, onRejected);
    }
    jit.move(made, derived);
    jit.move(thisGPR, promise);
    jit.move(handler, taken);
    checkStubOperands(target, { promise, taken, derived });
    if (takesBoth) {
        JumpList isNotFulfilled;
        emitBranchUnlessHolds(jit, awaitedIsFulfilled, { InvalidGPRReg, promise, made, scratch }, isNotFulfilled);
        jit.jumpToStub(target);
        isNotFulfilled.link(&jit);
        jit.jumpToCallOfOperation(Entry::operationAOTPromiseThen);
    } else
        jit.jumpToStub(target);

    emitLateRefill(jit, refill, callsHostFunction);
}

static void emitFirstCallThatFits(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(argumentGPR(0) == operationArgumentGPR(0) && argumentGPR(1) == operationArgumentGPR(1) && noOverlap(thisGPR, calleeGPR, operationArgumentGPR(1), operationArgumentGPR(2), operationArgumentGPR(3)));
    static constexpr std::array operands { StubRegister::A1, StubRegister::A2, StubRegister::A3, StubRegister::A4 };
    static_assert(gprOf(operands[0]) == operationArgumentGPR(1) && gprOf(operands[1]) == operationArgumentGPR(2) && gprOf(operands[2]) == operationArgumentGPR(3) && gprOf(operands[3]) == operationArgumentGPR(4));
    const BoxedCallHandler& handler = boxedCallHandlers[jit.key().form()];
    const unsigned count = jit.key().count();
    RELEASE_ASSERT(!handler.calls.empty() && count <= 2);

    Vector<const CallWithoutHostFunction*, 4> calls;
    for (const CallWithoutHostFunction& call : handler.calls) {
        bool takesRegExp = handler.receiver == ArgumentGuard::IsUntouchedRegExp || std::ranges::find(call.arguments, ArgumentGuard::IsUntouchedRegExp) != call.arguments.end();
        if ((!takesRegExp || jit.options().matchesRegExps) && !(call.takesCallee && isCalledByName(jit)))
            calls.append(&call);
    }
    if (calls.isEmpty()) {
        callsHostFunction.append(jit.jump());
        return;
    }

    checkCalleeRealm(jit, scratch, callsHostFunction);
    bool looksNothingUpOnReceiver = handler.form == BoxedCallForm::RegExpExec && !isCalledByName(jit);
    branchUnlessPasses(jit, looksNothingUpOnReceiver ? ArgumentGuard::IsRegExp : handler.receiver, thisGPR, scratch, callsHostFunction);
    for (const CallWithoutHostFunction* candidate : calls) {
        const CallWithoutHostFunction& call = *candidate;
        JumpList isAnotherKind;
        JumpList& fails = candidate == calls.last() ? callsHostFunction : isAnotherKind;
        for (unsigned i = 0; i < count; ++i)
            branchUnlessPasses(jit, call.arguments[i], argumentGPR(i), scratch, fails);

        for (unsigned i = count; i--;)
            jit.move(argumentGPR(i), jit.reuseScratch(operands[i + 1]));
        jit.move(thisGPR, jit.reuseScratch(operands[0]));
        RELEASE_ASSERT(!call.wordBehindArguments || !call.takesCallee);
        if (call.wordBehindArguments)
            jit.move(TrustedImm32(static_cast<int32_t>(*call.wordBehindArguments)), jit.reuseScratch(operands[count + 1]));
        if (call.takesCallee)
            jit.move(calleeGPR, jit.reuseScratch(operands[count + 1]));
        if (call.operation)
            jit.jumpToCallOfOperation(*call.operation);
        else {
            std::array registers { gprOf(operands[0]), gprOf(operands[1]), gprOf(operands[2]) };
            checkStubOperands(call.stub, std::span(registers).first(count + 1));
            jit.jumpToStub(call.stub);
        }
        isAnotherKind.link(&jit);
    }
}

bool acceptsArgumentCount(BoxedCallForm form, unsigned count)
{
    const BoxedCallHandler& handler = boxedCallHandlers[static_cast<unsigned>(form)];
    if (handler.emit == emitArrayPush && pushesSeveralValues(count))
        return true;
    if (handler.emit == emitStringSearchFromPosition && searchesFromPosition(count))
        return true;
    return count >= handler.minCount && count <= handler.maxCount;
}

bool fetchesFunctionOnlyToLeave(BoxedCallForm form, unsigned count)
{
    return acceptsArgumentCount(form, count) && std::ranges::none_of(boxedCallHandlers[static_cast<unsigned>(form)].calls, &CallWithoutHostFunction::takesCallee);
}

void generateCallBoxedBuiltin(StubAssembler& jit)
{
    const BoxedCallHandler& handler = boxedCallHandlers[jit.key().form()];
    const StubKey hostCall = fetchesFunctionOnlyToLeave(handler.form, jit.key().count()) ? callHostFunctionThatMayBeUnfetched(jit.key().count()) : callHostFunctionWithoutBoxedCall(jit.key().count());
    if (!acceptsArgumentCount(handler.form, jit.key().count()) || jit.options().boxedCallsOutOfReach.get(jit.key().form())) {
        jit.jumpToStub(hostCall);
        return;
    }

    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    JumpList callsHostFunction;
    if (handler.receiverTypes && handler.emit != emitFunctionBind) {
        callsHostFunction.append(jit.branchIfNotCell(thisGPR));
        callsHostFunction.append(jit.branchIfNotType(thisGPR, *handler.receiverTypes));
    }
    handler.emit(jit, scratch, callsHostFunction);
    jit.linkToStub(callsHostFunction, hostCall);
}

StubSignature signatureOfCallMethodOfBuiltinClass(StubKey key) { return signatureOf(callMethodById(key.count())); }

static bool receiverOfBoxedCallIsObject(BoxedCallForm form)
{
    const BoxedCallHandler& handler = boxedCallHandlers[static_cast<unsigned>(form)];
    return handler.receiverTypes && handler.receiverTypes->first >= ObjectType;
}

std::optional<JSTypeRange> cellTypesOfReceiversOf(BoxedCallForm form)
{
    return boxedCallHandlers[static_cast<unsigned>(form)].receiverTypes;
}

bool looksWhetherReceiverIsAsMade(BoxedCallForm form)
{
    return receiverOfBoxedCallIsObject(form);
}

bool servesCallsByName(BoxedCallForm form, unsigned count)
{
    const BoxedCallHandler& handler = boxedCallHandlers[static_cast<unsigned>(form)];
    bool looksAtReceiver = handler.receiverTypes || handler.receiver == ArgumentGuard::IsUntouchedRegExp;
    bool isGetter = handler.emit == emitTableSize || handler.emit == emitTypedArrayLength;
    bool needsCallee = handler.emit == emitJSONStringify
        || (handler.emit == emitFirstCallThatFits && std::ranges::all_of(handler.calls, &CallWithoutHostFunction::takesCallee));
    return looksAtReceiver && !isGetter && !needsCallee && acceptsArgumentCount(form, count);
}

bool leavesPatternAsMade(BoxedCallForm form, unsigned count)
{
    const BoxedCallHandler& handler = boxedCallHandlers[static_cast<unsigned>(form)];
    return count && servesCallsByName(form, count) && handler.emit == emitFirstCallThatFits && std::ranges::any_of(handler.calls, [](const CallWithoutHostFunction& call) {
        return call.arguments[0] == ArgumentGuard::IsUntouchedRegExp;
    });
}
StubSignature signatureOfTryCallMethodOfBuiltinClass(StubKey) { return callConventionSignature({ }); }

static bool structuresBelowClassOfReceiverAreKept(const BoxedCallHandler& handler)
{
    if (!handler.receiverTypes || handler.receiverTypes->first != handler.receiverTypes->last)
        return false;
    const BuiltinTable& table = BuiltinTable::singleton();
    for (unsigned index = 0; index < table.classCount(); ++index) {
        if (const BuiltinClass& builtinClass = table.at(BuiltinClassID(index)); builtinClass.keepsStructuresOfClassesBelow && builtinClass.type == handler.receiverTypes->first)
            return true;
    }
    return false;
}

enum class MethodIsRead : uint8_t { AtTheCall, InFrontOfTheCall, InFrontOfTheCallUnlessReceiverFindsIt };

static void askAgainWhatReceiverFinds(StubAssembler& jit, BoxedCallForm form, MethodIsRead methodIsRead, GPRReg receiver = thisGPR)
{
    if (!jit.options().checksWalk || !boxedCallHandlers[static_cast<unsigned>(form)].receiverTypes)
        return;
    jit.preservingCallerSavedRegisters(false, [&](unsigned) {
        const GPRReg A0 = jit.savedScratch(StubRegister::A0);
        const GPRReg A1 = jit.savedScratch(StubRegister::A1);
        const GPRReg A2 = jit.savedScratch(StubRegister::A2);
        const GPRReg A3 = jit.savedScratch(StubRegister::A3);
        jit.subPtr(TrustedImm32(2 * sizeof(CPURegister)), sp);
        jit.storePair64(receiver, calleeGPR, sp, TrustedImm32(0));
        jit.loadPair64(sp, TrustedImm32(0), A1, A2);
        jit.addPtr(TrustedImm32(2 * sizeof(CPURegister)), sp);
        if (methodIsRead != MethodIsRead::InFrontOfTheCall)
            jit.move(TrustedImm64(JSValue::encode(JSValue())), A2);
        jit.move(instanceGPR, A0);
        jit.move(TrustedImm32(static_cast<int32_t>(form)), A3);
        jit.callOperation(Entry::operationAOTStopAtStaleMemberOfBuiltinClass, jit.savedScratch(StubRegister::Operation));
    });
}

static void branchIfMethodMayHaveBeenReplaced(StubAssembler& jit, BoxedCallForm form, GPRReg scratch, JumpList& mayHaveBeenReplaced)
{
    if (!jit.options().methodsOfBuiltinsMayBeReplaced)
        return;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(builtins.membersWritten)), scratch);
    for (BuiltinID member : BuiltinTable::singleton().membersThatHoldMethodOf(form))
        mayHaveBeenReplaced.append(jit.branchTest8(CCallHelpers::NonZero, Address(scratch, member.index())));
}

static std::optional<uint8_t> flagsOfReceiverAsItsClassMakesIt(const BoxedCallHandler& handler)
{
    if (!handler.receiverTypes)
        return std::nullopt;
    auto asMade = [](unsigned structureFlags) { return static_cast<uint8_t>(structureFlags | IsPristine); };
    switch (handler.receiverTypes->first) {
    case ArrayType: return asMade(JSArray::StructureFlags);
    case JSMapType: return asMade(JSMap::StructureFlags);
    case JSSetType: return asMade(JSSet::StructureFlags);
    case JSWeakMapType: return asMade(JSWeakMap::StructureFlags);
    case JSWeakSetType: return asMade(JSWeakSet::StructureFlags);
    default: return std::nullopt;
    }
}

struct LooksBehindFirstLook {
    const BoxedCallHandler& next;
    JumpList isNoCell { };
    JumpList isOfNextClassAndAsMade { };
    JumpList isCompiledObject { };
};

static bool isKnownAsMadeByOneCompare(const StubAssembler& jit, const BoxedCallHandler& handler)
{
    return flagsOfReceiverAsItsClassMakesIt(handler) && !jit.options().methodsOfBuiltinsMayBeReplaced && jit.options().hasOneInstance;
}

static void branchUnlessReceiverFindsBuiltin(StubAssembler& jit, const BoxedCallHandler& handler, MethodIsRead methodIsRead, GPRReg scratch, JumpList& looksUp, std::optional<StubKey> entryForProvenClass = std::nullopt, GPRReg receiver = thisGPR, LooksBehindFirstLook* looksBehind = nullptr, std::optional<StubKey> stubOfMethod = std::nullopt)
{
    JumpList isOfClassAndAsMade;
    if (handler.receiverTypes) {
        (looksBehind ? looksBehind->isNoCell : looksUp).append(jit.branchIfNotCell(receiver));
        static_assert(JSCell::typeInfoFlagsOffset() == JSCell::typeInfoTypeOffset() + 1);
        if (isKnownAsMadeByOneCompare(jit, handler))
            isOfClassAndAsMade.append(jit.branch16(CCallHelpers::Equal, Address(receiver, JSCell::typeInfoTypeOffset()), TrustedImm32(handler.receiverTypes->first | *flagsOfReceiverAsItsClassMakesIt(handler) << 8)));
        if (looksBehind) {
            looksBehind->isOfNextClassAndAsMade.append(jit.branch16(CCallHelpers::Equal, Address(receiver, JSCell::typeInfoTypeOffset()), TrustedImm32(looksBehind->next.receiverTypes->first | *flagsOfReceiverAsItsClassMakesIt(looksBehind->next) << 8)));
            looksBehind->isCompiledObject.append(jit.branchTest8(CCallHelpers::NonZero, Address(receiver, offsetOfShapeWord + 3), TrustedImm32(compiledObjectShapeBit >> 24)));
        }
        looksUp.append(jit.branchIfNotType(receiver, *handler.receiverTypes));
    }
    if (entryForProvenClass)
        jit.bindEntry(*entryForProvenClass);
    branchIfMethodMayHaveBeenReplaced(jit, handler.form, scratch, looksUp);
    if (!receiverOfBoxedCallIsObject(handler.form)) {
        askAgainWhatReceiverFinds(jit, handler.form, methodIsRead, receiver);
        return;
    }
    if (handler.emit == emitFunctionBind) {
        jit.load32(Address(receiver, JSCell::structureIDOffset()), scratch);
        looksUp.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders[0]))));
        askAgainWhatReceiverFinds(jit, handler.form, methodIsRead, receiver);
        return;
    }
    if (!jit.options().hasOneInstance) {
        loadStructure(jit, receiver, scratch);
        looksUp.append(branchIfNotOfThisRealm(jit, scratch));
    }
    if (methodIsRead != MethodIsRead::AtTheCall || !structuresBelowClassOfReceiverAreKept(handler)) {
        looksUp.append(branchIfNotPristine(jit, receiver));
        isOfClassAndAsMade.link(&jit);
        askAgainWhatReceiverFinds(jit, handler.form, methodIsRead, receiver);
        return;
    }
    Jump isAsMade = jit.branchTest8(CCallHelpers::NonZero, Address(receiver, JSCell::typeInfoFlagsOffset()), TrustedImm32(IsPristine));
    jit.load32(Address(receiver, JSCell::structureIDOffset()), scratch);
    jit.urshift32(TrustedImm32(PropertyCacheWords::shiftOfPlaceOfStructureBelowBuiltinClass), scratch);
    jit.and32(TrustedImm32(PropertyCacheWords::numberOfStructuresBelowBuiltinClasses - 1), scratch);
    jit.load32(BaseIndex(instanceGPR, scratch, CCallHelpers::TimesFour, AOT_INSTANCE_OFFSET(propertyCaches.structuresBelowBuiltinClasses)), scratch);
    looksUp.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(receiver, JSCell::structureIDOffset())));
    if (stubOfMethod && !jit.options().checksWalk) {
        jit.linkToStub(isAsMade, *stubOfMethod);
        jit.linkToStub(isOfClassAndAsMade, *stubOfMethod);
        return;
    }
    isAsMade.link(&jit);
    isOfClassAndAsMade.link(&jit);
    askAgainWhatReceiverFinds(jit, handler.form, methodIsRead, receiver);
}

void generateTryCallMethodOfBuiltinClass(StubAssembler& jit)
{
    const BoxedCallHandler& handler = boxedCallHandlers[jit.key().form()];
    JumpList misses;
    if (servesCallsByName(handler.form, jit.key().count())) {
        const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
        branchUnlessReceiverFindsBuiltin(jit, handler, MethodIsRead::AtTheCall, scratch, misses);
        handler.emit(jit, scratch, misses);
        misses.link(&jit);
    }
    jit.move(TrustedImm64(JSValue::encode(JSValue())), jit.result());
    jit.returnToCaller();
}

StubSignature signatureOfCallMethodOfBuiltinClassByValue(StubKey key) { return signatureOf(callValue(key.count())).withItsHeadInFrontOfBody(); }
StubSignature signatureOfCallMethodOfProvenBuiltinClassByValue(StubKey key) { return signatureOf(callValue(key.count())).withoutHead(); }

void generateCallMethodOfBuiltinClassByValue(StubAssembler& jit)
{
    const BoxedCallHandler& handler = boxedCallHandlers[jit.key().form()];
    const unsigned count = jit.key().count();
    std::optional<BoxedCallForm> next = formAskedBehind(handler.form);
    const StubKey forOtherReceivers = next ? callMethodOfBuiltinClassByValue(*next, count) : callValue(count);
    if (!servesCallsByName(handler.form, count) || !handler.receiverTypes) {
        jit.bindEntry(callMethodOfProvenBuiltinClassByValue(handler.form, count));
        jit.jumpToStub(forOtherReceivers);
        return;
    }
    JumpList isOtherReceiver;
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    branchUnlessReceiverFindsBuiltin(jit, handler, MethodIsRead::InFrontOfTheCall, scratch, isOtherReceiver, callMethodOfProvenBuiltinClassByValue(handler.form, count));
    JumpList leavesUndone;
    handler.emit(jit, scratch, leavesUndone);
    jit.linkToStub(leavesUndone, callValue(count));
    jit.linkToStub(isOtherReceiver, forOtherReceivers);
}

StubSignature signatureOfReadMethodAtSiteUnlessReceiverFindsIt(StubKey) { return signatureOf(readAtSite(ReceiverAtSite::IsAnyValue)); }

void generateReadMethodAtSiteUnlessReceiverFindsIt(StubAssembler& jit)
{
    const BoxedCallHandler& handler = boxedCallHandlers[jit.key().form()];
    const StubKey reads = jit.key().count() ? readAtSite(ReceiverAtSite::IsAnyValue) : readAtSiteAskingStructureFirst(ReceiverAtSite::IsAnyValue);
    if (!handler.receiverTypes) {
        jit.jumpToStub(reads);
        return;
    }
    JumpList looksUp;
    branchUnlessReceiverFindsBuiltin(jit, handler, MethodIsRead::InFrontOfTheCallUnlessReceiverFindsIt, jit.allocateScratch(StubRegister::T11), looksUp, std::nullopt, jit.operand(0));
    jit.move(TrustedImm64(JSValue::encode(JSValue())), jit.result());
    jit.returnToCaller();
    jit.linkToStub(looksUp, reads);
}

StubSignature signatureOfCallMethodThatMayBeUnread(StubKey key) { return signatureOf(callValue(key.count())).withoutHead(); }
StubSignature signatureOfCallMethodOfAnyReceiverThatMayBeUnread(StubKey key) { return signatureOf(callValue(key.count())).withoutHead(); }
StubSignature signatureOfCallMethodThatMayBeUnreadInTailPosition(StubKey key) { return signatureOf(callValue(key.count())).withoutHead(); }

void generateCallMethodThatMayBeUnread(StubAssembler& jit)
{
    const BoxedCallHandler& handler = boxedCallHandlers[jit.key().form()];
    const unsigned count = jit.key().count();
    jit.bindEntry(callMethodThatMayBeUnreadInTailPosition(handler.form, count));
    jit.linkToStub(jit.branchTest64(CCallHelpers::NonZero, calleeGPR), callValueCell(count, CallPosition::Tail));
    Jump isUnread = jit.jump();
    JumpList wasRead;
    jit.bindEntry(callMethodOfAnyReceiverThatMayBeUnread(handler.form, count));
    wasRead.append(jit.branchTest64(CCallHelpers::NonZero, calleeGPR));
    jit.linkToStub(wasRead, callValue(count));
    isUnread.link(&jit);
    jit.bindEntry(jit.key());
    JumpList callsValue;
    if (servesCallsByName(handler.form, count) && handler.receiverTypes) {
        const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
        branchUnlessReceiverFindsBuiltin(jit, handler, MethodIsRead::InFrontOfTheCallUnlessReceiverFindsIt, scratch, callsValue);
        handler.emit(jit, scratch, callsValue);
        callsValue.link(&jit);
    }
    Jump isRead = jit.branchTest64(CCallHelpers::NonZero, calleeGPR);
    BuiltinID method = BuiltinTable::singleton().methodCalledBy(handler.form);
    RELEASE_ASSERT(method);
    Vector<GPRReg, 6> preserved { thisGPR };
    for (unsigned index = 0; index < count; ++index)
        preserved.append(argumentGPR(index));
    jit.emitFunctionPrologue();
    jit.preserving(preserved.span(), [&] {
        jit.move(TrustedImm32(static_cast<int32_t>(method.index())), operationArgumentGPR(1));
        jit.move(instanceGPR, operationArgumentGPR(0));
        jit.callOperation(Entry::operationAOTMethodCalledBoxed, jit.reuseScratch(StubRegister::Operation));
        jit.move(GPRInfo::returnValueGPR, calleeGPR);
    });
    jit.emitFunctionEpilogue();
    isRead.link(&jit);
    jit.jumpToStub(callValue(count));
}

StubSignature signatureOfCallMethodOfBuiltinClassAtSite(StubKey key) { return signatureOf(callMethodAtSite(key.count())); }
StubSignature signatureOfCallMethodOfProvenBuiltinClass(StubKey key) { return signatureOf(callMethodById(key.count())); }

StubSignature signatureOfCallMethodOfBuiltinClassWithSharedPattern(StubKey key) { return signatureOf(callMethodById(key.count())); }
StubSignature signatureOfCallMethodOfBuiltinClassAtSiteWithSharedPattern(StubKey key) { return signatureOf(callMethodAtSite(key.count())); }

static void makePatternOfItsOwn(StubAssembler& jit, unsigned count, GPRReg name)
{
    const GPRReg exception = jit.reuseScratch(StubRegister::T10);
    Vector<GPRReg, 6> preserved { thisGPR, name };
    for (unsigned index = 1; index < count; ++index)
        preserved.append(argumentGPR(index));
    RELEASE_ASSERT(!preserved.contains(exception) && !preserved.contains(argumentGPR(0)) && exception != argumentGPR(0));
    jit.emitFunctionPrologue();
    jit.preserving(preserved.span(), [&] {
        jit.move(argumentGPR(0), operationArgumentGPR(1));
        jit.move(instanceGPR, operationArgumentGPR(0));
        jit.callOperation(Entry::operationAOTCopyOfSharedRegExpLiteral, jit.reuseScratch(StubRegister::Operation));
        jit.move(GPRInfo::returnValueGPR2, exception);
        jit.move(GPRInfo::returnValueGPR, argumentGPR(0));
    });
    jit.emitFunctionEpilogue();
    Jump isMade = jit.branchTestPtr(CCallHelpers::Zero, exception);
    jit.jumpToExceptionHandler();
    isMade.link(&jit);
}

static void emitCallOfMethodOfBuiltinClass(StubAssembler& jit, bool patternIsShared)
{
    const BoxedCallHandler& handler = boxedCallHandlers[jit.key().form()];
    const unsigned count = jit.key().count();
    const StubKey byName = callMethodById(count);
    const std::optional<uint32_t> boundName = jit.key().immediate();
    const GPRReg name = gprOf(stubImmediateRegister);
    RELEASE_ASSERT(!boundName || !patternIsShared);
    auto makeBoundName = [&] {
        if (boundName)
            jit.move(TrustedImm32(static_cast<int32_t>(*boundName)), name);
    };
    std::optional<BoxedCallForm> next = formAskedBehind(handler.form);
    const StubKey forOtherReceiversAtSite = next ? callMethodOfBuiltinClassAtSite(*next, count) : callMethodAtSite(count);
    const StubKey entryAtSite = patternIsShared ? callMethodOfBuiltinClassAtSiteWithSharedPattern(handler.form, count) : callMethodOfBuiltinClassAtSite(handler.form, count);
    auto goOnBy = [&](StubKey stub) {
        if (patternIsShared && count)
            makePatternOfItsOwn(jit, count, name);
        jit.jumpToStub(stub);
    };
    std::optional<StubKey> entryForProvenClass = patternIsShared ? std::nullopt : std::optional<StubKey>(jit.key().withFamily(StubFamily::CallMethodOfProvenBuiltinClass));
    if (!servesCallsByName(handler.form, count)) {
        if (entryForProvenClass)
            jit.bindEntry(*entryForProvenClass);
        makeBoundName();
        goOnBy(byName);
        if (boundName)
            return;
        jit.bindEntry(entryAtSite);
        goOnBy(forOtherReceiversAtSite);
        return;
    }

    RELEASE_ASSERT(noOverlap(name, calleeGPR, thisGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3), instanceGPR));
    const bool handlerNeverMisses = handler.emit == emitJumpToTarget && handler.firstArgument == ArgumentGuard::None;
    const bool keepsName = !boundName && !handlerNeverMisses;
    JumpList looksUp;
    branchUnlessReceiverFindsBuiltin(jit, handler, MethodIsRead::AtTheCall, calleeGPR, looksUp, entryForProvenClass);
    Label receiverFindsBuiltin = jit.label();

    if (keepsName)
        jit.move(name, calleeGPR);
    JumpList misses;
    handler.emit(jit, jit.clobberRegister(stubImmediateRegister), misses);
    RELEASE_ASSERT(!handlerNeverMisses || misses.empty());
    misses.link(&jit);
    Jump cameBySite;
    if (keepsName) {
        jit.move(calleeGPR, name);
        cameBySite = jit.branchTest32(CCallHelpers::Signed, name);
    }
    looksUp.link(&jit);
    makeBoundName();
    goOnBy(byName);
    if (boundName)
        return;

    if (keepsName) {
        cameBySite.link(&jit);
        jit.and32(TrustedImm32(INT32_MAX), name);
        goOnBy(callMethodAtSite(count));
    }

    jit.bindEntry(entryAtSite);
    JumpList isOtherReceiver;
    const BoxedCallHandler* handlerOfNext = next ? &boxedCallHandlers[static_cast<unsigned>(*next)] : nullptr;
    const bool asksNextClassAtOnce = !patternIsShared && handlerOfNext && handlerOfNext->emit == emitJumpToTarget && servesCallsByName(*next, count) && isKnownAsMadeByOneCompare(jit, handler) && isKnownAsMadeByOneCompare(jit, *handlerOfNext)
        && handler.receiverTypes->first == handler.receiverTypes->last && handlerOfNext->receiverTypes->first == handlerOfNext->receiverTypes->last && handler.receiverTypes->first != CompiledObjectType && handlerOfNext->receiverTypes->first != CompiledObjectType;
    if (asksNextClassAtOnce) {
        LooksBehindFirstLook looksBehind { *handlerOfNext };
        branchUnlessReceiverFindsBuiltin(jit, handler, MethodIsRead::AtTheCall, calleeGPR, isOtherReceiver, std::nullopt, thisGPR, &looksBehind);
        if (handlerNeverMisses) {
            JumpList missesAtSite;
            handler.emit(jit, name, missesAtSite);
            RELEASE_ASSERT(missesAtSite.empty());
        } else {
            jit.or32(TrustedImm32(INT32_MIN), name);
            jit.jump().linkTo(receiverFindsBuiltin, &jit);
        }
        std::optional<BoxedCallForm> behindNext = formAskedBehind(*next);
        const StubKey forReceiversOfNeitherClass = behindNext ? callMethodOfBuiltinClassAtSite(*behindNext, count) : callMethodAtSite(count);
        JumpList leavesDoors = looksBehind.isNoCell;
        leavesDoors.append(looksBehind.isCompiledObject);

        looksBehind.isOfNextClassAndAsMade.link(&jit);
        askAgainWhatReceiverFinds(jit, *next, MethodIsRead::AtTheCall);
        branchUnlessPasses(jit, handlerOfNext->firstArgument, argumentGPR(0), calleeGPR, leavesDoors);
        const StubKey target = handlerOfNext->target;
        if (const GPRReg owner = signatureOf(target).operands[0].gpr(); owner != thisGPR) {
            jit.recordClobbers(RegisterSet { owner });
            jit.move(thisGPR, owner);
        }
        jit.jumpToStub(target);

        jit.linkToStub(leavesDoors, callMethodAtSite(count));
        isOtherReceiver.link(&jit);
        jit.linkToStub(jit.branchIfNotType(thisGPR, handlerOfNext->receiverTypes->first), forReceiversOfNeitherClass);
        jit.jumpToStub(forOtherReceiversAtSite);
        return;
    }
    branchUnlessReceiverFindsBuiltin(jit, handler, MethodIsRead::AtTheCall, calleeGPR, isOtherReceiver);
    if (handlerNeverMisses) {
        JumpList missesAtSite;
        handler.emit(jit, name, missesAtSite);
        RELEASE_ASSERT(missesAtSite.empty());
    } else {
        jit.or32(TrustedImm32(INT32_MIN), name);
        jit.jump().linkTo(receiverFindsBuiltin, &jit);
    }
    if (!patternIsShared) {
        jit.linkToStub(isOtherReceiver, forOtherReceiversAtSite);
        return;
    }
    isOtherReceiver.link(&jit);
    goOnBy(forOtherReceiversAtSite);
}

static BoxedCallForm boxedCallFormOf(TableMethod method)
{
    switch (method) {
    case TableMethod::MapDelete: return BoxedCallForm::MapDelete;
    case TableMethod::MapGet: return BoxedCallForm::MapGet;
    case TableMethod::MapHas: return BoxedCallForm::MapHas;
    case TableMethod::MapSet: return BoxedCallForm::MapSet;
    case TableMethod::SetAdd: return BoxedCallForm::SetAdd;
    case TableMethod::SetDelete: return BoxedCallForm::SetDelete;
    case TableMethod::SetHas: return BoxedCallForm::SetHas;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StubSignature signatureOfCallMethodOfTable(StubKey key)
{
    StubSignature signature = callConventionSignature({ });
    signature.addOperand(ownerOfHashTableGPR);
    signature.addOperand(argumentGPR(0));
    if (tableMethodOf(key) == TableMethod::MapSet)
        signature.addOperand(argumentGPR(1));
    signature.immediate = gprOf(stubImmediateRegister);
    return signature;
}

StubSignature signatureOfCallMethodOfProvenTable(StubKey key) { return signatureOfCallMethodOfTable(key); }

void generateCallMethodOfTable(StubAssembler& jit)
{
    const TableMethod method = tableMethodOf(jit.key());
    const BoxedCallHandler& handler = boxedCallHandlers[static_cast<unsigned>(boxedCallFormOf(method))];
    const unsigned count = method == TableMethod::MapSet ? 2 : 1;
    const GPRReg name = gprOf(stubImmediateRegister);
    const GPRReg receiver = jit.operand(0);
    const GPRReg scratch = jit.allocateScratch(hashTableRegisterPlan.hash);
    const StubKey stubOfMethod = stubOfTableMethod(method, keyComparisonOf(jit.key()));
    RELEASE_ASSERT(handler.receiverTypes && handler.emit == emitJumpToTarget && handler.firstArgument == ArgumentGuard::None && servesCallsByName(handler.form, count));
    RELEASE_ASSERT(noOverlap(receiver, name, scratch, argumentGPR(0), argumentGPR(1), instanceGPR));
    if (method == TableMethod::MapSet)
        checkStubOperands(stubOfMethod, { receiver, argumentGPR(0), argumentGPR(1) });
    else
        checkStubOperands(stubOfMethod, { receiver, argumentGPR(0) });

    JumpList looksUp;
    branchUnlessReceiverFindsBuiltin(jit, handler, MethodIsRead::AtTheCall, scratch, looksUp, jit.key().withFamily(StubFamily::CallMethodOfProvenTable), receiver, nullptr, stubOfMethod);
    jit.addOutOfLinePart([=](StubAssembler& jit) {
        looksUp.link(&jit);
        jit.move(receiver, thisGPR);
        if (std::optional<uint32_t> number = jit.key().immediate())
            jit.move(TrustedImm32(static_cast<int32_t>(*number)), name);
        jit.jumpToStub(callMethodById(count));
    });
    jit.jumpToStub(stubOfMethod);
}

void generateCallMethodOfBuiltinClass(StubAssembler& jit)
{
    emitCallOfMethodOfBuiltinClass(jit, false);
}

void generateCallMethodOfBuiltinClassWithSharedPattern(StubAssembler& jit)
{
    emitCallOfMethodOfBuiltinClass(jit, true);
}

void generateWeakMapSet(StubAssembler& jit)
{
    jit.move(jit.clobberOperand(2), operationArgumentGPR(3));
    jit.move(jit.clobberOperand(1), operationArgumentGPR(2));
    jit.move(jit.clobberOperand(0), operationArgumentGPR(1));
    jit.callOperationAndReturn(Entry::operationAOTWeakMapSet, { });
}

static void emitStringOperationAtPositionZero(StubAssembler& jit, Entry operation, const ScopedLambda<void()>& boxResult)
{
    static_assert(noOverlap(operationArgumentGPR(2), thisGPR));
    jit.move(jit.clobberOperand(1), operationArgumentGPR(2));
    jit.move(jit.clobberOperand(0), operationArgumentGPR(1));
    jit.move(TrustedImm32(0), operationArgumentGPR(3));
    jit.callOperationAndReturn(operation, { }, boxResult);
}

void generateStringStartsWith(StubAssembler& jit)
{
    static_assert(areDistinctOnEveryISA(StubRegister::A0, StubRegister::A1, StubRegister::A2, StubRegister::A3, StubRegister::A5, StubRegister::T9));
    JumpList isNotThere;
    JumpList needsOperation;
    branchUnlessTextStandsAtPlace(jit, { jit.operand(0), jit.operand(1), jit.allocateScratch(StubRegister::A1), jit.allocateScratch(StubRegister::A2), jit.allocateScratch(StubRegister::A3), jit.allocateScratch(StubRegister::T9), jit.allocateScratch(StubRegister::A5) }, PlaceOfSoughtText::IsStart, isNotThere, needsOperation);
    jit.move(TrustedImm32(JSValue::ValueTrue), jit.result());
    jit.returnToCaller();

    isNotThere.link(&jit);
    jit.move(TrustedImm32(JSValue::ValueFalse), jit.result());
    jit.returnToCaller();

    needsOperation.link(&jit);
    emitStringOperationAtPositionZero(jit, Entry::operationAOTStringHasAt, [&] {
        jit.or32(TrustedImm32(JSValue::ValueFalse), jit.result());
    });
}

void generateStringIndexOf(StubAssembler& jit)
{
    emitStringOperationAtPositionZero(jit, Entry::operationAOTStringIndexOfFrom, [&] {
        boxInt32(jit, jit.result(), jit.result());
    });
}

static void returnSizeOfTableOrGoOnIn(StubAssembler& jit, StubKey readOfAnyName)
{
    const GPRReg receiver = jit.operand(0);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);
    if (receiver != thisGPR)
        jit.move(receiver, thisGPR == jit.result() ? thisGPR : jit.clobberRegister(thisGPR));
    JumpList goesOn;
    for (BoxedCallForm form : { BoxedCallForm::MapSize, BoxedCallForm::SetSize }) {
        const BoxedCallHandler& handler = boxedCallHandlers[static_cast<size_t>(form)];
        RELEASE_ASSERT(handler.form == form && handler.receiverTypes);
        JumpList isOfOtherKind;
        branchUnlessReceiverFindsBuiltin(jit, handler, MethodIsRead::AtTheCall, scratch, isOfOtherKind);
        handler.emit(jit, scratch, goesOn);
        isOfOtherKind.link(&jit);
    }
    goesOn.link(&jit);
    jit.jumpToStub(readOfAnyName);
}

StubSignature signatureOfGetSizeOfTable(StubKey) { return signatureOf(getById()); }
StubSignature signatureOfReadSizeOfTableAtSite(StubKey) { return signatureOf(readAtSite(ReceiverAtSite::IsAnyValue)); }

void generateGetSizeOfTable(StubAssembler& jit)
{
    returnSizeOfTableOrGoOnIn(jit, getByIdCheckingColumn());
}

void generateReadSizeOfTableAtSite(StubAssembler& jit)
{
    returnSizeOfTableOrGoOnIn(jit, readAtSite(ReceiverAtSite::IsAnyValue));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
