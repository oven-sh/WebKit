/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCodeUnits.h"
#include "AOTCompiledCode.h"
#include "AOTConvention.h"
#include "AOTImageFormat.h"
#include "AOTStubCalls.h"
#include "AOTStubFrames.h"
#include "AOTStubRegisters.h"
#include "AOTStubTestSupport.h"
#include "AOTStubWords.h"
#include "AOTStubsArrays.h"
#include "AOTStubsCalls.h"
#include "AOTStubsCoroutines.h"
#include "AOTStubsOperations.h"
#include "AOTStubsPolls.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTStubsValues.h"
#include "AOTVector.h"
#include "CPU.h"
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <span>
#include <wtf/BitSet.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashSet.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

#if OS(UNIX)
#include <sys/mman.h>
#endif

namespace JSC { namespace AOT {

static constexpr StubKey familyKey { StubFamily::GetById };

static_assert(!StubKey().hasFamily() && !exists(StubKey()) && !StubKey().bits());
static_assert(familyKey.hasFamily() && familyKey.family() == StubFamily::GetById);
static_assert(!familyKey.form() && !familyKey.count());
static_assert(familyKey.callerFrame() == CallerFrame::HasFrame);
static_assert(!familyKey.operandRegister() && !familyKey.immediate());

static_assert(familyKey.withForm(5).form() == 5);
static_assert(familyKey.withCount(7).count() == 7);
static_assert(familyKey.withCallerFrame(CallerFrame::StubFrame).callerFrame() == CallerFrame::StubFrame);
static_assert(familyKey.withOperandRegister(GPRInfo::regT1).operandRegister() == GPRInfo::regT1);
static_assert(familyKey.withImmediate(UINT32_MAX).immediate() == UINT32_MAX);
static_assert(familyKey.withFamily(StubFamily::PutById).family() == StubFamily::PutById);

static_assert(familyKey.withImmediate(0) != familyKey && familyKey.withImmediate(0).immediate() == 0u);
static_assert(familyKey.withImmediate(UINT32_MAX).withoutImmediate() == familyKey);
static_assert(familyKey.withOperandRegister(GPRInfo::regT1).withoutOperandRegister() == familyKey);

static constexpr StubKey keyWithEveryAxis = familyKey.withForm(5).withCount(7).withCallerFrame(CallerFrame::Frameless)
    .withOperandRegister(GPRInfo::regT1).withImmediate(UINT32_MAX);

static_assert(keyWithEveryAxis.family() == StubFamily::GetById && keyWithEveryAxis.form() == 5 && keyWithEveryAxis.count() == 7);
static_assert(keyWithEveryAxis.callerFrame() == CallerFrame::Frameless);
static_assert(keyWithEveryAxis.operandRegister() == GPRInfo::regT1 && keyWithEveryAxis.immediate() == UINT32_MAX);
static_assert(StubKey::fromBits(keyWithEveryAxis.bits()) == keyWithEveryAxis);

static_assert(bodyOf(StubKey(StubFamily::ValueEqualsLiteral)) == StubKey(StubFamily::StringEqualsLiteral));
static_assert(bodyOf(StubKey(StubFamily::EnterSlimClosure)) == StubKey(StubFamily::EnterClosure));
static_assert(exists(callValue(numberOfArgumentGPRs)) && !exists(StubKey(StubFamily::CallValue).withCount(numberOfArgumentGPRs + 1)));
static_assert(!exists(StubKey(StubFamily::ArrayLiteralWithDouble)));
static_assert(exists(StubKey(StubFamily::PollAtLoop)) == isX86_64());
static_assert(exists(pollAtLoop(CallerFrame::Frameless)) == isX86_64() && exists(throwAtSite(ThrowSiteSource::FaultSite, CallerFrame::Frameless)));
static_assert(!exists(StubKey(StubFamily::StringsAreEqual).withCallerFrame(CallerFrame::Frameless)) && !exists(familyKey.withCallerFrame(CallerFrame::Frameless)));
static_assert(exists(familyKey.withCallerFrame(CallerFrame::StubFrame)) && !exists(StubKey(StubFamily::StringsAreEqual).withCallerFrame(CallerFrame::StubFrame)));
static_assert(!exists(StubKey(StubFamily::ToBoolean).withImmediate(1)));

static_assert(!ClosureWord::canEncode({ 0, 0 }, 0) && ClosureWord::canEncode({ ClosureWord::offsetUnit, 0 }, 0));
static_assert(ClosureWord::canEncode({ ClosureWord::maxSizeOfCode - ClosureWord::offsetUnit, 0 }, 0) && !ClosureWord::canEncode({ ClosureWord::maxSizeOfCode, 0 }, 0));
static_assert(ClosureWord::canEncode({ ClosureWord::offsetUnit, 0 }, ClosureWord::firstReservedFunction - 1) && !ClosureWord::canEncode({ ClosureWord::offsetUnit, 0 }, ClosureWord::firstReservedFunction));
static_assert(ClosureWord::canEncode({ ClosureWord::offsetUnit, ClosureWord::maxNumberOfParameters }, 0) && !ClosureWord::canEncode({ ClosureWord::offsetUnit, ClosureWord::maxNumberOfParameters + 1 }, 0));
static_assert(ClosureWord::canEncode({ 2, 0 }, 0) == !isARM64());

static constexpr unsigned maxClosureExecutable = (1u << (ClosureRequest::isSlimBit - ClosureRequest::executableShift)) - 1;
static constexpr uint32_t maxClosureAllocation = (1u << (ClosureRequest::countShift - ClosureRequest::allocationShift)) - 1;

static_assert(ClosureRequest::canEncode(ClosureWord::functionMask, 0, 0) && !ClosureRequest::canEncode(ClosureWord::functionMask + 1, 0, 0));
static_assert(ClosureRequest::canEncode(0, maxClosureExecutable, 0) && !ClosureRequest::canEncode(0, maxClosureExecutable + 1, 0));
static_assert(ClosureRequest::canEncode(0, 0, maxClosureAllocation) && !ClosureRequest::canEncode(0, 0, maxClosureAllocation + 1));
static_assert(ResumeClosureRequest::canEncode(0, ResumeClosureRequest::maxExecutable, 0, 0) && !ResumeClosureRequest::canEncode(0, ResumeClosureRequest::maxExecutable + 1, 0, 0));
static_assert(ResumeClosureRequest::canEncode(0, 0, 0, ResumeClosureRequest::maxFrameWord) && !ResumeClosureRequest::canEncode(0, 0, 0, ResumeClosureRequest::maxFrameWord + 1));

static_assert(!static_cast<unsigned>(StubFrameKind::Plain) && numberOfStubFrameKinds == 14);
static_assert(hasSingleReturnSite(StubFrameKind::Adapter) && hasSingleReturnSite(StubFrameKind::ConstructAdapter));
static_assert(hasSingleReturnSite(StubFrameKind::SavedFrameAdapter) && hasSingleReturnSite(StubFrameKind::AsyncGeneratorBody) && hasSingleReturnSite(StubFrameKind::ArgumentList));
static_assert(!hasSingleReturnSite(StubFrameKind::Plain) && !hasSingleReturnSite(StubFrameKind::EngineCallee) && !hasSingleReturnSite(StubFrameKind::HostCall));
static_assert(!hasSingleReturnSite(StubFrameKind::GeneratorBody) && !hasSingleReturnSite(StubFrameKind::FirstOfTwoReads));
static_assert(!hasSingleReturnSite(StubFrameKind::BelowSpilledArguments) && !hasSingleReturnSite(StubFrameKind::AtStubEntry) && !hasSingleReturnSite(StubFrameKind::InHostCallFrame));
static_assert(bytesAboveReturnAddressOf(StubFrameKind::EngineCallee) == sizeOfSpilledArgumentRegisters && bytesAboveReturnAddressOf(StubFrameKind::BelowSpilledArguments) == sizeOfSpilledArgumentRegisters);
static_assert(!bytesAboveReturnAddressOf(StubFrameKind::Plain) && !bytesAboveReturnAddressOf(StubFrameKind::Adapter) && !bytesAboveReturnAddressOf(StubFrameKind::AtStubEntry));

static std::optional<StubKey> keyIfExists(StubKey key)
{
    if (exists(key))
        return key;
    return std::nullopt;
}

static void testLegalKeys()
{
    Vector<StubKey> keys = allStubKeys();
    WTF::BitSet<numberOfStubFamilies> familiesWithKey;
    for (size_t i = 0; i < keys.size(); ++i) {
        StubKey key = keys[i];
        AOT_TEST_CHECK(exists(key));
        AOT_TEST_CHECK(!key.immediate());
        AOT_TEST_CHECK(StubKey::fromBits(key.bits()) == key);
        familiesWithKey.set(static_cast<size_t>(key.family()));
        if (!i)
            continue;
        StubKey keyBefore = keys[i - 1];
        AOT_TEST_CHECK(keyBefore < key);
        AOT_TEST_CHECK(keyBefore.bits() < key.bits());
        AOT_TEST_CHECK(keyBefore.family() <= key.family());
    }
    for (const StubDescriptor& descriptor : StubFamilyTokens::descriptors)
        AOT_TEST_CHECK(familiesWithKey.get(static_cast<size_t>(descriptor.family)) == descriptor.isas.contains(currentStubISA));
}

static void checkKeysOutsideBounds(StubKey key, const StubDescriptor& descriptor)
{
    if (descriptor.numberOfForms < StubKey::maxNumberOfForms)
        AOT_TEST_CHECK(!exists(key.withForm(descriptor.numberOfForms)));
    if (descriptor.maxCount < StubKey::maxCount)
        AOT_TEST_CHECK(!exists(key.withCount(descriptor.maxCount + 1u)));
    if (descriptor.minCount)
        AOT_TEST_CHECK(!exists(key.withCount(descriptor.minCount - 1u)));
    AOT_TEST_CHECK(!exists(key.withOperandRegister(instanceGPR)));

    bool takesImmediate = descriptor.axes.contains(StubAxis::Immediate) && !key.operandRegister();
    AOT_TEST_CHECK(exists(key.withImmediate(1)) == takesImmediate);
}

static void checkVariantAxes(StubKey key, const StubDescriptor& descriptor)
{
    if (key.callerFrame() == CallerFrame::HasFrame) {
        bool hasFramelessVariant = descriptor.axes.contains(StubAxis::CallerFrame) && descriptor.framelessISAs.contains(currentStubISA);
        AOT_TEST_CHECK(exists(key.withCallerFrame(CallerFrame::Frameless)) == hasFramelessVariant);

        bool hasStubFrameVariant = descriptor.axes.contains(StubAxis::StubFrame) && !key.operandRegister();
        AOT_TEST_CHECK(exists(key.withCallerFrame(CallerFrame::StubFrame)) == hasStubFrameVariant);
    }
    if (key.callerFrame() == CallerFrame::Frameless) {
        AOT_TEST_CHECK(key.family() == StubFamily::ThrowAtFault || key.family() == StubFamily::PollAtLoop);
        AOT_TEST_CHECK(descriptor.linkage == StubLinkage::EnteredByRuntime);
    }
}

static void testIllegalKeys()
{
    for (StubKey key : allStubKeys()) {
        const StubDescriptor& descriptor = descriptorOf(key.family());
        checkKeysOutsideBounds(key, descriptor);
        checkVariantAxes(key, descriptor);
    }
}

static void testVariantLookup()
{
    for (StubKey key : allStubKeys()) {
        const StubDescriptor& descriptor = descriptorOf(key.family());
        for (CallerFrame callerFrame : { CallerFrame::HasFrame, CallerFrame::Frameless, CallerFrame::StubFrame })
            AOT_TEST_CHECK(tryWithCallerFrame(key, callerFrame) == keyIfExists(key.withCallerFrame(callerFrame)));

        AOT_TEST_CHECK(!tryWithOperandRegister(key, instanceGPR));
        for (GPRReg reg : operandRegistersOf(descriptor.operandRegisters))
            AOT_TEST_CHECK(tryWithOperandRegister(key, reg) == keyIfExists(key.withOperandRegister(reg)));
    }
}

static void testBodyEntries()
{
    for (StubKey key : allStubKeys()) {
        StubKey body = bodyOf(key);
        AOT_TEST_CHECK(bodyOf(body) == body);
        AOT_TEST_CHECK(generatorOf(body.family()).generate);
        AOT_TEST_CHECK(generatorOf(key.family()).signature);

        unsigned occurrences = 0;
        for (StubKey entry : entriesOf(body)) {
            AOT_TEST_CHECK(exists(entry));
            AOT_TEST_CHECK(bodyOf(entry) == body);
            occurrences += entry == key;
        }
        AOT_TEST_CHECK(occurrences == 1);
    }
}

static String dumpedNameOf(StubKey key)
{
    StringPrintStream stream;
    dump(stream, key);
    return stream.toString();
}

static void testDumpedNames()
{
    HashSet<String> names;
    for (StubKey key : allStubKeys()) {
        const StubDescriptor& descriptor = descriptorOf(key.family());
        String name = dumpedNameOf(key);
        AOT_TEST_CHECK(!name.isEmpty());
        AOT_TEST_CHECK(names.add(name).isNewEntry);
        if (descriptor.axes.contains(StubAxis::Immediate))
            AOT_TEST_CHECK(dumpedNameOf(key.withImmediate(17)) != name);
        AOT_TEST_CHECK(descriptor.formNames.size() == (descriptor.axes.contains(StubAxis::Form) ? descriptor.numberOfForms : 0u));
    }

    AOT_TEST_CHECK(dumpedNameOf(StubKey()) == "<no stub>"_s);
    AOT_TEST_CHECK(dumpedNameOf(arrayCopy()) == "ArraySlice.Whole"_s);
    AOT_TEST_CHECK(dumpedNameOf(equal(true)) == "Equal.Strict"_s);
    AOT_TEST_CHECK(dumpedNameOf(putById(PutMode::Define)) == "PutById.Define"_s);
    AOT_TEST_CHECK(dumpedNameOf(putById(PutMode::Define).withImmediate(17)) == "PutById.Define#17"_s);
    AOT_TEST_CHECK(dumpedNameOf(callValue(2)) == "CallValue.n2"_s);
    AOT_TEST_CHECK(dumpedNameOf(getById().withCallerFrame(CallerFrame::StubFrame)) == "GetById.StubFrame"_s);
    AOT_TEST_CHECK(dumpedNameOf(throwAtSite(ThrowSiteSource::FaultSite, CallerFrame::Frameless)) == "ThrowAtFault.Frameless"_s);
    AOT_TEST_CHECK(dumpedNameOf(StubKey(StubFamily::ArraySlice).withForm(4)) == "ArraySlice.<no form 4>"_s);
}

static void testPackedWords()
{
    constexpr uint64_t offsetInCode = 64;
    constexpr uintptr_t code = 4096;
    uint64_t closureWord = ClosureWord::encode({ offsetInCode, 3 }, 7, { });
    AOT_TEST_CHECK(ClosureWord::functionOf(closureWord) == 7);
    AOT_TEST_CHECK(ClosureWord::addressOf(closureWord, code) == code + offsetInCode);
    AOT_TEST_CHECK(ClosureWord::numberOfParametersOf(closureWord) == 3);
    AOT_TEST_CHECK(ClosureWord::isCallable(closureWord) && !ClosureWord::takesNewTarget(closureWord));

    uint64_t classWord = ClosureWord::encode({ offsetInCode, ClosureWord::maxNumberOfParameters }, ClosureWord::firstReservedFunction - 1, { .isNotCallable = true, .takesNewTarget = true });
    AOT_TEST_CHECK(ClosureWord::functionOf(classWord) == ClosureWord::firstReservedFunction - 1);
    AOT_TEST_CHECK(ClosureWord::addressOf(classWord, code) == code + offsetInCode);
    AOT_TEST_CHECK(ClosureWord::numberOfParametersOf(classWord) == ClosureWord::maxNumberOfParameters);
    AOT_TEST_CHECK(!ClosureWord::isCallable(classWord) && ClosureWord::takesNewTarget(classWord));

    uint64_t request = ClosureRequest::encode(5, 2, 9);
    AOT_TEST_CHECK(ClosureRequest::functionOf(request) == 5);
    AOT_TEST_CHECK(ClosureRequest::executableOf(request) == 2);
    AOT_TEST_CHECK(ClosureRequest::allocationOf(request) == 9);

    uint64_t resumeRequest = ResumeClosureRequest::encode(5, 1, 9, 3);
    AOT_TEST_CHECK(ResumeClosureRequest::executableOf(resumeRequest) == 1);
    AOT_TEST_CHECK(ResumeClosureRequest::frameWordOf(resumeRequest) == 3);
}

template<typename Words>
static const void* wordAt(const Words& words, ptrdiff_t offset)
{
    const void* word;
    memcpySpan(asMutableByteSpan(word), asByteSpan(words).subspan(offset, sizeof(word)));
    return word;
}

static void testStubWords()
{
    StubWords words { };
    uintptr_t nextWord = 1;
    auto takeWord = [&] { return std::bit_cast<const void*>(nextWord++); };
    for (const void*& address : words.returnAddresses)
        address = takeWord();
    BoxedCallEntry entry { };
    entry.nativeFunction = takeWord();
    for (const void*& target : entry.targets)
        target = takeWord();

    for (unsigned kind = 0; kind < numberOfStubFrameKinds; ++kind) {
        StubFrameKind frameKind = static_cast<StubFrameKind>(kind);
        AOT_TEST_CHECK(words.returnAddress(frameKind) == words.returnAddresses[kind]);
        AOT_TEST_CHECK(wordAt(words, StubWords::offsetOfReturnAddress(frameKind)) == words.returnAddresses[kind]);
    }
    AOT_TEST_CHECK(wordAt(entry, BoxedCallEntry::offsetOfNativeFunction()) == entry.nativeFunction);
    for (unsigned count = BoxedCallEntry::minArgumentCount; count <= BoxedCallEntry::maxArgumentCount; ++count)
        AOT_TEST_CHECK(wordAt(entry, BoxedCallEntry::offsetOfTarget(count)) == entry.targets[count - BoxedCallEntry::minArgumentCount]);

    constexpr uintptr_t functionAlignment = 1u << StubWords::boxedCallHashShift;
    AOT_TEST_CHECK(!StubWords::indexOfBoxedCall(0) && StubWords::indexOfBoxedCall(functionAlignment) == 1);
    AOT_TEST_CHECK(StubWords::indexOfBoxedCall(functionAlignment * (StubWords::numberOfBoxedCallEntries - 1)) == StubWords::numberOfBoxedCallEntries - 1);
    AOT_TEST_CHECK(!StubWords::indexOfBoxedCall(functionAlignment * StubWords::numberOfBoxedCallEntries));
}

static void testAdapterFrame()
{
    RegisterSet savedRegisters;
    Vector<ptrdiff_t> offsets;
    for (const AdapterFrame::SavedRegister& saved : AdapterFrame::savedRegisters) {
        AOT_TEST_CHECK(!containsRegister(savedRegisters, saved.reg));
        savedRegisters.add(saved.reg, IgnoreVectors);
        offsets.append(saved.offset);
    }
    offsets.append(AdapterFrame::offsetOfInstance);
    offsets.append(AdapterFrame::offsetOfNewTarget);

    for (size_t i = 0; i < offsets.size(); ++i) {
        AOT_TEST_CHECK(offsets[i] < 0);
        AOT_TEST_CHECK(!(offsets[i] % 8));
        AOT_TEST_CHECK(offsets[i] >= -static_cast<ptrdiff_t>(AdapterFrame::saveAreaSize));
        for (size_t j = 0; j < i; ++j)
            AOT_TEST_CHECK(offsets[i] != offsets[j]);
    }
}

static void checkFrontEnds()
{
    WTF::BitSet<numberOfEntries> operations;
    WTF::BitSet<numberOfEntries> rawOperations;
    Vector<StubKey> stubs;
    for (const OperationFrontEnd& frontEnd : operationFrontEnds()) {
        AOT_TEST_CHECK(exists(frontEnd.stub));
        AOT_TEST_CHECK(bodyOf(frontEnd.stub) == frontEnd.stub);
        AOT_TEST_CHECK(descriptorOf(frontEnd.stub.family()).linkage == StubLinkage::FrontEnd);
        AOT_TEST_CHECK(!operations.testAndSet(indexOf(frontEnd.operation)));
        AOT_TEST_CHECK(!rawOperations.testAndSet(indexOf(frontEnd.raw)));
        stubs.append(frontEnd.stub);
    }
    AOT_TEST_CHECK(stubs.size() == 10);
    operations.filter(rawOperations);
    AOT_TEST_CHECK(operations.isEmpty());
    std::sort(stubs.begin(), stubs.end());
    AOT_TEST_CHECK(stubs == stubKeysWithLinkage(StubLinkage::FrontEnd));
}

static void checkPreludes()
{
    WTF::BitSet<numberOfEntries> operations;
    for (const OperationPrelude& prelude : operationPreludes()) {
        AOT_TEST_CHECK(prelude.emit);
        AOT_TEST_CHECK(!operations.testAndSet(indexOf(prelude.operation)));
    }
}

static void testFrontEndsAndPreludes()
{
    checkFrontEnds();
    checkPreludes();
}

static const StubDescriptor& checkedHandlerDescriptor(StubKey handler)
{
    AOT_TEST_CHECK(exists(handler));
    const StubDescriptor& descriptor = descriptorOf(handler.family());
    AOT_TEST_CHECK(descriptor.linkage == StubLinkage::Handler);
    return descriptor;
}

static void checkSlotExit(SlotHandlerTable table, SlotExit exit)
{
    StubKey key = slotExit(table, exit);
    AOT_TEST_CHECK(exists(key));
    AOT_TEST_CHECK(key.family() == (isReadTable(table) ? StubFamily::ReadSlotExit : StubFamily::StoreSlotExit));
    AOT_TEST_CHECK(descriptorOf(key.family()).linkage == StubLinkage::InternalEntry);
    AOT_TEST_CHECK(key.callerFrame() == callerFrameOf(traitsOf(table).origin));
}

static void testHandlerTables()
{
    Vector<StubKey> allHandlers;
    for (SlotHandlerTable table : allSlotHandlerTables) {
        auto handlers = slotHandlerKeys(table);
        for (Slot::Kind kind : slotKindsWithHandlers) {
            StubKey handler = handlers[static_cast<unsigned>(kind)];
            const StubDescriptor& descriptor = checkedHandlerDescriptor(handler);
            AOT_TEST_CHECK(descriptor.family == (isReadTable(table) ? StubFamily::ReadSlot : StubFamily::StoreSlot));
            AOT_TEST_CHECK(handler == slotHandlerFor(table, kind) && slotKindOf(handler) == kind && handlerTableOf(handler) == table);
            AOT_TEST_CHECK(handler.callerFrame() == callerFrameOf(traitsOf(table).origin));
            allHandlers.append(handler);
        }
        checkSlotExit(table, SlotExit::Miss);
        checkSlotExit(table, SlotExit::Uncached);
        AOT_TEST_CHECK(slotExit(table, SlotExit::Miss) != slotExit(table, SlotExit::Uncached));
    }
    std::sort(allHandlers.begin(), allHandlers.end());
    AOT_TEST_CHECK(std::adjacent_find(allHandlers.begin(), allHandlers.end()) == allHandlers.end());

    for (PutMode mode : { PutMode::Strict, PutMode::Sloppy, PutMode::Define }) {
        SlotHandlerTable table = slotHandlerTable(operationFor(mode), SlotOrigin::ById, SlotForm::Slot);
        AOT_TEST_CHECK(!isReadTable(table) && accessOf(traitsOf(table).operation) == (mode == PutMode::Define ? SlotAccess::Define : SlotAccess::Store));
    }
}

static void testResolvedValueRegister()
{
    for (unsigned count = 0; count <= numberOfArgumentGPRs; ++count)
        AOT_TEST_CHECK(signatureFor(resolveCallValue(count)).firstResult == Reg(gprOf(StubRegister::T11)));
}

static bool hasOperand(const StubSignature& signature, Reg reg)
{
    for (unsigned i = 0; i < signature.numberOfOperands; ++i) {
        if (signature.operands[i] == reg)
            return true;
    }
    return false;
}

static void checkOperands(const StubSignature& signature, const RegisterSet& pinned)
{
    AOT_TEST_CHECK(signature.numberOfOperands <= signature.operands.size());
    RegisterSet operands;
    for (unsigned i = 0; i < signature.operands.size(); ++i) {
        Reg operand = signature.operands[i];
        if (i >= signature.numberOfOperands) {
            AOT_TEST_CHECK(!operand.isSet());
            continue;
        }
        AOT_TEST_CHECK(operand.isSet());
        AOT_TEST_CHECK(!containsRegister(operands, operand));
        AOT_TEST_CHECK(!containsRegister(pinned, operand));
        operands.add(operand, IgnoreVectors);
    }
}

static void checkResults(const StubSignature& signature, const RegisterSet& pinned)
{
    Reg first = signature.firstResult;
    Reg second = signature.secondResult;
    switch (signature.result) {
    case StubResult::Flags:
        AOT_TEST_CHECK(signature.trueCondition == MacroAssembler::Zero || signature.trueCondition == MacroAssembler::NonZero);
        [[fallthrough]];
    case StubResult::None:
        AOT_TEST_CHECK(!first.isSet() && !second.isSet());
        break;
    case StubResult::GPR:
        AOT_TEST_CHECK(first.isGPR() && !second.isSet());
        break;
    case StubResult::FPR:
        AOT_TEST_CHECK(first.isFPR() && !second.isSet());
        break;
    case StubResult::TwoGPRs:
        AOT_TEST_CHECK(first.isGPR() && second.isGPR() && first != second);
        break;
    }
    for (Reg result : { first, second })
        AOT_TEST_CHECK(!result.isSet() || !containsRegister(pinned, result));
}

static void checkImmediateRegister(const StubSignature& signature, const StubDescriptor& descriptor, const RegisterSet& pinned)
{
    if (descriptor.axes.contains(StubAxis::Immediate))
        AOT_TEST_CHECK(signature.immediate.has_value());
    // A family may take its immediate in the register always: CallDirectFunction binds none.
    AOT_TEST_CHECK(signature.boundImmediates == StubBoundImmediates::None || descriptor.binding == ImmediateBinding::ByPolicy);
    if (!signature.immediate)
        return;
    AOT_TEST_CHECK(!containsRegister(pinned, *signature.immediate));
    AOT_TEST_CHECK(!hasOperand(signature, *signature.immediate));
}

static void checkChosenOperandRegister(StubKey key, const StubSignature& signature, OperandRegisterChoice choice)
{
    if (!key.operandRegister())
        return;
    Reg chosen = *key.operandRegister();
    AOT_TEST_CHECK(choice != OperandRegisterChoice::NoChoice);
    switch (choice) {
    case OperandRegisterChoice::NoChoice:
        break;
    case OperandRegisterChoice::AlternateReceivers:
        AOT_TEST_CHECK(hasOperand(signature, chosen));
        break;
    }
}

static void testSignatures()
{
    RegisterSet pinned = pinnedRegisters();
    for (StubKey key : allStubKeys()) {
        const StubDescriptor& descriptor = descriptorOf(key.family());
        StubSignature signature = signatureFor(key);
        checkOperands(signature, pinned);
        checkResults(signature, pinned);
        checkImmediateRegister(signature, descriptor, pinned);
        checkChosenOperandRegister(key, signature, descriptor.operandRegisters);
    }
}

namespace {

struct AssembledCalls {
    std::span<const uint8_t> bytesBefore(size_t relocation) const
    {
        uint32_t begin = labelOffsets[relocation];
        uint32_t end = body.relocations[relocation].offset;
        AOT_TEST_CHECK(begin <= end && end <= body.bytes.size());
        return body.bytes.subspan(begin, end - begin);
    }

    CodeBody body;
    Vector<uint32_t> labelOffsets;
};

} // anonymous namespace

static constexpr uint32_t nameWithOwnStub = 5;
static constexpr uint32_t nameWithoutOwnStub = 6;

static StubPolicy createPolicyWithOwnStubForReads()
{
    StubPolicy policy;
    policy.namesWithOwnStubByAccess[static_cast<size_t>(Typed::Access::Read)].set(nameWithOwnStub);
    return policy;
}

static StubOptions createValidatingOptions()
{
    StubOptions options;
    options.validates = true;
    return options;
}

template<typename Emit>
static Vector<uint8_t> assemble(const Emit& emit)
{
    CCallHelpers jit;
    RelocationRecorder relocations(jit, AssemblerPolicy::forImage(StubOptions { }.targetFeatures));
    emit(jit);
    std::expected<RelocatableCode, LinkFailure> code = relocations.finalize(LinkBuffer::Profile::Thunk, nullptr);
    AOT_TEST_CHECK(code.has_value() && code->relocations.isEmpty());
    return WTF::move(code->bytes);
}

// Under validation a call whose preserved registers are checked is followed by a call of this stub.
static constexpr StubKey reportOfChangedRegister = tailCallOperation(OperationCall::FirstArgument::Instance).withImmediate(offsetInRuntimeTable(Entry::operationAOTReportValidationFailure));

static bool isCallOfReport(const CodeBody& body, const Relocation& relocation)
{
    CodeSymbol symbol = relocation.codeSymbol();
    return relocation.kind == RelocationKind::Call && symbol.space == SymbolSpace::Stub && body.stubKeys[symbol.index] == reportOfChangedRegister;
}

template<typename Emit>
static AssembledCalls assembleCalls(const StubCatalog& catalog, const Emit& emit)
{
    CCallHelpers jit;
    RelocationRecorder relocations(jit, AssemblerPolicy::forImage(catalog.options().targetFeatures));
    CodeUnitRecorder units;
    StubCalls calls(catalog, relocations, units);
    WTF::Vector<CCallHelpers::Label> labels;
    emit(jit, calls, [&] { labels.append(jit.label()); });

    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::Thunk);
    AOT_TEST_CHECK(!buffer.didFailToAllocate());
    buffer.finalize(false, "calls of stubs"_s);
    std::expected<Vector<Relocation>, LinkFailure> linked = relocations.link(buffer, nullptr);
    AOT_TEST_CHECK(linked.has_value());

    AssembledCalls result;
    result.body.relocations = WTF::move(*linked);
    result.body.bytes.append(buffer.bytes());
    calls.describeCalls(result.body.relocations.span(), result.body);
    for (CCallHelpers::Label label : labels)
        result.labelOffsets.append(buffer.linkBuffer().offsetOf(label));
    size_t numberOfReports = std::ranges::count_if(result.body.relocations, [&](const Relocation& relocation) {
        return isCallOfReport(result.body, relocation);
    });
    AOT_TEST_CHECK(catalog.options().validates || !numberOfReports);
    AOT_TEST_CHECK(result.labelOffsets.size() + numberOfReports == result.body.relocations.size());
    return result;
}

static void checkRelocation(const Relocation& relocation, RelocationKind kind, CodeSymbol symbol)
{
    AOT_TEST_CHECK(relocation.kind == kind && relocation.codeSymbol() == symbol);
}

static void checkReturnSite(const AssembledCalls& assembled, size_t returnSite, size_t relocation, OptionalIndex site)
{
    AOT_TEST_CHECK(assembled.body.returnSites[returnSite].offset == assembled.body.relocations[relocation].offset + sizeOfNearCall);
    AOT_TEST_CHECK(assembled.body.returnSites[returnSite].site == site);
}

static void testStubCallsWithImmediates()
{
    StubCatalog catalog(StubOptions { }, createPolicyWithOwnStubForReads());
    constexpr StubKey key = callMethodById(1);
    AssembledCalls assembled = assembleCalls(catalog, [&](CCallHelpers& jit, StubCalls& calls, const auto& markCall) {
        markCall();
        calls.call(jit, key, nameWithOwnStub, OptionalIndex { 1 });
        markCall();
        calls.call(jit, key, nameWithoutOwnStub, OptionalIndex { 2 });
        markCall();
        calls.tailCall(jit, toBoolean());
    });
    const CodeBody& body = assembled.body;

    AOT_TEST_CHECK(body.stubKeys == (Vector<StubKey> { key.withImmediate(nameWithOwnStub), key, toBoolean() }));
    AOT_TEST_CHECK(body.relocations.size() == 3);
    checkRelocation(body.relocations[0], RelocationKind::Call, { SymbolSpace::Stub, 0 });
    checkRelocation(body.relocations[1], RelocationKind::Call, { SymbolSpace::Stub, 1 });
    checkRelocation(body.relocations[2], RelocationKind::Jump, { SymbolSpace::Stub, 2 });

    Vector<uint8_t> immediateMove = assemble([&](CCallHelpers& jit) {
        jit.move(CCallHelpers::TrustedImm32(nameWithoutOwnStub), *catalog.summaryOf(key).signature.immediate);
    });
    AOT_TEST_CHECK(assembled.bytesBefore(0).empty());
    AOT_TEST_CHECK(equalSpans(assembled.bytesBefore(1), immediateMove.span()));
    AOT_TEST_CHECK(assembled.bytesBefore(2).empty());

    AOT_TEST_CHECK(body.returnSites.size() == 2);
    checkReturnSite(assembled, 0, 0, OptionalIndex { 1 });
    checkReturnSite(assembled, 1, 1, OptionalIndex { 2 });
    AOT_TEST_CHECK(body.firstReadSites.isEmpty() && body.argumentMoves.isEmpty() && body.callees.isEmpty());
}

static void testStubCallsOfOperations()
{
    StubCatalog catalog(StubOptions { }, StubPolicy { });
    constexpr StubKey key = callOperation(OperationCall::Returns::Value, OperationCall::FirstArgument::Instance);
    constexpr uint32_t firstOffset = offsetInRuntimeTable(Entry::operationAOTValueAdd);
    constexpr uint32_t secondOffset = offsetInRuntimeTable(Entry::operationAOTGetById);
    AssembledCalls assembled = assembleCalls(catalog, [&](CCallHelpers& jit, StubCalls& calls, const auto& markCall) {
        markCall();
        calls.call(jit, key, firstOffset, { });
        markCall();
        calls.tailCall(jit, tailCallOperation(OperationCall::FirstArgument::Instance), secondOffset);
    });
    AOT_TEST_CHECK(assembled.body.stubKeys == (Vector<StubKey> { key.withImmediate(firstOffset), tailCallOperation(OperationCall::FirstArgument::Instance).withImmediate(secondOffset) }));
    AOT_TEST_CHECK(assembled.bytesBefore(0).empty() && assembled.bytesBefore(1).empty());
    AOT_TEST_CHECK(assembled.body.returnSites.size() == 1);
    checkReturnSite(assembled, 0, 0, { });
}

static void testStubCallsOfOneKey()
{
    StubCatalog catalog(StubOptions { }, StubPolicy { });
    uint32_t firstIndex = UINT32_MAX;
    uint32_t secondIndex = UINT32_MAX;
    AssembledCalls assembled = assembleCalls(catalog, [&](CCallHelpers& jit, StubCalls& calls, const auto& markCall) {
        markCall();
        firstIndex = calls.call(jit, toBoolean(), OptionalIndex { 7 });
        markCall();
        calls.call(jit, valueIsTruthy(), { });
        markCall();
        secondIndex = calls.call(jit, toBoolean(), OptionalIndex { 8 });
        markCall();
        calls.tailCall(jit, toBoolean());
    });
    const CodeBody& body = assembled.body;

    AOT_TEST_CHECK(body.stubKeys == (Vector<StubKey> { toBoolean(), valueIsTruthy() }));
    checkRelocation(body.relocations[0], RelocationKind::Call, { SymbolSpace::Stub, 0 });
    checkRelocation(body.relocations[1], RelocationKind::Call, { SymbolSpace::Stub, 1 });
    checkRelocation(body.relocations[2], RelocationKind::Call, { SymbolSpace::Stub, 0 });
    checkRelocation(body.relocations[3], RelocationKind::Jump, { SymbolSpace::Stub, 0 });
    AOT_TEST_CHECK(body.returnSites.size() == 3 && !firstIndex && secondIndex == 2);
    checkReturnSite(assembled, firstIndex, 0, OptionalIndex { 7 });
    checkReturnSite(assembled, 1, 1, { });
    checkReturnSite(assembled, secondIndex, 2, OptionalIndex { 8 });
}

static void testStubCallForTwoReads()
{
    StubCatalog catalog(StubOptions { }, StubPolicy { });
    AssembledCalls assembled = assembleCalls(catalog, [&](CCallHelpers& jit, StubCalls& calls, const auto& markCall) {
        markCall();
        calls.call(jit, getByIdChain(), OptionalIndex { 2 });
        markCall();
        calls.callForTwoReads(jit, getByIdChain(), OptionalIndex { 3 }, OptionalIndex { 4 });
    });
    const CodeBody& body = assembled.body;

    AOT_TEST_CHECK(body.stubKeys == Vector<StubKey> { getByIdChain() });
    AOT_TEST_CHECK(body.returnSites.size() == 2 && body.firstReadSites.size() == 1);
    checkReturnSite(assembled, 0, 0, OptionalIndex { 2 });
    checkReturnSite(assembled, 1, 1, OptionalIndex { 4 });
    AOT_TEST_CHECK(body.firstReadSites[0].returnOffset == body.returnSites[1].offset && body.firstReadSites[0].site == 3);
}

static void testStubCallsThroughArgumentThunks()
{
    StubCatalog catalog(StubOptions { }, createPolicyWithOwnStubForReads());
    constexpr StubKey key = callMethodById(1);
    ArgumentMoves moves;
    appendArgumentMove(moves, argumentGPR(0), ArgumentSource::Constant, 42);

    AssembledCalls assembled = assembleCalls(catalog, [&](CCallHelpers& jit, StubCalls& calls, const auto& markCall) {
        markCall();
        calls.call(jit, key, nameWithOwnStub, OptionalIndex { 1 }, moves);
        markCall();
        calls.call(jit, key, nameWithoutOwnStub, OptionalIndex { 2 }, moves);
        markCall();
        calls.call(jit, key, std::nullopt, OptionalIndex { 3 }, moves);
    });
    const CodeBody& body = assembled.body;

    AOT_TEST_CHECK(body.stubKeys.isEmpty() && body.argumentMoves.size() == 3 && body.returnSites.size() == 3);
    const uint64_t expectedHeaders[][2] = {
        { key.withImmediate(nameWithOwnStub).bits(), 0 },
        { key.bits(), 1ull << 32 | nameWithoutOwnStub },
        { key.bits(), 0 },
    };
    for (uint32_t index = 0; index < 3; ++index) {
        checkRelocation(body.relocations[index], RelocationKind::Call, { SymbolSpace::ArgumentThunk, index });
        AOT_TEST_CHECK(assembled.bytesBefore(index).empty());
        checkReturnSite(assembled, index, index, OptionalIndex { index + 1 });
        const ArgumentMoves& words = body.argumentMoves[index];
        AOT_TEST_CHECK(words.size() == 2 + moves.size() && words[0] == expectedHeaders[index][0] && words[1] == expectedHeaders[index][1]);
        AOT_TEST_CHECK(equalSpans(words.span().subspan(2), moves.span()));
    }
}

static void testCallsOfFunctions()
{
    StubCatalog catalog(createValidatingOptions(), StubPolicy { });
    constexpr uint16_t secondaryEntry = 3;
    FunctionID first { 11u };
    FunctionID second { 4u };
    AssembledCalls assembled = assembleCalls(catalog, [&](CCallHelpers& jit, StubCalls& calls, const auto& markCall) {
        markCall();
        calls.callFunction(jit, first, primaryEntry, OptionalIndex { 1 });
        markCall();
        calls.callFunction(jit, second, secondaryEntry, { });
        markCall();
        calls.jumpToFunction(jit, first, secondaryEntry);
    });
    const CodeBody& body = assembled.body;

    AOT_TEST_CHECK(body.callees == (Vector<FunctionID> { first, second }) && body.stubKeys.isEmpty());
    checkRelocation(body.relocations[0], RelocationKind::Call, { SymbolSpace::Function, 0, primaryEntry });
    checkRelocation(body.relocations[1], RelocationKind::Call, { SymbolSpace::Function, 1, secondaryEntry });
    checkRelocation(body.relocations[2], RelocationKind::Jump, { SymbolSpace::Function, 0, secondaryEntry });
    for (size_t index = 0; index < body.relocations.size(); ++index)
        AOT_TEST_CHECK(assembled.bytesBefore(index).empty());
    AOT_TEST_CHECK(body.returnSites.size() == 2);
    checkReturnSite(assembled, 0, 0, OptionalIndex { 1 });
    checkReturnSite(assembled, 1, 1, { });
}

static size_t sizeOfCall(const StubCatalog& catalog, StubKey key, size_t& bytesBeforeCall)
{
    AssembledCalls assembled = assembleCalls(catalog, [&](CCallHelpers& jit, StubCalls& calls, const auto& markCall) {
        markCall();
        calls.call(jit, key, { });
    });
    bytesBeforeCall = assembled.bytesBefore(0).size();
    std::span<const Relocation> relocations = assembled.body.relocations.span();
    AOT_TEST_CHECK(!isCallOfReport(assembled.body, relocations[0]));
    AOT_TEST_CHECK(relocations.size() == (bytesBeforeCall ? 2 : 1) && (relocations.size() == 1 || isCallOfReport(assembled.body, relocations[1])));
    return assembled.body.bytes.size();
}

static void testCallsWithoutValidation()
{
    StubCatalog catalog(createValidatingOptions(), StubPolicy { });
    AOT_TEST_CHECK(catalog.effectsOf(awaitBySavingFrame()).maySuspend && catalog.effectsOf(suspendGenerator(GeneratorSuspension::Yield)).maySuspend);
    AOT_TEST_CHECK(catalog.summaryOf(callValue(0)).signature.usesCallConvention);

    size_t bytesBeforeCall = 0;
    for (StubKey key : { pollAtPrologue(), awaitBySavingFrame(), suspendGenerator(GeneratorSuspension::Yield), throwAtSite(ThrowSiteSource::WordInRegister, CallerFrame::HasFrame), callValue(0) }) {
        AOT_TEST_CHECK(sizeOfCall(catalog, key, bytesBeforeCall) == sizeOfNearCall);
        AOT_TEST_CHECK(!bytesBeforeCall);
    }

    AOT_TEST_CHECK(!catalog.effectsOf(toBoolean()).maySuspend);
    AOT_TEST_CHECK(sizeOfCall(catalog, toBoolean(), bytesBeforeCall) > sizeOfNearCall);
    AOT_TEST_CHECK(bytesBeforeCall);

    StubCatalog catalogWithoutValidation(StubOptions { }, StubPolicy { });
    AOT_TEST_CHECK(sizeOfCall(catalogWithoutValidation, toBoolean(), bytesBeforeCall) == sizeOfNearCall);
}

#if OS(UNIX)

static StubKey findStubPreserving(const StubCatalog& catalog, GPRReg preserved)
{
    std::optional<StubKey> found;
    for (StubKey body : allStubBodies()) {
        const StubSummary& summary = catalog.summaryOf(body);
        if (entriesOf(body).size() != 1 || body.callerFrame() != CallerFrame::HasFrame || neverReturns(body) || catalog.effectsOf(body).maySuspend)
            continue;
        if (summary.signature.usesCallConvention || summary.signature.immediate || summary.signature.result != StubResult::GPR)
            continue;
        if (containsRegister(summary.clobbered, preserved) || body == pollAtPrologue())
            continue;
        found = body;
        break;
    }
    AOT_TEST_CHECK(found.has_value());
    return *found;
}

template<typename Generator>
static Vector<uint8_t> assembleValidatedCall(const StubCatalog& catalog, StubKey key, const Generator& generator)
{
    StubAssembler stubAssembler(catalog.options(), catalog);
    stubAssembler.generateForTesting(key, generator);
    ImageLinkBuffer stubBuffer(stubAssembler, LinkBuffer::Profile::Thunk);
    AOT_TEST_CHECK(!stubBuffer.didFailToAllocate());
    stubBuffer.finalize(false, "stub under validation"_s);
    AOT_TEST_CHECK(stubAssembler.boundEntries().size() == 1 && !stubAssembler.relocations().numberOfRelocations());
    uint32_t offsetOfEntry = stubBuffer.linkBuffer().offsetOf(stubAssembler.boundEntries()[0].label);

    AssembledCalls caller = assembleCalls(catalog, [&](CCallHelpers& jit, StubCalls& calls, const auto& markCall) {
        jit.emitFunctionPrologue();
        markCall();
        calls.call(jit, key, { });
        jit.emitFunctionEpilogue();
        jit.ret();
    });

    Vector<uint8_t> bytes = WTF::move(caller.body.bytes);
    while (bytes.size() % stackAlignmentBytes())
        bytes.append(0);
    size_t startOfStub = bytes.size();
    bytes.append(stubBuffer.bytes());
    size_t startOfReport = bytes.size();
    bytes.appendVector(assemble([](CCallHelpers& jit) {
        jit.breakpoint();
    }));
    AOT_TEST_CHECK(caller.body.relocations.size() == 2 && isCallOfReport(caller.body, caller.body.relocations[1]));
    AOT_TEST_CHECK(applyRelocation(bytes.mutableSpan(), 0, caller.body.relocations[0], startOfStub + offsetOfEntry));
    AOT_TEST_CHECK(applyRelocation(bytes.mutableSpan(), 0, caller.body.relocations[1], startOfReport));
    return bytes;
}

static void runInMappingOfThisProcess(std::span<const uint8_t> code)
{
    void* memory = mmap(nullptr, code.size(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    AOT_TEST_CHECK(memory != MAP_FAILED);
    memcpySpan(unsafeMakeSpan(static_cast<uint8_t*>(memory), code.size()), code);
    AOT_TEST_CHECK(!mprotect(memory, code.size(), PROT_READ | PROT_EXEC));
    MacroAssembler::cacheFlush(memory, code.size());
    std::bit_cast<void (SYSV_ABI *)()>(tagCodePtr<CFunctionPtrTag>(memory))();
}

static void testValidatedCalls()
{
    WTF::fastDisableScavenger();
    static constexpr GPRReg preserved = gprOf(StubRegister::T12);
    StubCatalog catalog(createValidatingOptions(), StubPolicy { });
    StubKey key = findStubPreserving(catalog, preserved);

    Vector<uint8_t> honestStub = assembleValidatedCall(catalog, key, [](StubAssembler& jit) {
        jit.add64(CCallHelpers::TrustedImm32(1), jit.result());
        jit.returnToCaller();
    });
    Vector<uint8_t> lyingStub = assembleValidatedCall(catalog, key, [](StubAssembler& jit) {
        jit.add64(CCallHelpers::TrustedImm32(1), preserved);
        jit.returnToCaller();
    });
    AOT_TEST_CHECK(runInChild([&] { runInMappingOfThisProcess(honestStub.span()); }) == ChildOutcome::Finished);
    AOT_TEST_CHECK(runInChild([&] { runInMappingOfThisProcess(lyingStub.span()); }) == ChildOutcome::Crashed);
}

#endif // OS(UNIX)

static void testRegistersLiveAcrossReads()
{
    for (const StubOptions& options : stubOptionVariants()) {
        StubCatalog catalog(options, StubPolicy { });
        Vector<StubKey> keys { getByIdAfterFrontProbe(), getByIdHashed(), slotExit(SlotHandlerTable::ReadById, SlotExit::Miss), slotExit(SlotHandlerTable::ReadById, SlotExit::Uncached) };
        for (StubKey handler : slotHandlerKeys(SlotHandlerTable::ReadByIdPacked))
            keys.append(handler);
        for (StubKey key : keys) {
            const RegisterSet& clobbered = catalog.summaryOf(key).clobbered;
            AOT_TEST_CHECK(!containsRegister(clobbered, gprOf(StubRegister::A1)));
            AOT_TEST_CHECK(!containsRegister(clobbered, gprOf(StubRegister::A2)));
        }
    }
}

std::span<const NativeTest> stubTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/keys-are-legal-and-ascending"_s, testLegalKeys },
        { "stubs/illegal-keys-do-not-exist"_s, testIllegalKeys },
        { "stubs/variant-lookup"_s, testVariantLookup },
        { "stubs/entries-of-a-body"_s, testBodyEntries },
        { "stubs/dumped-names-are-distinct"_s, testDumpedNames },
        { "stubs/packed-words"_s, testPackedWords },
        { "stubs/stub-words"_s, testStubWords },
        { "stubs/adapter-frame"_s, testAdapterFrame },
        { "stubs/front-ends-and-preludes"_s, testFrontEndsAndPreludes },
        { "stubs/handler-tables"_s, testHandlerTables },
        { "stubs/resolved-value-register"_s, testResolvedValueRegister },
        { "stubs/signatures-are-well-formed"_s, testSignatures },
        { "stubs/calls-with-immediates"_s, testStubCallsWithImmediates },
        { "stubs/calls-of-operations"_s, testStubCallsOfOperations },
        { "stubs/calls-of-one-key"_s, testStubCallsOfOneKey },
        { "stubs/call-for-two-reads"_s, testStubCallForTwoReads },
        { "stubs/calls-through-argument-thunks"_s, testStubCallsThroughArgumentThunks },
        { "stubs/calls-of-functions"_s, testCallsOfFunctions },
        { "stubs/calls-without-validation"_s, testCallsWithoutValidation },
#if OS(UNIX)
        { "stubs/validated-calls"_s, testValidatedCalls },
#endif
        { "stubs/registers-live-across-reads"_s, testRegistersLiveAcrossReads },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
