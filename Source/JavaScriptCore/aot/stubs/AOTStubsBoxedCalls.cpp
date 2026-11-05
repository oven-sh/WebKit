/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsBoxedCalls.h"

#if ENABLE(AOT)

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
#include "AOTStubsHashTables.h"
#include "AOTStubsOperations.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTStubsRegExp.h"
#include "AOTStubsStrings.h"
#include "AOTVector.h"
#include "IndexingType.h"
#include "JSArrayBufferView.h"
#include "JSBoundFunction.h"
#include "JSCallee.h"
#include "JSGlobalObject.h"
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
    jit.loadPtr(Address(calleeGPR, JSCallee::offsetOfScopeChain()), scratch);
    callsHostFunction.append(jit.branchPtr(CCallHelpers::NotEqual, scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject))));
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

    static constexpr int32_t maxElementsSearchedOneByOne = 32;
    jit.loadPtr(Address(thisGPR, JSObject::butterflyOffset()), cursor);
    callsHostFunction.append(jit.branch32(CCallHelpers::Above, Address(cursor, Butterfly::offsetOfPublicLength()), TrustedImm32(maxElementsSearchedOneByOne)));

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

    Label nextWord = jit.label();
    isFound.append(jit.branch64(CCallHelpers::Equal, Address(cursor), searched));
    jit.addPtr(TrustedImm32(sizeof(EncodedJSValue)), cursor);
    jit.branchPtr(CCallHelpers::Below, cursor, end).linkTo(nextWord, &jit);
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

static void emitArrayPush(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    static_assert(noOverlap(gprOf(StubRegister::A2), thisGPR) && noOverlap(gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(arrayPushKindRegister)));

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

static void emitArraySort(StubAssembler& jit, GPRReg scratch, JumpList& callsHostFunction)
{
    if (jit.key().count() == 1)
        callsHostFunction.append(jit.branch64(CCallHelpers::NotEqual, argumentGPR(0), TrustedImm64(JSValue::ValueUndefined)));
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
    for (unsigned i = count; i--;)
        jit.move(argumentGPR(i), jit.reuseScratch(operands[i + 1]));
    jit.move(thisGPR, jit.reuseScratch(operands[0]));
    jit.jumpToCallOfOperation(count == 3 ? Entry::operationAOTArraySpliceItemIntoAnyList : Entry::operationAOTArraySpliceOfAnyList);
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

enum class ArgumentGuard : uint8_t { None, IsString, IsObject, IsFunction, IsUntouchedRegExp };

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
    isLeftToOperation.append(jit.branch32(CCallHelpers::NotEqual, kind, TrustedImm32(Int32Shape)));
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
static constexpr CallWithoutHostFunction arrayPopCalls[] = { { { anything, anything }, Entry::operationAOTArrayPopOfAnyList } };
static constexpr CallWithoutHostFunction arrayShiftCalls[] = { { { anything, anything }, Entry::operationAOTArrayShiftOfAnyList } };
static constexpr CallWithoutHostFunction arrayUnshiftCalls[] = { { { anything, anything }, Entry::operationAOTArrayUnshiftOfAnyList } };
static constexpr CallWithoutHostFunction mapClearCalls[] = { { { anything, anything }, Entry::operationAOTMapClearOfAnyMap } };
static constexpr CallWithoutHostFunction setClearCalls[] = { { { anything, anything }, Entry::operationAOTSetClearOfAnySet } };
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
    { BoxedCallForm::ArrayIsArray, 1, 1, std::nullopt, emitArrayIsArray },
    { BoxedCallForm::ArrayJoin, 1, 1, arrayTypes, emitFirstCallThatFits, { }, anything, arrayJoinCalls },
    { BoxedCallForm::ArrayPop, 0, 0, arrayTypes, emitArrayPop, { }, anything, arrayPopCalls },
    { BoxedCallForm::ArrayPush, 1, 1, arrayTypes, emitArrayPush },
    { BoxedCallForm::ArrayShift, 0, 0, arrayTypes, emitFirstCallThatFits, { }, anything, arrayShiftCalls },
    { BoxedCallForm::ArraySlice, 0, 2, arrayTypes, emitArraySlice },
    { BoxedCallForm::ArraySort, 0, 1, arrayTypes, emitArraySort },
    { BoxedCallForm::ArraySplice, 2, 3, arrayTypes, emitArraySplice },
    { BoxedCallForm::ArrayToString, 0, 0, arrayTypes, emitFirstCallThatFits, { }, anything, arrayToStringCalls },
    { BoxedCallForm::ArrayUnshift, 1, 1, arrayTypes, emitFirstCallThatFits, { }, anything, arrayUnshiftCalls },
    { BoxedCallForm::FunctionBind, 0, std::numeric_limits<unsigned>::max(), std::nullopt, emitFunctionBind },
    { BoxedCallForm::IsNaN, 1, 1, std::nullopt, emitIsNaN },
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
    branchUnlessPasses(jit, handler.receiver, thisGPR, scratch, callsHostFunction);
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

void generateCallBoxedBuiltin(StubAssembler& jit)
{
    const BoxedCallHandler& handler = boxedCallHandlers[jit.key().form()];
    const StubKey hostCall = callHostFunctionWithoutBoxedCall(jit.key().count());
    if (!acceptsArgumentCount(handler.form, jit.key().count())) {
        jit.jumpToStub(hostCall);
        return;
    }

    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    JumpList callsHostFunction;
    if (handler.receiverTypes) {
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
    bool needsCallee = handler.emit == emitFunctionBind || handler.emit == emitJSONStringify
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

enum class MethodIsRead : uint8_t { AtTheCall, InFrontOfTheCall };

static void branchIfMethodMayHaveBeenReplaced(StubAssembler& jit, BoxedCallForm form, GPRReg scratch, JumpList& mayHaveBeenReplaced)
{
    if (!jit.options().methodsOfBuiltinsMayBeReplaced)
        return;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(builtins.membersWritten)), scratch);
    for (BuiltinID member : BuiltinTable::singleton().membersThatHoldMethodOf(form))
        mayHaveBeenReplaced.append(jit.branchTest8(CCallHelpers::NonZero, Address(scratch, member.index())));
}

static void branchUnlessReceiverFindsBuiltin(StubAssembler& jit, const BoxedCallHandler& handler, MethodIsRead methodIsRead, GPRReg scratch, JumpList& looksUp, std::optional<StubKey> entryForProvenClass = std::nullopt)
{
    if (handler.receiverTypes) {
        looksUp.append(jit.branchIfNotCell(thisGPR));
        looksUp.append(jit.branchIfNotType(thisGPR, *handler.receiverTypes));
    }
    if (entryForProvenClass)
        jit.bindEntry(*entryForProvenClass);
    branchIfMethodMayHaveBeenReplaced(jit, handler.form, scratch, looksUp);
    if (!receiverOfBoxedCallIsObject(handler.form))
        return;
    if (methodIsRead == MethodIsRead::InFrontOfTheCall || !structuresBelowClassOfReceiverAreKept(handler)) {
        looksUp.append(branchIfNotPristine(jit, thisGPR));
        return;
    }
    Jump isAsMade = jit.branchTest8(CCallHelpers::NonZero, Address(thisGPR, JSCell::typeInfoFlagsOffset()), TrustedImm32(IsPristine));
    jit.load32(Address(thisGPR, JSCell::structureIDOffset()), scratch);
    jit.urshift32(TrustedImm32(PropertyCacheWords::shiftOfPlaceOfStructureBelowBuiltinClass), scratch);
    jit.and32(TrustedImm32(PropertyCacheWords::numberOfStructuresBelowBuiltinClasses - 1), scratch);
    jit.load32(BaseIndex(instanceGPR, scratch, CCallHelpers::TimesFour, AOT_INSTANCE_OFFSET(propertyCaches.structuresBelowBuiltinClasses)), scratch);
    looksUp.append(jit.branch32(CCallHelpers::NotEqual, scratch, Address(thisGPR, JSCell::structureIDOffset())));
    isAsMade.link(&jit);
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

StubSignature signatureOfCallMethodOfBuiltinClassByValue(StubKey key) { return signatureOf(callValue(key.count())); }
StubSignature signatureOfCallMethodOfProvenBuiltinClassByValue(StubKey key) { return signatureOf(callValue(key.count())); }

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

StubSignature signatureOfCallMethodThatMayBeUnread(StubKey key) { return signatureOf(callValue(key.count())); }

void generateCallMethodThatMayBeUnread(StubAssembler& jit)
{
    const BoxedCallHandler& handler = boxedCallHandlers[jit.key().form()];
    const unsigned count = jit.key().count();
    JumpList callsValue;
    if (servesCallsByName(handler.form, count) && receiverOfBoxedCallIsObject(handler.form)) {
        callsValue.append(branchIfNotPristine(jit, thisGPR));
        const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
        branchIfMethodMayHaveBeenReplaced(jit, handler.form, scratch, callsValue);
        handler.emit(jit, scratch, callsValue);
        callsValue.link(&jit);
    }
    Jump isRead = jit.branchTest64(CCallHelpers::NonZero, calleeGPR);
    Vector<GPRReg, 6> preserved { thisGPR };
    for (unsigned index = 0; index < count; ++index)
        preserved.append(argumentGPR(index));
    jit.emitFunctionPrologue();
    jit.preserving(preserved.span(), [&] {
        jit.move(TrustedImm32(static_cast<int32_t>(handler.form)), operationArgumentGPR(1));
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
    const GPRReg name = jit.immediate();
    std::optional<BoxedCallForm> next = formAskedBehind(handler.form);
    const StubKey forOtherReceiversAtSite = next ? callMethodOfBuiltinClassAtSite(*next, count) : callMethodAtSite(count);
    const StubKey entryAtSite = patternIsShared ? callMethodOfBuiltinClassAtSiteWithSharedPattern(handler.form, count) : callMethodOfBuiltinClassAtSite(handler.form, count);
    auto goOnBy = [&](StubKey stub) {
        if (patternIsShared && count)
            makePatternOfItsOwn(jit, count, name);
        jit.jumpToStub(stub);
    };
    std::optional<StubKey> entryForProvenClass = patternIsShared ? std::nullopt : std::optional<StubKey>(callMethodOfProvenBuiltinClass(handler.form, count));
    if (!servesCallsByName(handler.form, count)) {
        if (entryForProvenClass)
            jit.bindEntry(*entryForProvenClass);
        goOnBy(byName);
        jit.bindEntry(entryAtSite);
        goOnBy(forOtherReceiversAtSite);
        return;
    }

    RELEASE_ASSERT(noOverlap(name, calleeGPR, thisGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3), instanceGPR));
    JumpList looksUp;
    branchUnlessReceiverFindsBuiltin(jit, handler, MethodIsRead::AtTheCall, calleeGPR, looksUp, entryForProvenClass);
    Label receiverFindsBuiltin = jit.label();

    jit.move(name, calleeGPR);
    JumpList misses;
    handler.emit(jit, jit.clobberRegister(stubImmediateRegister), misses);
    misses.link(&jit);
    jit.move(calleeGPR, name);
    looksUp.link(&jit);
    goOnBy(byName);

    jit.bindEntry(entryAtSite);
    JumpList isOtherReceiver;
    branchUnlessReceiverFindsBuiltin(jit, handler, MethodIsRead::AtTheCall, calleeGPR, isOtherReceiver);
    static_assert(sizeof(uint16_t) == InstanceLayout::sizeOfIndexOfSiteAnswers);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.namesOfSitesByDistance)), calleeGPR);
    jit.load16(BaseIndex(calleeGPR, name, CCallHelpers::TimesOne), name);
    jit.jump().linkTo(receiverFindsBuiltin, &jit);
    if (!patternIsShared) {
        jit.linkToStub(isOtherReceiver, forOtherReceiversAtSite);
        return;
    }
    isOtherReceiver.link(&jit);
    goOnBy(forOtherReceiversAtSite);
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
