/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsBoxedCalls.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCompactTable.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTStringBuiltinOperationData.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsArrays.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsCallHost.h"
#include "AOTStubsCalls.h"
#include "AOTStubsCollections.h"
#include "AOTStubsHashTables.h"
#include "AOTStubsOperations.h"
#include "AOTStubsStrings.h"
#include "IndexingType.h"
#include "JSArrayBufferView.h"
#include "JSBoundFunction.h"
#include "JSCallee.h"
#include "JSObject.h"
#include "JSString.h"
#include "JSType.h"
#include "JSTypeInfo.h"
#include "NativeExecutable.h"
#include <algorithm>
#include <array>
#include <bit>
#include <iterator>
#include <limits>
#include <span>

namespace JSC { namespace AOT {

static constexpr JSTypeRange arrayTypes { ArrayType, DerivedArrayType };
static_assert(DerivedArrayType == ArrayType + 1);
static constexpr JSTypeRange typedArrayTypes { static_cast<JSType>(FirstTypedArrayType), static_cast<JSType>(LastTypedArrayTypeExcludingDataView) };

enum class ArgumentGuard : uint8_t { None, IsString, IsObject };

struct BoxedCallTarget {
    BoxedCallForm form;
    JSType receiverType;
    ArgumentGuard firstArgument;
    StubKey target;
};

static constexpr BoxedCallTarget boxedCallTargets[] = {
    { BoxedCallForm::WeakMapGet, JSWeakMapType, ArgumentGuard::None, weakMapGet() },
    { BoxedCallForm::StringStartsWith, StringType, ArgumentGuard::IsString, stringStartsWith() },
    { BoxedCallForm::MapGet, JSMapType, ArgumentGuard::None, mapGet(KeyComparison::AnyValue) },
    { BoxedCallForm::StringIndexOf, StringType, ArgumentGuard::IsString, stringIndexOf() },
    { BoxedCallForm::MapHas, JSMapType, ArgumentGuard::None, mapHas(KeyComparison::AnyValue) },
    { BoxedCallForm::SetHas, JSSetType, ArgumentGuard::None, setHas(KeyComparison::AnyValue) },
    { BoxedCallForm::WeakMapHas, JSWeakMapType, ArgumentGuard::None, weakMapHas() },
    { BoxedCallForm::WeakSetHas, JSWeakSetType, ArgumentGuard::None, weakSetHas() },
    { BoxedCallForm::SetAdd, JSSetType, ArgumentGuard::None, setAdd(KeyComparison::AnyValue) },
    { BoxedCallForm::WeakMapSet, JSWeakMapType, ArgumentGuard::IsObject, weakMapSet() },
    { BoxedCallForm::MapSet, JSMapType, ArgumentGuard::None, mapSet(KeyComparison::AnyValue) },
    { BoxedCallForm::MapDelete, JSMapType, ArgumentGuard::None, mapDelete(KeyComparison::AnyValue) },
    { BoxedCallForm::SetDelete, JSSetType, ArgumentGuard::None, setDelete(KeyComparison::AnyValue) },
};

static const BoxedCallTarget& boxedCallTargetOf(BoxedCallForm form)
{
    const BoxedCallTarget* target = std::ranges::find(boxedCallTargets, form, &BoxedCallTarget::form);
    RELEASE_ASSERT(target != std::end(boxedCallTargets));
    return *target;
}

// The executable remembers what the table says of its function: one more than the index of the entry, or that there is none. Zero: nobody has looked yet.
constexpr int32_t hasNoBoxedCallEntry = UINT8_MAX;
static_assert(StubWords::numberOfBoxedCallEntries < hasNoBoxedCallEntry);
constexpr int32_t offsetOfBoxedCallTable = Instance::offsetOfBoxedCalls();
constexpr StubRegister boxedCallEntryRegister = StubRegister::T9;

void emitBoxedCallDispatch(StubAssembler& jit, const CallRegisters& registers, unsigned count, JumpList& hasNoBoxedCall)
{
    if (count < BoxedCallEntry::minArgumentCount || count > BoxedCallEntry::maxArgumentCount)
        return;
    static_assert(BoxedCallEntry::maxArgumentCount == 4);
    const GPRReg executable = registers.scratch;
    const GPRReg entry = jit.allocateScratch(boxedCallEntryRegister);
    RELEASE_ASSERT(noOverlap(executable, registers.word, entry, instanceGPR, calleeGPR, thisGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3)));
    constexpr unsigned numberOfForms = descriptorOf(StubFamily::CallBoxedBuiltin).numberOfForms;

    jit.load8(Address(executable, NativeExecutable::offsetOfIndexOfBoxedCall()), entry);
    Jump isNotLookedUp = jit.branchTest32(CCallHelpers::Zero, entry);
    Label isLookedUp = jit.label();
    hasNoBoxedCall.append(jit.branch32(CCallHelpers::Equal, entry, TrustedImm32(hasNoBoxedCallEntry)));
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

// Once for an executable. With the registers of emitBoxedCallDispatch(): takes the executable, leaves in the entry's register what it has written into the executable.
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

static void checkCallConventionOperands(StubKey target, unsigned count)
{
    static constexpr std::array registers { thisGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3) };
    static_assert(registers.size() == BoxedCallEntry::maxArgumentCount + 1);
    checkStubOperands(target, std::span(registers).first(count + 1));
}

static void checkCalleeRealm(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    jit.loadPtr(Address(calleeGPR, JSCallee::offsetOfScopeChain()), scratch);
    callsHostFunction.append(jit.branchPtr(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject))));
}

static void emitArrayIsArray(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    Jump notCell = jit.branchIfNotCell(argumentGPR(0));
    jit.load8(Address(argumentGPR(0), JSCell::typeInfoTypeOffset()), scratch);
    callsHostFunction.append(jit.branch32(CCallHelpers::Equal, scratch, TrustedImm32(ProxyObjectType)));
    jit.sub32(TrustedImm32(arrayTypes.first), scratch);
    Jump isArray = jit.branch32(CCallHelpers::BelowOrEqual, scratch, TrustedImm32(arrayTypes.last - arrayTypes.first));
    notCell.link(&jit);
    jit.move(TrustedImm32(JSValue::ValueFalse), jit.result());
    jit.returnToCaller();
    isArray.link(&jit);
    jit.move(TrustedImm32(JSValue::ValueTrue), jit.result());
    jit.returnToCaller();
}

static void returnBoxedBoolean(StubAssembler& jit, bool value)
{
    jit.move(TrustedImm32(value ? JSValue::ValueTrue : JSValue::ValueFalse), jit.result());
    jit.returnToCaller();
}

// Two values with the same bits are the same value. Of two values with other bits, only two numbers, two strings or two BigInts can be.
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

// A table that was filled before the program was registered has the engine's storage, which the host function reads.
static void emitTableSize(StubAssembler& jit, JSType receiverType, GPRReg scratch, JumpList& callsHostFunction)
{
    const GPRReg table = scratch;
    callsHostFunction.append(jit.branchIfNotType(thisGPR, receiverType));
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
    callsHostFunction.append(jit.branchIfNotType(thisGPR, typedArrayTypes));
    callsHostFunction.append(jit.branchTest8(CCallHelpers::NonZero, Address(thisGPR, JSArrayBufferView::offsetOfMode()), TrustedImm32(resizabilityAndAutoLengthMask)));
    jit.load64(Address(thisGPR, JSArrayBufferView::offsetOfLength()), length);
    callsHostFunction.append(jit.branch64(CCallHelpers::Above, length, TrustedImm64(std::numeric_limits<int32_t>::max())));
    boxInt32(jit, length, jit.result());
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

// Only an object or a BigInt is asked for toJSON, and `undefined` has no text.
static void emitJSONStringify(StubAssembler& jit, unsigned count, GPRReg scratch, JumpList& callsHostFunction)
{
    constexpr GPRReg value = argumentGPR(0);
    if (count == 2)
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

// A flat 8-bit string that starts and ends with a printable ASCII character other than the space is its own result.
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
    callsHostFunction.append(jit.branchIfNotType(thisGPR, StringType));
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

// The head of the operation's calls returns a string without a letter to convert.
static void emitStringToLowerCase(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(gprOf(StubRegister::A1) == operationArgumentGPR(1));
    callsHostFunction.append(jit.branchIfNotType(thisGPR, StringType));
    checkCalleeRealm(jit, scratch, callsHostFunction);
    jit.move(thisGPR, jit.reuseScratch(StubRegister::A1));
    jit.jumpToStub(callOperation(OperationCall::Returns::Value, OperationCall::FirstArgument::Instance).withImmediate(offsetInRuntimeTable(Entry::operationAOTStringToLowerCase)));
}

static void emitStringSlice(StubAssembler& jit, unsigned count, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(argumentGPR(1) == gprOf(StubRegister::A1) && noOverlap(gprOf(StubRegister::A2), gprOf(StubRegister::A3), thisGPR, calleeGPR, argumentGPR(0), argumentGPR(1)));

    callsHostFunction.append(jit.branchIfNotType(thisGPR, StringType));
    checkCalleeRealm(jit, scratch, callsHostFunction);
    callsHostFunction.append(jit.branchIfNotInt32(argumentGPR(0)));
    if (count == 2)
        callsHostFunction.append(jit.branchIfNotInt32(argumentGPR(1)));

    const GPRReg length = scratch;
    const GPRReg string = jit.reuseScratch(StubRegister::A1);
    const GPRReg start = jit.reuseScratch(StubRegister::A2);
    const GPRReg end = jit.reuseScratch(StubRegister::A3);
    RELEASE_ASSERT(noOverlap(length, string, start, end, thisGPR, argumentGPR(0)));
    checkStubOperands(substring(), { string, start, end });

    auto toPosition = [&](GPRReg relativeIndex, GPRReg position) {
        jit.signExtend32ToPtr(relativeIndex, position);
        Jump countsFromStart = jit.branch64(CCallHelpers::GreaterThanOrEqual, position, TrustedImm32(0));
        jit.add64(length, position);
        Jump isInside = jit.branch64(CCallHelpers::GreaterThanOrEqual, position, TrustedImm32(0));
        jit.move(TrustedImm32(0), position);
        Jump isAtStart = jit.jump();
        countsFromStart.link(&jit);
        Jump isNotPastEnd = jit.branch64(CCallHelpers::LessThanOrEqual, position, length);
        jit.move(length, position);
        isNotPastEnd.link(&jit);
        isInside.link(&jit);
        isAtStart.link(&jit);
    };
    jit.loadPtr(Address(thisGPR, JSString::offsetOfValue()), length);
    loadStringLength(jit, thisGPR, length, length);
    if (count == 2)
        toPosition(argumentGPR(1), end);
    else
        jit.move(length, end);
    toPosition(argumentGPR(0), start);
    jit.move(thisGPR, string);
    jit.jumpToStub(substring());
}

// The element is found by its bits, a string also by its text.
static void emitArraySearch(StubAssembler& jit, BoxedCallForm form, GPRReg scratch, JumpList& callsHostFunction)
{
    constexpr GPRReg searched = argumentGPR(0);
    const GPRReg cursor = jit.reuseScratch(StubRegister::A1);
    const GPRReg flags = jit.reuseScratch(StubRegister::A2);
    const GPRReg element = jit.reuseScratch(StubRegister::A3);
    const GPRReg searchedText = jit.allocateScratch(StubRegister::T11);
    const GPRReg text = jit.allocateScratch(StubRegister::T13);
    const GPRReg shape = scratch;
    const GPRReg searchedLength = scratch;
    RELEASE_ASSERT(noOverlap(searched, cursor, flags, element, searchedText, text, scratch, calleeGPR, thisGPR, instanceGPR));

    auto loadSearchedText = [&] {
        jit.loadPtr(Address(searched, JSString::offsetOfValue()), searchedText);
    };
    auto loadSearchedLength = [&] {
        jit.load32(Address(searchedText, StringImpl::lengthMemoryOffset()), searchedLength);
    };

    callsHostFunction.append(jit.branchIfNotType(thisGPR, arrayTypes));
    callsHostFunction.append(branchIfNotPristine(jit, thisGPR));
    jit.load8(Address(thisGPR, JSCell::indexingTypeAndMiscOffset()), shape);
    jit.and32(TrustedImm32(IndexingShapeMask), shape);
    jit.move(TrustedImm32(0), searchedText);

    JumpList isReady;
    Jump isInt32 = jit.branchIfInt32(searched);
    callsHostFunction.append(jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(ContiguousShape)));
    callsHostFunction.append(jit.branchIfNumber(searched));
    callsHostFunction.append(jit.branch64(CCallHelpers::Equal, searched, TrustedImm64(JSValue::ValueUndefined)));
    isReady.append(jit.branchIfNotCell(searched));
    jit.load8(Address(searched, JSCell::typeInfoTypeOffset()), element);
    callsHostFunction.append(jit.branch32(CCallHelpers::Equal, element, TrustedImm32(HeapBigIntType)));
    isReady.append(jit.branch32(CCallHelpers::NotEqual, element, TrustedImm32(StringType)));
    loadSearchedText();
    callsHostFunction.append(jit.branchIfRopeStringImpl(searchedText));
    loadSearchedLength();
    isReady.append(jit.jump());
    isInt32.link(&jit);
    callsHostFunction.append(jit.branch32(CCallHelpers::NotEqual, shape, TrustedImm32(Int32Shape)));
    isReady.link(&jit);

    constexpr int32_t maxElementsSearchedOneByOne = 32;
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), cursor);
    callsHostFunction.append(jit.branch32(CCallHelpers::Above, Address(cursor, Butterfly::offsetOfPublicLength()), TrustedImm32(maxElementsSearchedOneByOne)));

    JumpList isFound;
    JumpList isAnother;
    Label next = jit.label();
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), element);
    jit.load32(Address(element, Butterfly::offsetOfPublicLength()), text);
    jit.getEffectiveAddress(BaseIndex(element, text, CCallHelpers::TimesEight), element);
    Jump isAtEnd = jit.branchPtr(CCallHelpers::AboveOrEqual, cursor, element);
    jit.load64(Address(cursor), element);
    isFound.append(jit.branch64(CCallHelpers::Equal, element, searched));
    isAnother.append(jit.branchTestPtr(CCallHelpers::Zero, searchedText));
    isAnother.append(jit.branchTest64(CCallHelpers::Zero, element));
    isAnother.append(jit.branchIfNotCell(element));
    isAnother.append(jit.branchIfNotType(element, StringType));
    jit.loadPtr(Address(element, JSString::offsetOfValue()), text);
    callsHostFunction.append(jit.branchIfRopeStringImpl(text));
    isFound.append(jit.branchPtr(CCallHelpers::Equal, text, searchedText));
    isAnother.append(jit.branch32(CCallHelpers::NotEqual, Address(text, StringImpl::lengthMemoryOffset()), searchedLength));
    jit.load32(Address(text, StringImpl::flagsOffset()), flags);
    jit.and32(Address(searchedText, StringImpl::flagsOffset()), flags);
    isAnother.append(jit.branchTest32(CCallHelpers::NonZero, flags, TrustedImm32(StringImpl::flagIsAtom())));
    JumpList differs;
    branchIfCharactersDiffer(jit, { text, searchedText, searchedLength, flags, element }, differs, callsHostFunction);
    isFound.append(jit.jump());
    differs.link(&jit);
    loadSearchedText();
    loadSearchedLength();
    isAnother.link(&jit);
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), cursor);
    jit.jump().linkTo(next, &jit);

    if (form == BoxedCallForm::ArrayIncludes) {
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

// A transition is recorded for a compiled object that is on no chain, so what is left of the built-in is the test of extensibility and the store.
static void emitObjectSetPrototypeOf(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(sizeof(PrototypeTransition) == 16 && hasOneBitSet(numberOfPrototypeTransitions));
    static_assert(indexOfPrototypeTransition(5 << prototypeTransitionHashShift, 3 << prototypeTransitionHashShift) == 6);
    constexpr GPRReg object = argumentGPR(0);
    constexpr GPRReg prototype = argumentGPR(1);
    const GPRReg transition = scratch;
    const GPRReg structureID = jit.allocateScratch(StubRegister::T11);
    RELEASE_ASSERT(noOverlap(object, prototype, transition, structureID, calleeGPR, thisGPR, instanceGPR));
    if (jit.options().validates) {
        callsHostFunction.append(jit.jump());
        return;
    }

    callsHostFunction.append(jit.branchIfNotCell(object));
    jit.load32(Address(object, JSCell::structureIDOffset()), structureID);
    jit.xor32(prototype, structureID, transition);
    jit.urshift32(TrustedImm32(prototypeTransitionHashShift), transition);
    jit.and32(TrustedImm32(numberOfPrototypeTransitions - 1), transition);
    jit.lshift32(TrustedImm32(4), transition);
    jit.addPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(objects.prototypeTransitions)), transition);
    callsHostFunction.append(jit.branch64(CCallHelpers::NotEqual, Address(transition, OBJECT_OFFSETOF(PrototypeTransition, prototype)), prototype));
    callsHostFunction.append(jit.branch32(CCallHelpers::NotEqual, Address(transition, OBJECT_OFFSETOF(PrototypeTransition, before)), structureID));
    callsHostFunction.append(jit.branchTest32(CCallHelpers::NonZero, Address(object, offsetOfOrderWord), TrustedImm32(static_cast<int32_t>(orderWordIsNotExtensible))));
    jit.load32(Address(transition, OBJECT_OFFSETOF(PrototypeTransition, after)), structureID);
    jit.store32(structureID, Address(object, JSCell::structureIDOffset()));
    jit.load8(Address(object, JSCell::typeInfoFlagsOffset()), structureID);
    jit.or32(TrustedImm32(IsPristine), structureID);
    jit.store8(structureID, Address(object, JSCell::typeInfoFlagsOffset()));
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

static void emitArrayPush(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(noOverlap(gprOf(StubRegister::A2), thisGPR) && noOverlap(gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(arrayPushKindRegister)));

    callsHostFunction.append(jit.branchIfNotType(thisGPR, arrayTypes));
    checkCalleeRealm(jit, scratch, callsHostFunction);

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

static void emitArraySlice(StubAssembler& jit, unsigned count, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(argumentGPR(1) == gprOf(StubRegister::A1) && noOverlap(gprOf(StubRegister::A2), gprOf(StubRegister::A3), thisGPR, argumentGPR(0), argumentGPR(1)));

    callsHostFunction.append(jit.branchIfNotType(thisGPR, arrayTypes));
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

// What FunctionBuiltinEmitter::generateFunctionBind() emits where the callee is known.
static void emitFunctionBind(StubAssembler& jit, unsigned count, GPRReg allocator, JumpList& callsHostFunction)
{
    constexpr unsigned kind = static_cast<unsigned>(InlineAllocation::BoundFunction);
    constexpr unsigned maxBoundArguments = JSBoundFunction::maxEmbeddedArgs;
    static_assert(BoxedCallEntry::maxArgumentCount == 1 + maxBoundArguments && BoxedCallEntry::maxArgumentCount <= numberOfArgumentGPRs);
    static_assert(JSBoundFunction::offsetOfCanConstruct() == JSBoundFunction::offsetOfBoundArgsLength() + sizeof(uint32_t) && JSBoundFunction::offsetOfCanConstruct() + sizeof(uint32_t) == sizeof(JSBoundFunction));
    const unsigned numberOfBoundArguments = count ? count - 1 : 0;
    const GPRReg bound = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T13);
    RELEASE_ASSERT(noOverlap(bound, scratch, allocator, calleeGPR, thisGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3)));

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

static void emitBoxedCallTarget(StubAssembler& jit, const BoxedCallTarget& target, unsigned count, JumpList& callsHostFunction)
{
    checkCallConventionOperands(target.target, count);
    callsHostFunction.append(jit.branchIfNotType(thisGPR, target.receiverType));
    switch (target.firstArgument) {
    case ArgumentGuard::None:
        break;
    case ArgumentGuard::IsString:
        callsHostFunction.append(jit.branchIfNotCell(argumentGPR(0)));
        callsHostFunction.append(jit.branchIfNotType(argumentGPR(0), StringType));
        break;
    case ArgumentGuard::IsObject:
        callsHostFunction.append(jit.branchIfNotCell(argumentGPR(0)));
        callsHostFunction.append(jit.branchIfNotObject(argumentGPR(0)));
        break;
    }
    jit.jumpToStub(target.target);
}

static bool readsReceiver(BoxedCallForm form)
{
    switch (form) {
    case BoxedCallForm::ArrayIsArray:
    case BoxedCallForm::IsNaN:
    case BoxedCallForm::JSONStringify:
    case BoxedCallForm::ObjectGetPrototypeOf:
    case BoxedCallForm::ObjectIs:
    case BoxedCallForm::ObjectSetPrototypeOf:
        return false;
    case BoxedCallForm::ArrayIncludes:
    case BoxedCallForm::ArrayIndexOf:
    case BoxedCallForm::ArrayPush:
    case BoxedCallForm::ArraySlice:
    case BoxedCallForm::FunctionBind:
    case BoxedCallForm::MapDelete:
    case BoxedCallForm::MapGet:
    case BoxedCallForm::MapHas:
    case BoxedCallForm::MapSet:
    case BoxedCallForm::MapSize:
    case BoxedCallForm::SetAdd:
    case BoxedCallForm::SetDelete:
    case BoxedCallForm::SetHas:
    case BoxedCallForm::SetSize:
    case BoxedCallForm::StringIndexOf:
    case BoxedCallForm::StringSlice:
    case BoxedCallForm::StringStartsWith:
    case BoxedCallForm::StringToLowerCase:
    case BoxedCallForm::StringTrim:
    case BoxedCallForm::TypedArrayLength:
    case BoxedCallForm::WeakMapGet:
    case BoxedCallForm::WeakMapHas:
    case BoxedCallForm::WeakMapSet:
    case BoxedCallForm::WeakSetHas:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void generateCallBoxedBuiltin(StubAssembler& jit)
{
    const BoxedCallForm form = static_cast<BoxedCallForm>(jit.key().form());
    const unsigned count = jit.key().count();
    const StubKey hostCall = callHostFunctionWithoutBoxedCall(count);
    if (!acceptsArgumentCount(form, count)) {
        jit.jumpToStub(hostCall);
        return;
    }

    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    JumpList callsHostFunction;
    if (readsReceiver(form))
        callsHostFunction.append(jit.branchIfNotCell(thisGPR));
    switch (form) {
    case BoxedCallForm::ArrayIncludes:
    case BoxedCallForm::ArrayIndexOf:
        emitArraySearch(jit, form, scratch, callsHostFunction);
        break;
    case BoxedCallForm::ArrayIsArray:
        emitArrayIsArray(jit, scratch, callsHostFunction);
        break;
    case BoxedCallForm::ArrayPush:
        emitArrayPush(jit, scratch, callsHostFunction);
        break;
    case BoxedCallForm::ArraySlice:
        emitArraySlice(jit, count, scratch, callsHostFunction);
        break;
    case BoxedCallForm::FunctionBind:
        emitFunctionBind(jit, count, scratch, callsHostFunction);
        break;
    case BoxedCallForm::IsNaN:
        emitIsNaN(jit, scratch, callsHostFunction);
        break;
    case BoxedCallForm::JSONStringify:
        emitJSONStringify(jit, count, scratch, callsHostFunction);
        break;
    case BoxedCallForm::MapSize:
        emitTableSize(jit, JSMapType, scratch, callsHostFunction);
        break;
    case BoxedCallForm::ObjectGetPrototypeOf:
        emitObjectGetPrototypeOf(jit, scratch, callsHostFunction);
        break;
    case BoxedCallForm::ObjectIs:
        emitObjectIs(jit, scratch, callsHostFunction);
        break;
    case BoxedCallForm::ObjectSetPrototypeOf:
        emitObjectSetPrototypeOf(jit, scratch, callsHostFunction);
        break;
    case BoxedCallForm::SetSize:
        emitTableSize(jit, JSSetType, scratch, callsHostFunction);
        break;
    case BoxedCallForm::StringSlice:
        emitStringSlice(jit, count, scratch, callsHostFunction);
        break;
    case BoxedCallForm::StringToLowerCase:
        emitStringToLowerCase(jit, scratch, callsHostFunction);
        break;
    case BoxedCallForm::StringTrim:
        emitStringTrim(jit, scratch, callsHostFunction);
        break;
    case BoxedCallForm::TypedArrayLength:
        emitTypedArrayLength(jit, scratch, callsHostFunction);
        break;
    case BoxedCallForm::MapDelete:
    case BoxedCallForm::MapGet:
    case BoxedCallForm::MapHas:
    case BoxedCallForm::MapSet:
    case BoxedCallForm::SetAdd:
    case BoxedCallForm::SetDelete:
    case BoxedCallForm::SetHas:
    case BoxedCallForm::StringIndexOf:
    case BoxedCallForm::StringStartsWith:
    case BoxedCallForm::WeakMapGet:
    case BoxedCallForm::WeakMapHas:
    case BoxedCallForm::WeakMapSet:
    case BoxedCallForm::WeakSetHas:
        emitBoxedCallTarget(jit, boxedCallTargetOf(form), count, callsHostFunction);
        break;
    }
    jit.linkToStub(callsHostFunction, hostCall);
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
