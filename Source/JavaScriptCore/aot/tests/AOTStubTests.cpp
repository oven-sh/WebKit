/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCodeRole.h"
#include "AOTCompiledCode.h"
#include "AOTConvention.h"
#include "AOTImageFormat.h"
#include "AOTSlotRepresentation.h"
#include "AOTStubAssembler.h"
#include "AOTStubCalls.h"
#include "AOTStubCatalog.h"
#include "AOTStubFrames.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubRecording.h"
#include "AOTStubRegisters.h"
#include "AOTStubRunner.h"
#include "AOTStubTestSupport.h"
#include "AOTStubWords.h"
#include "AOTStubsCalls.h"
#include "AOTStubsConstants.h"
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

#if OS(UNIX)
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
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

static constexpr StubKey keyWithEveryAxis = familyKey.withForm(5).withCount(7).withCallerFrame(CallerFrame::Frameless).withOperandRegister(GPRInfo::regT1).withImmediate(UINT32_MAX);

static_assert(keyWithEveryAxis.family() == StubFamily::GetById && keyWithEveryAxis.form() == 5 && keyWithEveryAxis.count() == 7);
static_assert(keyWithEveryAxis.callerFrame() == CallerFrame::Frameless);
static_assert(keyWithEveryAxis.operandRegister() == GPRInfo::regT1 && keyWithEveryAxis.immediate() == UINT32_MAX);
static_assert(StubKey::fromBits(keyWithEveryAxis.bits()) == keyWithEveryAxis);

static_assert(bodyOf(StubKey(StubFamily::ValueEqualsLiteral)) == StubKey(StubFamily::StringEqualsLiteral));
static_assert(bodyOf(StubKey(StubFamily::EnterSlimClosure)) == StubKey(StubFamily::EnterClosure));
static_assert(exists(callValue(numberOfArgumentGPRs)) && !exists(StubKey(StubFamily::CallValue).withCount(numberOfArgumentGPRs + 1)));
static_assert(!exists(StubKey(StubFamily::ArrayLiteralWithDouble)));
static_assert(exists(StubKey(StubFamily::PollAtLoop)));
static_assert(exists(pollAtLoop(CallerFrame::Frameless)) && exists(throwAtSite(ThrowSiteSource::FaultSite, CallerFrame::Frameless)));
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

static_assert(!static_cast<unsigned>(StubFrameKind::Plain) && numberOfStubFrameKinds == 20);
static_assert(!hasSingleReturnSite(StubFrameKind::ModuleStart) && !bytesAboveReturnAddressOf(StubFrameKind::ModuleStart) && !(ModuleStartFrame::size % 16) && -ModuleStartFrame::offsetOfModuleToAbandon <= static_cast<ptrdiff_t>(ModuleStartFrame::size));
static_assert(hasSingleReturnSite(StubFrameKind::Adapter) && hasSingleReturnSite(StubFrameKind::ConstructAdapter));
static_assert(hasSingleReturnSite(StubFrameKind::SavedFrameAdapter) && hasSingleReturnSite(StubFrameKind::AsyncGeneratorBody) && hasSingleReturnSite(StubFrameKind::ArgumentList) && hasSingleReturnSite(StubFrameKind::EngineCallee));
static_assert(!hasSingleReturnSite(StubFrameKind::Plain) && !hasSingleReturnSite(StubFrameKind::HostCall));
static_assert(!hasSingleReturnSite(StubFrameKind::GeneratorBody) && !hasSingleReturnSite(StubFrameKind::FirstOfTwoReads) && !hasSingleReturnSite(StubFrameKind::MethodRead));
static_assert(!hasSingleReturnSite(StubFrameKind::BelowSpilledArguments) && !hasSingleReturnSite(StubFrameKind::AtStubEntry) && !hasSingleReturnSite(StubFrameKind::InHostCallFrame) && !hasSingleReturnSite(StubFrameKind::PlainFunctionConstruct) && !hasSingleReturnSite(StubFrameKind::PlainFunctionConstructMakesThis) && !hasSingleReturnSite(StubFrameKind::ConstructAdapterMakesThis) && !hasSingleReturnSite(StubFrameKind::ReturnsAfterOperation));
static_assert(bytesAboveReturnAddressOf(StubFrameKind::EngineCallee) == sizeOfSpilledArgumentRegisters && bytesAboveReturnAddressOf(StubFrameKind::BelowSpilledArguments) == sizeOfSpilledArgumentRegisters && bytesAboveReturnAddressOf(StubFrameKind::PlainFunctionConstruct) == sizeOfSpilledArgumentRegisters && bytesAboveReturnAddressOf(StubFrameKind::PlainFunctionConstructMakesThis) == sizeOfSpilledArgumentRegisters);
static_assert(!bytesAboveReturnAddressOf(StubFrameKind::Plain) && !bytesAboveReturnAddressOf(StubFrameKind::Adapter) && !bytesAboveReturnAddressOf(StubFrameKind::ConstructAdapterMakesThis) && !bytesAboveReturnAddressOf(StubFrameKind::AtStubEntry));

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
        for (CallerFrame callerFrame : { CallerFrame::HasFrame, CallerFrame::Frameless, CallerFrame::StubFrame, CallerFrame::KeepsNoFPRs })
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

static void testPackedWords()
{
    constexpr uint64_t offsetInCode = 64;
    constexpr uintptr_t code = 4096;
    uint64_t closureWord = ClosureWord::encode({ offsetInCode, 3 }, 7, { });
    AOT_TEST_CHECK(ClosureWord::closureNumberOf(closureWord) == 7);
    AOT_TEST_CHECK(ClosureWord::addressOf(closureWord, code) == code + offsetInCode);
    AOT_TEST_CHECK(ClosureWord::numberOfParametersOf(closureWord) == 3);
    AOT_TEST_CHECK(ClosureWord::isCallable(closureWord) && !ClosureWord::takesNewTarget(closureWord));

    uint64_t classWord = ClosureWord::encode({ offsetInCode, ClosureWord::maxNumberOfParameters }, ClosureWord::firstReservedFunction - 1, { .isNotCallable = true, .takesNewTarget = true });
    AOT_TEST_CHECK(ClosureWord::closureNumberOf(classWord) == ClosureWord::firstReservedFunction - 1);
    AOT_TEST_CHECK(ClosureWord::addressOf(classWord, code) == code + offsetInCode);
    AOT_TEST_CHECK(ClosureWord::numberOfParametersOf(classWord) == ClosureWord::maxNumberOfParameters);
    AOT_TEST_CHECK(!ClosureWord::isCallable(classWord) && ClosureWord::takesNewTarget(classWord));

    uint64_t request = ClosureRequest::encode(5, 2, 9);
    AOT_TEST_CHECK(ClosureRequest::closureNumberOf(request) == 5);
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
    if (!signature.immediate)
        return;
    AOT_TEST_CHECK(!containsRegister(pinned, *signature.immediate));
    AOT_TEST_CHECK(!hasOperand(signature, *signature.immediate));
}

static void checkChosenOperandRegister(StubKey key, const StubSignature& signature, OperandRegisterChoice choice)
{
    if (!key.operandRegister())
        return;
    AOT_TEST_CHECK(choice != OperandRegisterChoice::NoChoice);
    AOT_TEST_CHECK(hasOperand(signature, *key.operandRegister()) || signature.firstResult == Reg(*key.operandRegister()));
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

static constexpr uint32_t nameOfFirstStubCall = 5;
static constexpr uint32_t nameOfSecondStubCall = 6;

template<typename Emit>
static Vector<uint8_t> assemble(const Emit& emit, const AssemblerPolicy& policy = AssemblerPolicy::forImage(StubOptions { }.targetFeatures))
{
    CCallHelpers jit;
    RelocationRecorder relocations(jit, policy);
    emit(jit);
    std::expected<RelocatableCode, LinkFailure> code = relocations.finalize(LinkBuffer::Profile::Thunk, nullptr);
    AOT_TEST_CHECK(code.has_value() && code->relocations.isEmpty());
    return WTF::move(code->bytes);
}

template<typename Emit>
static AssembledCalls assembleCalls(const StubCatalog& catalog, const Emit& emit)
{
    CCallHelpers jit;
    RelocationRecorder relocations(jit, AssemblerPolicy::forImage(catalog.options().targetFeatures));
    StubCalls calls(catalog, relocations);
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
    AOT_TEST_CHECK(result.labelOffsets.size() == result.body.relocations.size());
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
    StubCatalog catalog(StubOptions { });
    constexpr StubKey key = getById();
    AssembledCalls assembled = assembleCalls(catalog, [&](CCallHelpers& jit, StubCalls& calls, const auto& markCall) {
        markCall();
        calls.call(jit, key, nameOfFirstStubCall, OptionalIndex { 1 });
        markCall();
        calls.call(jit, key, nameOfSecondStubCall, OptionalIndex { 2 });
        markCall();
        calls.tailCall(jit, toBoolean());
    });
    const CodeBody& body = assembled.body;

    AOT_TEST_CHECK(body.stubKeys == (Vector<StubKey> { key, toBoolean() }));
    AOT_TEST_CHECK(body.relocations.size() == 3);
    checkRelocation(body.relocations[0], RelocationKind::Call, { SymbolSpace::Stub, 0 });
    checkRelocation(body.relocations[1], RelocationKind::Call, { SymbolSpace::Stub, 0 });
    checkRelocation(body.relocations[2], RelocationKind::Jump, { SymbolSpace::Stub, 1 });

    auto assembleMoveOf = [&](uint32_t immediate) {
        return assemble([&](CCallHelpers& jit) {
            jit.move(CCallHelpers::TrustedImm32(immediate), *catalog.summaryOf(key).signature.immediate);
        });
    };
    Vector<uint8_t> firstMove = assembleMoveOf(nameOfFirstStubCall);
    Vector<uint8_t> secondMove = assembleMoveOf(nameOfSecondStubCall);
    AOT_TEST_CHECK(equalSpans(assembled.bytesBefore(0), firstMove.span()));
    AOT_TEST_CHECK(equalSpans(assembled.bytesBefore(1), secondMove.span()));
    AOT_TEST_CHECK(assembled.bytesBefore(2).empty());

    AOT_TEST_CHECK(body.returnSites.size() == 2);
    checkReturnSite(assembled, 0, 0, OptionalIndex { 1 });
    checkReturnSite(assembled, 1, 1, OptionalIndex { 2 });
    AOT_TEST_CHECK(body.firstReadSites.isEmpty() && body.argumentMoves.isEmpty() && body.callees.isEmpty());
}

static void testStubCallsOfOperations()
{
    StubCatalog catalog(StubOptions { });
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

static void testStubCallsWithNumbersInKeys()
{
    StubCatalog catalog(StubOptions { });
    constexpr StubKey key { StubFamily::BuiltinCell };
    constexpr uint32_t firstMention = 3;
    constexpr uint32_t secondMention = 8;
    AssembledCalls assembled = assembleCalls(catalog, [&](CCallHelpers& jit, StubCalls& calls, const auto& markCall) {
        markCall();
        calls.call(jit, key, NumberedImmediate { firstMention }, OptionalIndex { 1 });
        markCall();
        calls.call(jit, key, NumberedImmediate { secondMention }, OptionalIndex { 2 });
        markCall();
        calls.call(jit, key, NumberedImmediate { firstMention }, OptionalIndex { 3 });
    });
    const CodeBody& body = assembled.body;

    AOT_TEST_CHECK(body.stubKeys == (Vector<StubKey> { key.withImmediate(0), key.withImmediate(0) }));
    constexpr uint32_t expectedSymbols[] = { 0, 1, 0 };
    AOT_TEST_CHECK(body.relocations.size() == std::size(expectedSymbols));
    for (size_t index = 0; index < std::size(expectedSymbols); ++index) {
        checkRelocation(body.relocations[index], RelocationKind::Call, { SymbolSpace::Stub, expectedSymbols[index] });
        AOT_TEST_CHECK(assembled.bytesBefore(index).empty());
    }
    constexpr uint32_t expectedMentions[] = { firstMention, secondMention };
    const auto& numberedStubKeys = body.rareLists->numberedStubKeys;
    AOT_TEST_CHECK(numberedStubKeys.size() == std::size(expectedMentions));
    for (uint32_t index = 0; index < std::size(expectedMentions); ++index)
        AOT_TEST_CHECK(numberedStubKeys[index].stubKey == index && numberedStubKeys[index].mention == expectedMentions[index]);
}

static void testStubCallsOfOneKey()
{
    StubCatalog catalog(StubOptions { });
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
    StubCatalog catalog(StubOptions { });
    constexpr StubKey key = throwAtSite(ThrowSiteSource::WordsAfterCall, CallerFrame::HasFrame);
    AssembledCalls assembled = assembleCalls(catalog, [&](CCallHelpers& jit, StubCalls& calls, const auto& markCall) {
        markCall();
        calls.call(jit, key, OptionalIndex { 2 });
        markCall();
        calls.callForTwoReads(jit, key, OptionalIndex { 3 }, OptionalIndex { 4 });
    });
    const CodeBody& body = assembled.body;

    AOT_TEST_CHECK(body.stubKeys == Vector<StubKey> { key });
    AOT_TEST_CHECK(body.returnSites.size() == 2 && body.firstReadSites.size() == 1);
    checkReturnSite(assembled, 0, 0, OptionalIndex { 2 });
    checkReturnSite(assembled, 1, 1, OptionalIndex { 4 });
    AOT_TEST_CHECK(body.firstReadSites[0].returnOffset == body.returnSites[1].offset && body.firstReadSites[0].site == 3);
}

static void testStubCallsThroughArgumentThunks()
{
    StubCatalog catalog(StubOptions { });
    constexpr StubKey key = getById();
    constexpr StubKey keyOfOperation = callOperation(OperationCall::Returns::Value, OperationCall::FirstArgument::Instance);
    constexpr uint32_t offsetOfOperation = offsetInRuntimeTable(Entry::operationAOTValueAdd);
    ArgumentMoves moves;
    appendArgumentMove(moves, argumentGPR(0), ArgumentSource::Constant, 42);

    AssembledCalls assembled = assembleCalls(catalog, [&](CCallHelpers& jit, StubCalls& calls, const auto& markCall) {
        markCall();
        calls.call(jit, keyOfOperation, offsetOfOperation, OptionalIndex { 1 }, moves);
        markCall();
        calls.call(jit, key, nameOfFirstStubCall, OptionalIndex { 2 }, moves);
        markCall();
        calls.call(jit, key, std::nullopt, OptionalIndex { 3 }, moves);
    });
    const CodeBody& body = assembled.body;

    AOT_TEST_CHECK(body.stubKeys.isEmpty() && body.argumentMoves.size() == 3 && body.returnSites.size() == 3);
    const uint64_t expectedHeaders[][2] = {
        { keyOfOperation.withImmediate(offsetOfOperation).bits(), 0 },
        { key.bits(), 1ull << 32 | nameOfFirstStubCall },
        { key.bits(), 0 },
    };
    for (uint32_t index = 0; index < 3; ++index) {
        checkRelocation(body.relocations[index], RelocationKind::Call, { SymbolSpace::ArgumentThunk, index });
        AOT_TEST_CHECK(assembled.bytesBefore(index).empty());
        checkReturnSite(assembled, index, index, OptionalIndex { index + 1 });
        std::span<const uint64_t> words = body.argumentMoves[index];
        AOT_TEST_CHECK(words.size() == 2 + moves.size() && words[0] == expectedHeaders[index][0] && words[1] == expectedHeaders[index][1]);
        AOT_TEST_CHECK(equalSpans(words.subspan(2), moves.span()));
    }
}

static void testArgumentMovesOfUnitLieInOneList()
{
    constexpr size_t mostLists = 5;
    Vector<ArgumentMoves> lists;
    size_t numberOfWordsInLists = 0;
    for (size_t numberOfLists = 0; numberOfLists <= mostLists; ++numberOfLists) {
        ArgumentMovesOfUnit kept(lists.span());
        AOT_TEST_CHECK(kept.size() == numberOfLists && kept.isEmpty() == !numberOfLists);
        AOT_TEST_CHECK(kept.list().size() == (numberOfLists ? numberOfLists / 2 + 1 + numberOfWordsInLists : 0));
        AOT_TEST_CHECK(kept.list().capacity() == kept.list().size());
        for (size_t index = 0; index < numberOfLists; ++index)
            AOT_TEST_CHECK(equalSpans(std::as_const(kept)[index], lists[index].span()));
        AOT_TEST_CHECK(kept == ArgumentMovesOfUnit(lists.span()));

        if (numberOfLists) {
            ArgumentMovesOfUnit changed = kept;
            changed[numberOfLists - 1][1] ^= 1;
            AOT_TEST_CHECK(changed != kept && changed.size() == numberOfLists);
            AOT_TEST_CHECK(changed[numberOfLists - 1][1] == (lists.last()[1] ^ 1) && changed[0][0] == lists[0][0]);
        }

        ArgumentMoves next;
        for (size_t word = 0; word < numberOfHeaderWords + 4 * numberOfLists; ++word)
            next.append((numberOfLists + 1) << 32 | word);
        numberOfWordsInLists += next.size();
        lists.append(WTF::move(next));
    }
}

static void testCallsOfFunctions()
{
    StubCatalog catalog(StubOptions { });
    constexpr uint16_t secondaryEntry = 3;
    Typed::CodeUnit first { FunctionID { 11u }, Typed::CodeRole::GenericCallEntry };
    Typed::CodeUnit second { FunctionID { 11u }, Typed::CodeRole::Body };
    AssembledCalls assembled = assembleCalls(catalog, [&](CCallHelpers& jit, StubCalls& calls, const auto& markCall) {
        markCall();
        calls.callFunction(jit, first, primaryEntry, OptionalIndex { 1 });
        markCall();
        calls.callFunction(jit, second, secondaryEntry, { });
        markCall();
        calls.jumpToFunction(jit, first, secondaryEntry);
    });
    const CodeBody& body = assembled.body;

    AOT_TEST_CHECK(body.callees == (Vector<Typed::CodeUnit> { first, second }) && body.stubKeys.isEmpty());
    checkRelocation(body.relocations[0], RelocationKind::Call, { SymbolSpace::Function, 0, primaryEntry });
    checkRelocation(body.relocations[1], RelocationKind::Call, { SymbolSpace::Function, 1, secondaryEntry });
    checkRelocation(body.relocations[2], RelocationKind::Jump, { SymbolSpace::Function, 0, secondaryEntry });
    for (size_t index = 0; index < body.relocations.size(); ++index)
        AOT_TEST_CHECK(assembled.bytesBefore(index).empty());
    AOT_TEST_CHECK(body.returnSites.size() == 2);
    checkReturnSite(assembled, 0, 0, OptionalIndex { 1 });
    checkReturnSite(assembled, 1, 1, { });
}

#if OS(UNIX)

static void runInMappingOfThisProcess(std::span<const uint8_t> code)
{
    void* memory = mmap(nullptr, code.size(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    AOT_TEST_CHECK(memory != MAP_FAILED);
    memcpySpan(unsafeMakeSpan(static_cast<uint8_t*>(memory), code.size()), code);
    AOT_TEST_CHECK(!mprotect(memory, code.size(), PROT_READ | PROT_EXEC));
    MacroAssembler::cacheFlush(memory, code.size());
    std::bit_cast<void (SYSV_ABI *)()>(tagCodePtr<CFunctionPtrTag>(memory))();
}

static int signalThatEndsListener(std::span<const uint8_t> code)
{
    constexpr unsigned timeoutInSeconds = 10;

    fflush(nullptr);
    pid_t child = fork();
    AOT_TEST_CHECK(child >= 0);
    if (!child) {
        alarm(timeoutInSeconds);
        signal(SIGTRAP, [](int) { });
        for (int number : { SIGILL, SIGSEGV, SIGBUS, SIGALRM })
            signal(number, [](int number) { _exit(number); });
        runInMappingOfThisProcess(code);
        _exit(0);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    AOT_TEST_CHECK(waited == child && WIFEXITED(status));
    return WEXITSTATUS(status);
}

static void testTrapsEndListenerForBreakpoints()
{
    WTF::fastDisableScavenger();
    auto signalBehind = [](const auto& emit) {
        Vector<uint8_t> code = assemble([&](CCallHelpers& jit) {
            emit(jit);
            jit.ret();
        });
        return signalThatEndsListener(code.span());
    };
    AOT_TEST_CHECK(!signalBehind([](CCallHelpers&) { }));
    AOT_TEST_CHECK(signalBehind([](CCallHelpers& jit) { jit.breakpoint(); }) == SIGILL);
    AOT_TEST_CHECK(signalBehind([](CCallHelpers& jit) { jit.oops(); }) == SIGILL);
    AOT_TEST_CHECK(signalBehind(emitPadding) == (isX86_64() ? SIGSEGV : SIGILL));

    AOT_TEST_CHECK(!AssemblerPolicy::forJIT().trapsWithIllegalInstruction);
    AssemblerPolicy policy = AssemblerPolicy::forImage(StubOptions { }.targetFeatures);
    policy.trapsWithIllegalInstruction = false;
    Vector<uint8_t> breakpoint = assemble([](CCallHelpers& jit) { jit.breakpoint(); }, policy);
    AOT_TEST_CHECK(breakpoint.size() == codeOffsetUnit && MacroAssembler::isBreakpoint(breakpoint.mutableSpan().data()));
}

static void testAlignmentOfStubsIsPadding()
{
    StubCatalog catalog(StubOptions { });
    StubAssembler jit(catalog.options(), catalog);
    jit.ret();
    const size_t sizeOfReturn = jit.m_assembler.codeSize();
    jit.align();
    AOT_TEST_CHECK(isX86_64() ? jit.m_assembler.codeSize() == sizeOfReturn : !(jit.m_assembler.codeSize() % 16));
    emitPadding(jit);
    jit.ret();
    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::Thunk);
    AOT_TEST_CHECK(!buffer.didFailToAllocate());
    buffer.finalize(false, "stub with an aligned part"_s);
    std::span<uint8_t> bytes = buffer.bytes();
    AOT_TEST_CHECK(bytes.size() > 2 * sizeOfReturn);
    for (size_t offset = 0; offset < bytes.size(); offset += codeOffsetUnit)
        AOT_TEST_CHECK(!MacroAssembler::isBreakpoint(bytes.subspan(offset).data()));
}

#endif // OS(UNIX)

static void testRegistersLiveAcrossReads()
{
    for (const StubOptions& options : stubOptionVariants()) {
        StubCatalog catalog(options);
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

static void testHeadsOfOperationsWriteWhatRowsName()
{
    for (const StubOptions& options : stubOptionVariants()) {
        StubCatalog catalog(options);
        unsigned numberOfHeads = 0;
        forEachKeyWithoutImmediate([&](StubKey key) {
            if (descriptorOf(key.family()).binding != ImmediateBinding::OperationOffset)
                return;
            for (const OperationPrelude& prelude : operationPreludes()) {
                StubKey head = key.withImmediate(offsetInRuntimeTable(prelude.operation));
                if (!exists(head))
                    continue;
                ++numberOfHeads;
                AOT_TEST_CHECK(catalog.effectsOf(head).writes.isSubsetOf(effectsOf(prelude.operation).writes));
            }
        });
        AOT_TEST_CHECK(numberOfHeads >= operationPreludes().size());
        for (unsigned count = 0; count <= numberOfArgumentGPRs + 1; ++count)
            AOT_TEST_CHECK(!catalog.effectsOf(resolveCallValue(count)).mayCollect);
    }
}

static void testWhatFamiliesDeclareOfTheirCallsHolds()
{
    for (const StubOptions& options : stubOptionVariants()) {
        StubCatalog catalog(options);
        unsigned numberOfLeaves = 0;
        auto lookAt = [&](StubKey key) {
            Typed::Effects effects = catalog.effectsOf(key);
            StubLeafness leafness = descriptorOf(key.family()).leafness;
            if (leafness == StubLeafness::MayCall)
                return;
            AOT_TEST_CHECK(!effects.mayCollect && !effects.mayThrow && !effects.mayCallUnknownCode);
            if (leafness != StubLeafness::Leaf)
                return;
            ++numberOfLeaves;
            Vector<StubKey> bodies { bodyOf(key) };
            for (size_t index = 0; index < bodies.size(); ++index) {
                const StubRecording& recording = catalog.recordingOf(bodies[index]);
                AOT_TEST_CHECK(recording.operationsCalled.isEmpty());
                for (const StubReference& reference : recording.references) {
                    StubKey body = bodyOf(reference.target);
                    if (reference.kind != StubReference::Kind::Address && reference.kind != StubReference::Kind::ReturnedAddress && !bodies.contains(body))
                        bodies.append(body);
                }
            }
        };
        forEachKeyWithoutImmediate([&](StubKey key) {
            lookAt(key);
            if (descriptorOf(key.family()).binding != ImmediateBinding::OperationOffset)
                return;
            for (const OperationPrelude& prelude : operationPreludes()) {
                StubKey head = key.withImmediate(offsetInRuntimeTable(prelude.operation));
                if (exists(head))
                    lookAt(head);
            }
        });
        AOT_TEST_CHECK(numberOfLeaves);
    }
}

static constexpr std::array constantsAtSites { ConstantAtSite::String, ConstantAtSite::Key, ConstantAtSite::BuiltinCell };

static void testConstantsAtSitesLoseNoRegister()
{
    RegisterSet scratch = RegisterSet::macroClobberedGPRs();
    scratch.merge(RegisterSet::macroClobberedFPRs());
    for (const StubOptions& options : stubOptionVariants()) {
        StubCatalog catalog(options);
        for (ConstantAtSite constant : constantsAtSites) {
            for (GPRReg result : allocatableGPRs) {
                StubKey key = createConstantAtSite(constant).withOperandRegister(result);
                AOT_TEST_CHECK(exists(key));
                const StubSummary& summary = catalog.summaryOf(key);
                AOT_TEST_CHECK(summary.signature.result == StubResult::GPR && summary.signature.firstResult == Reg(result) && !summary.signature.immediate);
                AOT_TEST_CHECK(!summary.signature.numberOfOperands);
                RegisterSet lost = summary.clobbered;
                lost.remove(result);
                lost.exclude(scratch);
                AOT_TEST_CHECK(lost.isEmpty());
                Typed::Effects effects = catalog.effectsOf(key);
                AOT_TEST_CHECK(!effects.mayCollect && !effects.mayExitSideways());
            }
        }
    }
}

using RegistersAtSite = std::array<uint64_t, allocatableGPRs.size()>;

static constexpr uint64_t valueKeptAtSiteIn(size_t index) { return 0x0a0b0c0d00000000ull + index; }
static constexpr uint32_t numberOfConstantAtSite = 5;

static RegistersAtSite callStubAtSite(StubRunner& runner, StubKey key)
{
    Vector<GPRReg> savedRegisters;
    RegisterSet::vmCalleeSaveRegisters().forEach([&](Reg reg) {
        if (reg.isGPR())
            savedRegisters.append(reg.gpr());
    });
    auto slotOf = [](size_t index) {
        return CCallHelpers::Address(GPRInfo::callFrameRegister, -static_cast<int32_t>((index + 1) * sizeof(CPURegister)));
    };
    const size_t slotOfAnswer = savedRegisters.size();
    const size_t firstSlotOfRegisters = slotOfAnswer + 1;
    constexpr int32_t offsetOfStub = static_cast<int32_t>(OBJECT_OFFSETOF(StubEnvironment, stub)) - static_cast<int32_t>(OBJECT_OFFSETOF(StubEnvironment, instance));

    CCallHelpers jit;
    jit.emitFunctionPrologue();
    jit.subPtr(CCallHelpers::TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>((firstSlotOfRegisters + allocatableGPRs.size()) * sizeof(CPURegister))), CCallHelpers::stackPointerRegister);
    for (size_t index = 0; index < savedRegisters.size(); ++index)
        jit.storePtr(savedRegisters[index], slotOf(index));
    jit.storePtr(GPRInfo::argumentGPR1, slotOf(slotOfAnswer));
    jit.move(GPRInfo::argumentGPR0, instanceGPR);
#if CPU(ARM64)
    jit.emitMaterializeTagCheckRegisters();
#endif
    jit.loadPtr(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(cellCageBase)), cellCageBaseGPR);
    for (size_t index = 0; index < allocatableGPRs.size(); ++index)
        jit.move(CCallHelpers::TrustedImm64(valueKeptAtSiteIn(index)), allocatableGPRs[index]);

    jit.call(CCallHelpers::Address(instanceGPR, offsetOfStub), JITThunkPtrTag);
#if CPU(X86_64)
    jit.m_assembler.buffer().putByte(static_cast<int8_t>(ThrowSiteWord::opcodeInFrontOfWordBehindCall));
#endif
    jit.m_assembler.buffer().putInt(numberOfConstantAtSite);

    for (size_t index = 0; index < allocatableGPRs.size(); ++index)
        jit.store64(allocatableGPRs[index], slotOf(firstSlotOfRegisters + index));
    jit.loadPtr(slotOf(slotOfAnswer), GPRInfo::regT0);
    for (size_t index = 0; index < allocatableGPRs.size(); ++index) {
        jit.load64(slotOf(firstSlotOfRegisters + index), GPRInfo::regT1);
        jit.store64(GPRInfo::regT1, CCallHelpers::Address(GPRInfo::regT0, static_cast<int32_t>(index * sizeof(uint64_t))));
    }
    for (size_t index = 0; index < savedRegisters.size(); ++index)
        jit.loadPtr(slotOf(index), savedRegisters[index]);
    jit.emitFunctionEpilogue();
    jit.ret();

    LinkBuffer linkBuffer(jit, nullptr);
    MacroAssemblerCodeRef<JSEntryPtrTag> site = FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "AOT site of a stub");
    void* addressOfSite = untagCFunctionPtr<JSEntryPtrTag>(site.code().taggedPtr());

    StubEnvironment& environment = StubEnvironment::of(&runner.instance());
    environment.stub = runner.entryOf(key);
    environment.numberOfOperationCalls = 0;
    environment.numberOfRecordedCalls = 0;
    environment.recordedArguments = { };
    RegistersAtSite registers { };
    std::bit_cast<void (SYSV_ABI *)(Instance*, uint64_t*)>(addressOfSite)(&runner.instance(), registers.data());
    return registers;
}

static void checkRegistersAtSite(const RegistersAtSite& registers, GPRReg result, uint64_t expectedResult)
{
    for (size_t index = 0; index < allocatableGPRs.size(); ++index)
        AOT_TEST_CHECK(registers[index] == (allocatableGPRs[index] == result ? expectedResult : valueKeptAtSiteIn(index)));
}

static constexpr size_t numberOfCellWordsAtSites = 8;
static constexpr uint32_t narrowWordOfCellAtSite = 0x4321;
static constexpr uint64_t cellBehindNarrowWordAtSite = static_cast<uint64_t>(narrowWordOfCellAtSite) << cell32Shift;

static void testConstantsAtSitesTakeNumberBehindCall()
{
    constexpr uint64_t madeCell = 0x123456780;
    struct Site {
        ConstantAtSite constant;
        Entry operation;
    };
    constexpr Site sites[] = {
        { ConstantAtSite::Key, Entry::operationAOTKeyConstant },
        { ConstantAtSite::BuiltinCell, Entry::operationAOTBuiltinCell },
    };

    StubRunner runner;
    StubEnvironment& environment = StubEnvironment::of(&runner.instance());
    std::array<uint32_t, numberOfCellWordsAtSites> keyCells { };
    std::array<JSCell*, numberOfCellWordsAtSites> builtinCells { };
    runner.instance().runtime.constants.keyCells = keyCells.data();
    runner.instance().prefix().builtinCells = builtinCells.data();
    runner.setResultOfRecordedOperation(madeCell);
    for (const Site& site : sites) {
        runner.recordCallsOf(site.operation);
        for (GPRReg result : allocatableGPRs) {
            StubKey key = createConstantAtSite(site.constant).withOperandRegister(result);
            RegistersAtSite registers = callStubAtSite(runner, key);
            checkRegistersAtSite(registers, result, madeCell);
            AOT_TEST_CHECK(environment.numberOfRecordedCalls == 1 && environment.recordedArguments[0] == numberOfConstantAtSite);
        }
    }
}

static void testConstantsAtSitesReadFilledWord()
{
    constexpr uint64_t builtinCell = 0x123456780;
    StubRunner runner;
    StubEnvironment& environment = StubEnvironment::of(&runner.instance());
    std::array<uint32_t, numberOfCellWordsAtSites> keyCells { };
    std::array<JSCell*, numberOfCellWordsAtSites> builtinCells { };
    runner.instance().runtime.constants.keyCells = keyCells.data();
    runner.instance().prefix().builtinCells = builtinCells.data();
    runner.stringCell(numberOfConstantAtSite) = narrowWordOfCellAtSite;
    keyCells[numberOfConstantAtSite] = narrowWordOfCellAtSite;
    builtinCells[numberOfConstantAtSite] = std::bit_cast<JSCell*>(builtinCell);
    for (ConstantAtSite constant : { ConstantAtSite::String, ConstantAtSite::Key, ConstantAtSite::BuiltinCell }) {
        for (GPRReg result : allocatableGPRs) {
            RegistersAtSite registers = callStubAtSite(runner, createConstantAtSite(constant).withOperandRegister(result));
            checkRegistersAtSite(registers, result, constant == ConstantAtSite::BuiltinCell ? builtinCell : cellBehindNarrowWordAtSite);
            AOT_TEST_CHECK(!environment.numberOfOperationCalls);
        }
    }
}

static UGPRPair JIT_OPERATION_ATTRIBUTES makeKeyAndFillWordAtSite(Instance* instance, uint64_t number)
{
    ++StubEnvironment::of(instance).numberOfOperationCalls;
    unsafeMakeSpan(instance->runtime.constants.keyCells, numberOfCellWordsAtSites)[number] = narrowWordOfCellAtSite;
    return makeUGPRPair(cellBehindNarrowWordAtSite, 0);
}

static void testConstantAtSiteIsMadeOnce()
{
    StubRunner runner;
    StubEnvironment& environment = StubEnvironment::of(&runner.instance());
    std::array<uint32_t, numberOfCellWordsAtSites> keyCells { };
    runner.instance().runtime.constants.keyCells = keyCells.data();
    runner.install(Entry::operationAOTKeyConstant, makeKeyAndFillWordAtSite);
    bool isMade = false;
    for (GPRReg result : allocatableGPRs) {
        RegistersAtSite registers = callStubAtSite(runner, createConstantAtSite(ConstantAtSite::Key).withOperandRegister(result));
        checkRegistersAtSite(registers, result, cellBehindNarrowWordAtSite);
        AOT_TEST_CHECK(environment.numberOfOperationCalls == (isMade ? 0u : 1u));
        AOT_TEST_CHECK(keyCells[numberOfConstantAtSite] == narrowWordOfCellAtSite);
        isMade = true;
    }
}

std::span<const NativeTest> stubTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/keys-are-legal-and-ascending"_s, testLegalKeys },
        { "stubs/illegal-keys-do-not-exist"_s, testIllegalKeys },
        { "stubs/variant-lookup"_s, testVariantLookup },
        { "stubs/entries-of-a-body"_s, testBodyEntries },
        { "stubs/packed-words"_s, testPackedWords },
        { "stubs/stub-words"_s, testStubWords },
        { "stubs/adapter-frame"_s, testAdapterFrame },
        { "stubs/handler-tables"_s, testHandlerTables },
        { "stubs/signatures-are-well-formed"_s, testSignatures },
        { "stubs/calls-with-immediates"_s, testStubCallsWithImmediates },
        { "stubs/calls-of-operations"_s, testStubCallsOfOperations },
        { "stubs/calls-with-numbers-in-keys"_s, testStubCallsWithNumbersInKeys },
        { "stubs/calls-of-one-key"_s, testStubCallsOfOneKey },
        { "stubs/call-for-two-reads"_s, testStubCallForTwoReads },
        { "stubs/calls-through-argument-thunks"_s, testStubCallsThroughArgumentThunks },
        { "stubs/argument-moves-of-a-unit-lie-in-one-list"_s, testArgumentMovesOfUnitLieInOneList },
        { "stubs/calls-of-functions"_s, testCallsOfFunctions },
#if OS(UNIX)
        { "stubs/traps-end-listener-for-breakpoints"_s, testTrapsEndListenerForBreakpoints },
        { "stubs/alignment-is-padding"_s, testAlignmentOfStubsIsPadding },
#endif
        { "stubs/registers-live-across-reads"_s, testRegistersLiveAcrossReads },
        { "stubs/heads-of-operations-write-what-their-rows-name"_s, testHeadsOfOperationsWriteWhatRowsName },
        { "stubs/what-families-declare-of-their-calls-holds"_s, testWhatFamiliesDeclareOfTheirCallsHolds },
        { "stubs/constants-at-sites-lose-no-register"_s, testConstantsAtSitesLoseNoRegister },
        { "stubs/constants-at-sites-take-the-number-behind-the-call"_s, testConstantsAtSitesTakeNumberBehindCall },
        { "stubs/constants-at-sites-read-a-filled-word"_s, testConstantsAtSitesReadFilledWord },
        { "stubs/constant-at-a-site-is-made-once"_s, testConstantAtSiteIsMadeOnce },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
