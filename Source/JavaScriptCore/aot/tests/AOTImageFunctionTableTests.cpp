/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBodyFactsForTables.h"
#include "AOTCodeLayout.h"
#include "AOTCompiledFrame.h"
#include "AOTFunctionKinds.h"
#include "AOTFunctionTables.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTImage.h"
#include "AOTImageBuilder.h"
#include "AOTImageTestSupport.h"
#include "AOTInstance.h"
#include "AOTNumbering.h"
#include "AOTOperationData.h"
#include "AOTPlan.h"
#include "AOTPrefixCode.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRepresentation.h"
#include "AOTStubCalls.h"
#include "AOTStubFrames.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsCallLists.h"
#include "AOTStubsCalls.h"
#include "AOTStubsExceptions.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTTableParts.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "FFIStubs.h"
#include "LLIntData.h"
#include "LLIntThunks.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/MathExtras.h>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

using namespace ImageTests;

static CodeSketch sketchWithSite(CodeUnit unit, unsigned frameSize)
{
    CodeSketch sketch(unit);
    sketch.withFrame(frameSize);
    sketch.plain(0x1000 + unit.function.index());
    sketch.callStub(writeBarrier(), sketch.addSite());
    return sketch;
}

static CodeSketch sketchWithSite(uint32_t function, unsigned frameSize)
{
    return sketchWithSite(bodyUnitOf(function), frameSize);
}

static CompiledCode plainBody(uint32_t function, unsigned frameSize = 16)
{
    CodeSketch sketch = sketchWithSite(function, frameSize);
    sketch.end();
    return sketch.take();
}

static CompiledCode plainCodeOf(CodeUnit unit)
{
    CodeSketch sketch = sketchWithSite(unit, 16);
    sketch.end();
    return sketch.take();
}

static CompiledCode jumpToStubBody(uint32_t function)
{
    CodeSketch sketch(function);
    sketch.plain(7);
    sketch.jumpToStub(catchException());
    return sketch.take();
}

static void testImageFunctionOf()
{
    TestBuild build(8);
    TestImage image = build.build(bodies(jumpToStubBody(1), jumpToStubBody(2), plainBody(4), plainBody(6)));
    const Image& loaded = *image.image;
    auto hasOwnRecord = [&](uint32_t function) {
        std::optional<uint32_t> index = loaded.imageFunctionOf(FunctionID(function));
        return index && loaded.rootFunctionOf(*index) == FunctionID(function);
    };

    for (uint32_t function : { 1u, 2u, 4u, 6u })
        AOT_TEST_CHECK(hasOwnRecord(function));
    AOT_TEST_CHECK(loaded.imageFunctionOf(FunctionID(1u)) != loaded.imageFunctionOf(FunctionID(2u)));
    for (uint32_t function : { 0u, 3u, 5u, 7u })
        AOT_TEST_CHECK(!loaded.imageFunctionOf(FunctionID(function)));
    AOT_TEST_CHECK(!loaded.imageFunctionOf(FunctionID(build.program().functions.size() + 100)));
}

static Vector<CompiledCode> recordBodies()
{
    CodeSketch withHandler = sketchWithSite(3, 48);
    uint32_t handlerOffset = withHandler.plain(0x2000);
    withHandler.code().sites.rareLists.ensure().handlers.append(SiteHandler { 1, handlerOffset, HandlerType::Catch });
    withHandler.end();
    return bodies(plainBody(1, 32), plainBody(2, 48), withHandler.take(), jumpToStubBody(4));
}

static void testRecords()
{
    using enum ImageFunctionFlag;
    TestBuild build(5);
    TestImage image = build.build(recordBodies());
    const Image& loaded = *image.image;
    uint32_t first = image.indexOf(1);
    uint32_t second = image.indexOf(2);
    uint32_t third = image.indexOf(3);
    uint32_t frameless = image.indexOf(4);

    AOT_TEST_CHECK(loaded.frameOf(first).frameSizeInBytes() == 32);
    AOT_TEST_CHECK(loaded.frameOf(second).frameSizeInBytes() == 48);
    AOT_TEST_CHECK(loaded.frameOf(third).frameSizeInBytes() == 48);
    AOT_TEST_CHECK(!loaded.frameOf(frameless).frameSizeInBytes());
    AOT_TEST_CHECK(&loaded.frameOf(second) == &loaded.frameOf(third));
    AOT_TEST_CHECK(&loaded.frameOf(first) != &loaded.frameOf(second));
    AOT_TEST_CHECK(loaded.header().counts.functions == 4);

    AOT_TEST_CHECK(loaded.function(first).flags() == OptionSet<ImageFunctionFlag>({ HasCallSites, HasPositions }));
    AOT_TEST_CHECK(loaded.function(second).flags() == OptionSet<ImageFunctionFlag>({ HasCallSites, HasPositions }));
    AOT_TEST_CHECK(loaded.function(third).flags() == OptionSet<ImageFunctionFlag>({ HasCallSites, HasPositions, HasHandlers }));
    AOT_TEST_CHECK(!loaded.function(frameless).has(HasHandlers));
    AOT_TEST_CHECK(!loaded.function(frameless).has(HasFrameMaps));
    for (uint32_t function : { 1u, 2u, 3u, 4u })
        AOT_TEST_CHECK(loaded.rootFunctionOf(image.indexOf(function)) == FunctionID(function));
}

static SUPPRESS_ASAN void testSampleInNativeCode()
{
    TestBuild build(4);
    CodeSketch outer(2);
    outer.withFrame(48);
    outer.plain(0x1002);
    outer.callFunction(1, outer.addSite());
    uint32_t afterIndirectCall = outer.callStub(callValue(1), outer.addSite()) + sizeOfNearCall;
    uint32_t afterCallToOtherFunction = outer.callFunction(3, outer.addSite()) + sizeOfNearCall;
    uint32_t afterCallToNativeCode = outer.callStub(writeBarrier(), outer.addSite()) + sizeOfNearCall;
    outer.end();
    CodeSketch larger = sketchWithSite(3, 64);
    uint32_t afterCallInLargerFrame = larger.callFunction(1, larger.addSite()) + sizeOfNearCall;
    larger.end();
    TestImage image = build.build(bodies(plainBody(1, 32), outer.take(), larger.take()));
    const Image& loaded = *image.image;
    const StackBounds& bounds = Thread::currentSingleton().stack();
    auto returnAddressIn = [&](uint32_t function) {
        return const_cast<uint8_t*>(image.address(function, sizeof(uint32_t) + sizeOfNearCall));
    };
    void* nativeCode = reinterpret_cast<void*>(&testSampleInNativeCode);

    alignas(16) std::array<void*, 48> stack { };
    constexpr size_t recordOfCaller = 32;
    constexpr size_t recordOfOuter = recordOfCaller + 2 + 48 / sizeof(void*);
    constexpr size_t endOfNativeFrame = recordOfCaller - 32 / sizeof(void*);
    stack[recordOfOuter] = &stack[recordOfOuter + 4];
    stack[recordOfOuter + 1] = nativeCode;
    stack[recordOfCaller] = &stack[recordOfOuter];
    stack[recordOfCaller + 1] = returnAddressIn(2);
    auto isTop = [&](const SampledRegisters& registers, size_t record, const void* pc) {
        std::optional<SampledTop> top = findSampledTop(loaded, registers, nullptr, nullptr, bounds);
        return top && top->callFrame == std::bit_cast<CallFrame*>(&stack[record]) && top->pc == pc && !top->framelessTopCallerPC && !top->framePointer;
    };

    SampledRegisters withoutFrameRecord { &stack[recordOfCaller], nativeCode, nullptr, &stack[endOfNativeFrame - 8] };
    AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfOuter, returnAddressIn(2)));
    if constexpr (isX86_64()) {
        stack[endOfNativeFrame - 6] = returnAddressIn(3);
        AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfOuter, returnAddressIn(2)));
        stack[endOfNativeFrame - 1] = const_cast<uint8_t*>(image.address(1, sizeof(uint32_t)));
        AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfOuter, returnAddressIn(2)));
        stack[endOfNativeFrame - 1] = returnAddressIn(1);
        AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfCaller, returnAddressIn(1)));
        constexpr size_t endOfLargerFrame = recordOfCaller - 64 / sizeof(void*);
        stack[endOfLargerFrame - 1] = returnAddressIn(3);
        AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfCaller, returnAddressIn(1)));
        stack[recordOfCaller + 1] = const_cast<uint8_t*>(image.address(2, afterIndirectCall));
        AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfOuter, image.address(2, afterIndirectCall)));
        stack[endOfLargerFrame - 1] = const_cast<uint8_t*>(image.address(3, afterCallInLargerFrame));
        AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfCaller, returnAddressIn(1)));
        stack[recordOfCaller + 1] = const_cast<uint8_t*>(image.address(2, afterCallToOtherFunction));
        AOT_TEST_CHECK(isTop(withoutFrameRecord, recordOfCaller, returnAddressIn(1)));
    }

    void* afterNativeCall = const_cast<uint8_t*>(image.address(2, afterCallToNativeCode));
    stack[endOfNativeFrame - 1] = returnAddressIn(1);
    stack[recordOfCaller + 1] = afterNativeCall;
    for (void* linkRegister : { static_cast<void*>(nullptr), afterNativeCall, nativeCode }) {
        SampledRegisters withOwnFrameRecord { &stack[recordOfCaller], nativeCode, linkRegister, &stack[endOfNativeFrame - 8] };
        AOT_TEST_CHECK(isTop(withOwnFrameRecord, recordOfOuter, afterNativeCall));
    }
    stack[recordOfCaller + 1] = returnAddressIn(2);

    if constexpr (isARM64()) {
        auto check = [&](void* returnAddress) {
            SampledRegisters inLeaf { &stack[recordOfCaller], nativeCode, returnAddress, &stack[endOfNativeFrame] };
            AOT_TEST_CHECK(isTop(inLeaf, recordOfCaller, returnAddressIn(1)));
            stack[endOfNativeFrame - 2] = &stack[recordOfCaller];
            stack[endOfNativeFrame - 1] = returnAddress;
            SampledRegisters withFrameRecord { &stack[endOfNativeFrame - 2], nativeCode, returnAddress, &stack[endOfNativeFrame - 8] };
            AOT_TEST_CHECK(isTop(withFrameRecord, recordOfCaller, returnAddressIn(1)));
            withFrameRecord.linkRegister = nativeCode;
            AOT_TEST_CHECK(isTop(withFrameRecord, recordOfCaller, returnAddressIn(1)));
        };
        check(returnAddressIn(1));
#if CPU(ARM64) && !OS(DARWIN)
        uintptr_t signedReturnAddress = std::bit_cast<uintptr_t>(returnAddressIn(1));
        asm("mov x30, %0\n"
            "hint #24\n"
            "mov %0, x30"
            : "+r"(signedReturnAddress) : : "x30");
        check(std::bit_cast<void*>(signedReturnAddress));
#endif
    }
}

static void* findReturnSiteInStubs(const Image& image, StubFrameKind kind, const void* behind = nullptr)
{
    auto looksLikeEndOfCall = [](const uint8_t* end) {
        return !isX86_64() || end[-static_cast<ptrdiff_t>(sizeOfNearCall)] == 0xe8 || (end[-2] == 0xff && end[-1] >> 3 == 0x1a) || (end[-3] == 0xff && end[-2] >> 3 == 0x0a);
    };
    for (const uint8_t& byte : image.code().subspan(sizeOfNearCall)) {
        CodeLocation location = image.locate(&byte);
        if (&byte > behind && location.kind == CodeKind::Stub && location.stubFrameKind == kind && (kind != StubFrameKind::Plain || looksLikeEndOfCall(&byte)))
            return const_cast<uint8_t*>(&byte);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

#if CPU(ARM64)
static SUPPRESS_ASAN void testSampleInNativeCodeThatLeavesAddressInLinkRegister()
{
    TestBuild build(3);
    CodeSketch outer(2);
    outer.withFrame(48);
    outer.plain(0x1002);
    outer.callStub(callValue(1), outer.addSite());
    outer.end();
    TestImage image = build.build(bodies(plainBody(1, 32), outer.take()));
    const Image& loaded = *image.image;
    auto returnAddressIn = [&](uint32_t function) -> void* {
        return const_cast<uint8_t*>(image.address(function, sizeof(uint32_t) + sizeOfNearCall));
    };
    auto behindInstructionsInStub = [&](uint32_t first, uint32_t second) -> void* {
        auto* code = reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(loaded.code().data()));
        code[0] = first;
        code[1] = second;
        AOT_TEST_CHECK(loaded.locate(code + 2).kind == CodeKind::Stub && loaded.locate(code + 2).stubFrameKind == StubFrameKind::Plain);
        return code + 2;
    };
    void* nativeCode = reinterpret_cast<void*>(&testSampleInNativeCodeThatLeavesAddressInLinkRegister);
    void* inInterpreter = LLInt::getCodePtr<JSEntryPtrTag>(llint_program_prologue).untaggedPtr();

    alignas(16) std::array<void*, 48> stack { };
    constexpr size_t lowerRecord = 28;
    constexpr size_t upperRecord = 32;
    constexpr size_t recordOfOuter = 40;
    stack[lowerRecord] = &stack[upperRecord];
    stack[upperRecord] = &stack[recordOfOuter];
    stack[upperRecord + 1] = returnAddressIn(2);
    enum class Answer : bool { IsPassedOver, IsTop };
    auto check = [&](void* linkRegister, void* returnAddressInRecord, Answer answer, size_t stackPointer = 0) {
        stack[lowerRecord + 1] = returnAddressInRecord;
        std::optional<SampledTop> top = findSampledTop(loaded, { &stack[lowerRecord], nativeCode, linkRegister, &stack[stackPointer] }, nullptr, nullptr, Thread::currentSingleton().stack());
        bool isCalledByCompiledCode = loaded.locate(returnAddressInRecord).runsCompiledCode();
        size_t callFrame = answer == Answer::IsTop ? lowerRecord : isCalledByCompiledCode ? upperRecord : recordOfOuter;
        void* pc = answer == Answer::IsTop ? linkRegister : isCalledByCompiledCode ? returnAddressInRecord : returnAddressIn(2);
        AOT_TEST_CHECK(top && top->callFrame == std::bit_cast<CallFrame*>(&stack[callFrame]) && top->pc == pc);
    };

    void* afterOperation = findReturnSiteInStubs(loaded, StubFrameKind::ReturnsAfterOperation);
    void* adapter = findReturnSiteInStubs(loaded, StubFrameKind::Adapter);
    for (void* linkRegister : { returnAddressIn(1), afterOperation, adapter })
        check(linkRegister, nativeCode, Answer::IsPassedOver);

    check(returnAddressIn(1), returnAddressIn(2), Answer::IsTop);
    check(returnAddressIn(1), adapter, Answer::IsTop);
    check(returnAddressIn(1), afterOperation, Answer::IsPassedOver);
    check(returnAddressIn(1), inInterpreter, Answer::IsPassedOver);
    check(afterOperation, returnAddressIn(2), Answer::IsTop);
    check(adapter, inInterpreter, Answer::IsTop);
    check(adapter, inInterpreter, Answer::IsPassedOver, lowerRecord);
    check(adapter, returnAddressIn(2), Answer::IsPassedOver);

    constexpr uint32_t load = 0xf940016b; // ldr x11, [x11]
    constexpr uint32_t additionOfOffsetOfCode = 0x8b2e4a0e; // add x14, x16, w14, uxtw #2
    constexpr uint32_t nearCall = 0x94000001;
    constexpr uint32_t callThroughX11 = 0xd63f0160;
    constexpr uint32_t callThroughX14 = 0xd63f01c0;
    check(returnAddressIn(1), behindInstructionsInStub(load, callThroughX11), Answer::IsPassedOver);
    check(returnAddressIn(1), behindInstructionsInStub(additionOfOffsetOfCode, callThroughX14), Answer::IsTop);
    check(returnAddressIn(1), behindInstructionsInStub(load, nearCall), Answer::IsTop);
    static_cast<uint32_t*>(returnAddressIn(2))[-1] = callThroughX11;
    check(returnAddressIn(1), returnAddressIn(2), Answer::IsTop);
}
#endif

static SUPPRESS_ASAN void testSampleInNativeCodeThatFramePointerDoesNotLeadOutOf()
{
    TestBuild build(5, { .admittedCode = Wire::AdmittedCode::Inspector });
    CodeSketch outer(2);
    outer.withFrame(48);
    outer.plain(0x1002);
    outer.callFunction(1, outer.addSite());
    uint32_t afterIndirectCall = outer.callStub(callValue(1), outer.addSite()) + sizeOfNearCall;
    uint32_t afterCallToNativeCode = outer.callStub(writeBarrier(), outer.addSite()) + sizeOfNearCall;
    outer.end();
    CodeSketch withoutRecord = sketchWithSite(4, 16);
    withoutRecord.code().frame.hasFrameRecord = false;
    withoutRecord.end();
    TestImage image = build.build(bodies(plainBody(1, 32), outer.take(), plainBody(3, 64), withoutRecord.take()));
    const Image& loaded = *image.image;
    const StackBounds& bounds = Thread::currentSingleton().stack();
    auto addressIn = [&](uint32_t function, uint32_t offset = sizeof(uint32_t) + sizeOfNearCall) {
        return const_cast<uint8_t*>(image.address(function, offset));
    };
    void* nativeCode = reinterpret_cast<void*>(&testSampleInNativeCodeThatFramePointerDoesNotLeadOutOf);

    constexpr size_t wordsSearched = 16 * KB / sizeof(void*);
    alignas(16) std::array<void*, wordsSearched + 64> stack;
    constexpr size_t recordOfCaller = wordsSearched + 32;
    constexpr size_t recordOfOuter = recordOfCaller + 2 + 48 / sizeof(void*);
    constexpr size_t entryFrame = recordOfOuter + 8;
    constexpr size_t recordOfNativeCode = recordOfCaller - 32 / sizeof(void*) - 2;
    constexpr size_t stackPointer = recordOfNativeCode - 24;
    auto setRecord = [&](size_t record, size_t callerFrame, void* returnPC) {
        stack[record] = &stack[callerFrame];
        stack[record + 1] = returnPC;
    };
    auto reset = [&] {
        stack.fill(nullptr);
        setRecord(recordOfOuter, entryFrame, nativeCode);
        setRecord(recordOfCaller, recordOfOuter, addressIn(2));
        setRecord(recordOfNativeCode, recordOfCaller, addressIn(1));
    };
    auto find = [&](std::optional<size_t> topCallFrame = std::nullopt, size_t sampledStackPointer = stackPointer, void* framePointer = nullptr) {
        return findSampledTop(loaded, { framePointer, nativeCode, nullptr, &stack[sampledStackPointer] }, topCallFrame ? std::bit_cast<CallFrame*>(&stack[*topCallFrame]) : nullptr, std::bit_cast<EntryFrame*>(&stack[entryFrame]), bounds);
    };
    auto isTop = [&](const std::optional<SampledTop>& top, size_t callFrame, const void* pc, std::optional<size_t> framePointer = std::nullopt) {
        return top && top->callFrame == std::bit_cast<CallFrame*>(&stack[callFrame]) && top->pc == pc && !top->framelessTopCallerPC
            && top->framePointer == (framePointer ? std::bit_cast<CallFrame*>(&stack[*framePointer]) : nullptr);
    };
    auto isCaller = [&](const std::optional<SampledTop>& top) {
        return isTop(top, recordOfCaller, addressIn(1));
    };

    // What called the VM is on the other side of the entry frame.
    stack.fill(nullptr);
    setRecord(recordOfOuter, entryFrame, nativeCode);
    setRecord(entryFrame, entryFrame + 4, nativeCode);
    setRecord(entryFrame + 4, entryFrame + 12, addressIn(2, afterCallToNativeCode));
    AOT_TEST_CHECK(!find(std::nullopt, recordOfOuter - 4, &stack[recordOfOuter]));

    reset();
    if constexpr (!isX86_64()) {
        AOT_TEST_CHECK(!find());
        return;
    }

    for (void* framePointer : { static_cast<void*>(nullptr), std::bit_cast<void*>(static_cast<uintptr_t>(1)), static_cast<void*>(&stack[stackPointer + 4]), nativeCode })
        AOT_TEST_CHECK(isCaller(find(std::nullopt, stackPointer, framePointer)));

    // At an entry frame it only goes by what was stored.
    auto findAtEntryFrame = [&](void* topCallFrame) {
        return findSampledTop(loaded, { &stack[stackPointer], nullptr, nullptr, &stack[stackPointer] }, static_cast<CallFrame*>(topCallFrame), std::bit_cast<EntryFrame*>(&stack[entryFrame]), bounds);
    };
    AOT_TEST_CHECK(isCaller(findAtEntryFrame(&stack[recordOfNativeCode])));
    AOT_TEST_CHECK(!findAtEntryFrame(nullptr));

    // How far it looks.
    AOT_TEST_CHECK(isCaller(find(std::nullopt, recordOfNativeCode - wordsSearched + 1)));
    AOT_TEST_CHECK(!find(std::nullopt, recordOfNativeCode - wordsSearched));
    AOT_TEST_CHECK(isCaller(find(recordOfNativeCode, recordOfNativeCode - wordsSearched)));
    AOT_TEST_CHECK(!find(std::nullopt, recordOfNativeCode + 1));
    AOT_TEST_CHECK(!find(recordOfNativeCode, recordOfNativeCode + 1));

    // What is no record of native code.
    stack[recordOfNativeCode + 1] = addressIn(1, sizeof(uint32_t));
    AOT_TEST_CHECK(!find());
    stack[recordOfNativeCode + 1] = addressIn(3);
    AOT_TEST_CHECK(!find());
    reset();
    stack[recordOfNativeCode] = nullptr;
    AOT_TEST_CHECK(!find());
    reset();
    AOT_TEST_CHECK(isCaller(find(recordOfCaller)));

    // A callee of the caller has returned.
    constexpr size_t recordBelowDeadCallee = recordOfNativeCode - 64 / sizeof(void*) - 2;
    setRecord(recordBelowDeadCallee, recordOfNativeCode, addressIn(3));
    AOT_TEST_CHECK(isCaller(find()));
    AOT_TEST_CHECK(isCaller(find(recordBelowDeadCallee)));

    // So has a function that was in the place of the caller. Only topCallFrame tells which of them.
    reset();
    constexpr size_t recordBelowLargerFrame = recordOfCaller - 64 / sizeof(void*) - 2;
    stack[recordOfCaller + 1] = addressIn(2, afterIndirectCall);
    setRecord(recordBelowLargerFrame, recordOfCaller, addressIn(3));
    AOT_TEST_CHECK(isCaller(find(recordOfNativeCode)));
    AOT_TEST_CHECK(isTop(find(recordBelowLargerFrame), recordOfCaller, addressIn(3)));

    // The engine runs the frame at the top, or that is where native code is by now.
    void* inInterpreter = LLInt::getCodePtr<JSEntryPtrTag>(llint_program_prologue).untaggedPtr();
    void* inOtherCodeOfImage = const_cast<uint8_t*>(loaded.ffiCode().data());
    AOT_TEST_CHECK(loaded.ffiCode().empty() == !FFI_STUBS_SUPPORTED);
    struct Record {
        size_t callerFrame;
        void* returnPC;
        bool isOfEngine;
    };
    for (Record record : std::initializer_list<Record> {
        { recordOfNativeCode, inInterpreter, true },
        { recordOfNativeCode, nullptr, true },
        { entryFrame, nullptr, true },
        { recordOfNativeCode, nativeCode, false },
        { recordOfNativeCode, FFI_STUBS_SUPPORTED ? inOtherCodeOfImage : nativeCode, false },
        { entryFrame, nativeCode, false },
        { entryFrame + 2, nullptr, false },
        { stackPointer + 8, nullptr, false },
    }) {
        reset();
        setRecord(stackPointer + 8, record.callerFrame, record.returnPC);
        AOT_TEST_CHECK(record.isOfEngine ? !find(stackPointer + 8) : isCaller(find(stackPointer + 8)));
        AOT_TEST_CHECK(isCaller(find()));
    }

    // How a run of compiled frames ends.
    void* inEntry = untagCFunctionPtr<void*, CFunctionPtrTag>(vmEntryToJavaScript);
    for (auto [returnPC, endsRun] : { std::pair { nativeCode, false }, { inEntry, false }, { inInterpreter, true }, { inOtherCodeOfImage, !!FFI_STUBS_SUPPORTED }, { g_jscConfig.startExecutableMemory, !!g_jscConfig.startExecutableMemory } }) {
        reset();
        setRecord(recordOfOuter, recordOfOuter + 4, returnPC);
        AOT_TEST_CHECK(endsRun ? isCaller(find()) : !find());
        for (void* entryFrameOfOtherThread : { static_cast<void*>(nullptr), static_cast<void*>(static_cast<uint8_t*>(bounds.origin()) + 1 * MB) })
            AOT_TEST_CHECK(!findSampledTop(loaded, { nullptr, nativeCode, nullptr, &stack[stackPointer] }, nullptr, static_cast<EntryFrame*>(entryFrameOfOtherThread), bounds));
    }
    reset();
    setRecord(recordOfCaller, entryFrame + 2, addressIn(2));
    setRecord(entryFrame + 2, entryFrame, nativeCode);
    AOT_TEST_CHECK(!find());
    reset();
    constexpr size_t recordInLongRun = recordOfOuter - 2 * 300;
    for (size_t record = recordInLongRun; record < recordOfOuter; record += 2)
        setRecord(record, record + 2, addressIn(2));
    setRecord(recordInLongRun - 32 / sizeof(void*) - 2, recordInLongRun, addressIn(1));
    AOT_TEST_CHECK(isTop(find(std::nullopt, recordInLongRun - 16), recordInLongRun, addressIn(1)));

    // A caller without a frame record.
    reset();
    constexpr size_t frameWithoutRecord = recordOfNativeCode;
    constexpr size_t recordBelowIt = frameWithoutRecord - 16 / sizeof(void*) - 2;
    setRecord(recordBelowIt, recordOfCaller, addressIn(4));
    AOT_TEST_CHECK(isCaller(find()));
    reset();
    constexpr size_t recordBelowCalleeOfOuter = recordOfCaller - 16 / sizeof(void*) - 2;
    stack[recordOfNativeCode + 1] = nullptr;
    stack[recordOfCaller] = nullptr;
    stack[recordOfCaller + 1] = addressIn(2, afterIndirectCall);
    setRecord(recordBelowCalleeOfOuter, recordOfOuter, addressIn(4));
    AOT_TEST_CHECK(isTop(find(), recordOfCaller, addressIn(4), recordOfOuter));

    stack[recordOfCaller + 1] = findReturnSiteInStubs(loaded, StubFrameKind::ArgumentList);
    AOT_TEST_CHECK(isTop(find(), recordOfCaller, addressIn(4), recordOfOuter));

    reset();
    setRecord(stackPointer + 8, recordOfNativeCode, findReturnSiteInStubs(loaded, StubFrameKind::Plain));
    AOT_TEST_CHECK(isTop(find(), recordOfNativeCode, findReturnSiteInStubs(loaded, StubFrameKind::Plain)));
    setRecord(stackPointer + 8, recordOfNativeCode, findReturnSiteInStubs(loaded, StubFrameKind::EngineCallee));
    AOT_TEST_CHECK(!find(stackPointer + 8));
    setRecord(stackPointer - 4, recordOfCaller, addressIn(1));
    setRecord(stackPointer + 8, stackPointer - 4, findReturnSiteInStubs(loaded, StubFrameKind::Plain));
    AOT_TEST_CHECK(isCaller(find()));
}

static SUPPRESS_ASAN void testSampleInHostFunction()
{
    TestBuild build(3);
    CodeSketch outer(2);
    outer.withFrame(48);
    outer.plain(0x1002);
    outer.callFunction(1, outer.addSite());
    outer.end();
    CodeSketch caller = sketchWithSite(1, 32);
    uint32_t afterIndirectCall = caller.callStub(callValue(1), caller.addSite()) + sizeOfNearCall;
    caller.end();
    TestImage image = build.build(bodies(caller.take(), outer.take()));
    const Image& loaded = *image.image;
    auto returnAddressIn = [&](uint32_t function) {
        return const_cast<uint8_t*>(image.address(function, sizeof(uint32_t) + sizeOfNearCall));
    };
    void* nativeCode = reinterpret_cast<void*>(&testSampleInHostFunction);
    void* mark = findReturnSiteInStubs(loaded, StubFrameKind::HostCall);
    void* afterCallInFrontOfMark = findReturnSiteInStubs(loaded, StubFrameKind::Plain);
    void* afterHostCall = findReturnSiteInStubs(loaded, StubFrameKind::Plain, mark);
    void* afterCallBehindNextSite = findReturnSiteInStubs(loaded, StubFrameKind::Plain, findReturnSiteInStubs(loaded, StubFrameKind::HostCall, mark));
    AOT_TEST_CHECK(afterCallInFrontOfMark < mark);

    alignas(16) std::array<void*, 64> stack;
    constexpr size_t recordOfCaller = 40;
    constexpr size_t recordOfOuter = recordOfCaller + 2 + 48 / sizeof(void*);
    constexpr size_t entryFrame = recordOfOuter + 8;
    constexpr size_t recordOfStub = recordOfCaller - 32 / sizeof(void*) - 2;
    constexpr size_t frameOfHostCall = recordOfStub - 8;
    constexpr size_t recordOfHostFunction = frameOfHostCall - 2;
    constexpr size_t stackPointer = recordOfHostFunction - 12;
    auto setRecord = [&](size_t record, size_t callerFrame, void* returnPC) {
        stack[record] = &stack[callerFrame];
        stack[record + 1] = returnPC;
    };
    auto reset = [&] {
        stack.fill(nullptr);
        setRecord(recordOfOuter, entryFrame, nativeCode);
        setRecord(recordOfCaller, recordOfOuter, returnAddressIn(2));
        setRecord(recordOfStub, recordOfCaller, returnAddressIn(1));
        setRecord(frameOfHostCall, recordOfStub, mark);
        setRecord(recordOfHostFunction, recordOfStub, afterHostCall);
        setRecord(stackPointer + 2, recordOfHostFunction, nativeCode);
    };
    auto find = [&](const SampledRegisters& registers, CallFrame* topCallFrame) {
        return findSampledTop(loaded, registers, topCallFrame, std::bit_cast<EntryFrame*>(&stack[entryFrame]), Thread::currentSingleton().stack());
    };
    auto isTop = [&](size_t callFrame, const SampledRegisters& registers, CallFrame* topCallFrame) {
        std::optional<SampledTop> top = find(registers, topCallFrame);
        return top && top->callFrame == std::bit_cast<CallFrame*>(&stack[callFrame]) && !top->framelessTopCallerPC && !top->framePointer
            && loaded.locate(top->pc).runsCompiledCode() == (callFrame != frameOfHostCall);
    };
    auto* topCallFrame = std::bit_cast<CallFrame*>(&stack[frameOfHostCall]);
    auto check = [&](size_t expected, CallFrame* topCallFrame) {
        void* returnAddress = stack[recordOfHostFunction + 1];
        AOT_TEST_CHECK(isTop(expected, { &stack[recordOfHostFunction], nativeCode, nullptr, &stack[stackPointer] }, topCallFrame));
        AOT_TEST_CHECK(isTop(expected, { &stack[stackPointer + 2], nativeCode, nullptr, &stack[stackPointer] }, topCallFrame));
        AOT_TEST_CHECK(!isX86_64() || isTop(expected, { nullptr, nativeCode, nullptr, &stack[stackPointer] }, topCallFrame));
        AOT_TEST_CHECK(!isARM64() || isTop(expected, { &stack[recordOfStub], nativeCode, returnAddress, &stack[frameOfHostCall] }, topCallFrame));
    };

    reset();
    check(frameOfHostCall, topCallFrame);
    check(recordOfStub, nullptr);
    check(recordOfStub, std::bit_cast<CallFrame*>(std::bit_cast<uintptr_t>(topCallFrame) + 1));
    check(recordOfStub, std::bit_cast<CallFrame*>(sizeof(void*)));
    stack[frameOfHostCall] = &stack[recordOfCaller];
    check(recordOfStub, topCallFrame);

    // It has returned, and nothing has written over it.
    for (void* returnAddress : { afterCallInFrontOfMark, afterCallBehindNextSite }) {
        reset();
        stack[recordOfHostFunction + 1] = returnAddress;
        check(recordOfStub, topCallFrame);
    }

    // Native code has its frame where another one was.
    reset();
    check(recordOfStub, std::bit_cast<CallFrame*>(&stack[stackPointer + 2]));

    // It is in the place of a function that has returned.
    reset();
    setRecord(frameOfHostCall - 32 / sizeof(void*) - 2, frameOfHostCall, returnAddressIn(1));
    check(frameOfHostCall, topCallFrame);

    reset();
    AOT_TEST_CHECK(isTop(recordOfCaller, { &stack[recordOfStub], nativeCode, nullptr, &stack[stackPointer] }, topCallFrame));

    // It has no frame record. The search does not go past the stub, unless that has returned.
    if (isX86_64()) {
        const SampledRegisters registers { nullptr, nativeCode, nullptr, &stack[stackPointer] };
        stack[recordOfHostFunction] = nullptr;
        AOT_TEST_CHECK(isTop(recordOfCaller, registers, topCallFrame));
        stack[recordOfStub + 1] = const_cast<uint8_t*>(image.address(1, afterIndirectCall));
        setRecord(recordOfOuter, recordOfOuter + 2, findReturnSiteInStubs(loaded, StubFrameKind::ArgumentList));
        setRecord(recordOfOuter + 2, recordOfOuter + 4, afterCallInFrontOfMark);
        setRecord(recordOfOuter + 4, entryFrame, nativeCode);
        AOT_TEST_CHECK(isTop(recordOfOuter + 4, registers, nullptr));
        AOT_TEST_CHECK(!find(registers, topCallFrame));
        stack[recordOfStub] = nullptr;
        AOT_TEST_CHECK(isTop(recordOfOuter + 4, registers, topCallFrame));
    }

    reset();
    setRecord(frameOfHostCall, recordOfStub, findReturnSiteInStubs(loaded, StubFrameKind::Adapter));
    stack[recordOfHostFunction + 1] = findReturnSiteInStubs(loaded, StubFrameKind::Plain, stack[frameOfHostCall + 1]);
    check(recordOfStub, topCallFrame);
}

static SUPPRESS_ASAN void testSampleBelowFrameOfEngine()
{
    TestBuild build(2);
    TestImage image = build.build(bodies(plainBody(1, 32)));
    const Image& loaded = *image.image;
    void* inFunction = const_cast<uint8_t*>(image.address(1, sizeof(uint32_t) + sizeOfNearCall));
    void* inStub = findReturnSiteInStubs(loaded, StubFrameKind::EngineCallee);
    void* nativeCode = reinterpret_cast<void*>(&testSampleBelowFrameOfEngine);
    void* inInterpreter = LLInt::getCodePtr<JSEntryPtrTag>(llint_program_prologue).untaggedPtr();
    void* inEntry = untagCFunctionPtr<void*, CFunctionPtrTag>(vmEntryToJavaScript);
    void* inJITCode = g_jscConfig.startExecutableMemory ? g_jscConfig.startExecutableMemory : inInterpreter;

    alignas(16) std::array<void*, 64> stack;
    constexpr size_t stackPointer = 2;
    constexpr size_t inNativeFrame = 6;
    constexpr size_t recordOfNativeCode = 12;
    constexpr size_t frameOfSlowPath = 14;
    constexpr size_t frameOfEngine = 24;
    constexpr size_t recordOfStub = 34;
    constexpr size_t recordOfFunction = recordOfStub + 2 + 32 / sizeof(void*);
    constexpr size_t entryFrame = recordOfFunction + 8;
    auto setRecord = [&](size_t record, size_t callerFrame, void* returnPC) {
        stack[record] = &stack[callerFrame];
        stack[record + 1] = returnPC;
    };
    auto reset = [&] {
        stack.fill(nullptr);
        setRecord(recordOfFunction, entryFrame, inEntry);
        setRecord(recordOfStub, recordOfFunction, inFunction);
        setRecord(frameOfEngine, recordOfStub, inStub);
        setRecord(recordOfNativeCode, frameOfEngine, inInterpreter);
    };
    auto frameAt = [&](size_t frame) {
        return std::bit_cast<CallFrame*>(&stack[frame]);
    };
    auto isTop = [&](size_t expected, const void* expectedPC, CallFrame* topCallFrame, size_t framePointer, void* pc) {
        std::optional<SampledTop> top = findSampledTop(loaded, { &stack[framePointer], pc, nullptr, &stack[pc ? stackPointer : framePointer] }, topCallFrame, std::bit_cast<EntryFrame*>(&stack[entryFrame]), Thread::currentSingleton().stack());
        return top && top->callFrame == frameAt(expected) && top->pc == expectedPC && !top->framelessTopCallerPC && !top->framePointer;
    };
    // In native code, and on the other side of an entry frame.
    auto check = [&](size_t expected, const void* expectedPC, CallFrame* topCallFrame) {
        AOT_TEST_CHECK(isTop(expected, expectedPC, topCallFrame, recordOfNativeCode, nativeCode));
        AOT_TEST_CHECK(isTop(expected, expectedPC, topCallFrame, recordOfNativeCode, nullptr));
    };

    // Not all of the engine's code runs in a frame of JavaScript. Only topCallFrame tells.
    for (void* returnPC : { inInterpreter, inEntry, inJITCode }) {
        reset();
        stack[recordOfNativeCode + 1] = returnPC;
        check(frameOfEngine, returnPC, frameAt(frameOfEngine));
        check(recordOfStub, inStub, nullptr);
    }

    // A frame that no frame pointer leads to.
    reset();
    for (const void* returnPC : { static_cast<Opcode>(nullptr), LLInt::getOpcode(llint_generic_return_point), LLInt::getOpcodeWide16(llint_generic_return_point), LLInt::getOpcodeWide32(llint_generic_return_point) }) {
        setRecord(frameOfSlowPath, frameOfEngine, const_cast<void*>(returnPC));
        check(frameOfSlowPath, nullptr, frameAt(frameOfSlowPath));
        check(frameOfEngine, inInterpreter, frameAt(frameOfEngine));
    }
    check(recordOfStub, inStub, std::bit_cast<CallFrame*>(std::bit_cast<uintptr_t>(frameAt(frameOfSlowPath)) + 1));
    stack[recordOfNativeCode + 1] = nativeCode;
    check(recordOfStub, inStub, frameAt(frameOfSlowPath));
    reset();
    setRecord(frameOfSlowPath, frameOfEngine, inInterpreter);
    check(recordOfStub, inStub, frameAt(frameOfSlowPath));
    setRecord(frameOfSlowPath, recordOfStub, nullptr);
    check(recordOfStub, inStub, frameAt(frameOfSlowPath));
    for (size_t frame : { inNativeFrame, frameOfEngine + 4 }) {
        setRecord(frame, frameOfEngine, nullptr);
        check(recordOfStub, inStub, frameAt(frame));
    }

    // Native code without a frame record.
    for (void* returnPC : { inStub, inInterpreter, inEntry, inJITCode }) {
        reset();
        stack[frameOfEngine + 1] = returnPC;
        AOT_TEST_CHECK(isTop(frameOfEngine, nativeCode, frameAt(frameOfEngine), frameOfEngine, nativeCode));
    }

    // A tail call may not have written the return address.
    stack[frameOfEngine + 1] = nativeCode;
    AOT_TEST_CHECK(isTop(frameOfEngine, inInterpreter, frameAt(frameOfEngine), frameOfEngine, inInterpreter));
    AOT_TEST_CHECK(isTop(recordOfFunction, inFunction, frameAt(frameOfEngine), frameOfEngine, inEntry));

    // What compiled code calls like an operation has no frame of JavaScript.
    reset();
    setRecord(recordOfNativeCode, recordOfStub, inInterpreter);
    check(recordOfFunction, inFunction, frameAt(recordOfStub));
}

static SUPPRESS_ASAN void testSampleInStub()
{
    if (!isX86_64())
        return;

    TestBuild build(5);
    CodeSketch outer(2);
    outer.withFrame(48);
    outer.plain(0x1002);
    outer.callFunction(1, outer.addSite());
    outer.end();
    CodeSketch caller = sketchWithSite(1, 32);
    uint32_t afterIndirectCall = caller.callStub(callValue(1), caller.addSite()) + sizeOfNearCall;
    caller.end();
    auto bodyWithoutFrame = [](uint32_t function, bool spillsArgumentRegisters) {
        CodeSketch sketch(function);
        sketch.plain(0x1000 + function);
        sketch.callStub(writeBarrier());
        sketch.end();
        sketch.code().frame.spillsArgumentRegisters = spillsArgumentRegisters;
        return sketch.take();
    };
    TestImage image = build.build(bodies(caller.take(), outer.take(), bodyWithoutFrame(3, false), bodyWithoutFrame(4, true)));
    const Image& loaded = *image.image;
    auto addressIn = [&](uint32_t function, uint32_t offset = sizeof(uint32_t) + sizeOfNearCall) {
        return const_cast<uint8_t*>(image.address(function, offset));
    };
    void* inStub = findReturnSiteInStubs(loaded, StubFrameKind::Plain);

    alignas(16) std::array<void*, 48> stack { };
    constexpr size_t recordOfCaller = 32;
    constexpr size_t recordOfOuter = recordOfCaller + 2 + 48 / sizeof(void*);
    constexpr size_t returnAddress = recordOfCaller - 32 / sizeof(void*) - 1;
    stack[recordOfCaller] = &stack[recordOfOuter];
    stack[recordOfCaller + 1] = addressIn(2);
    auto isTop = [&](size_t framePointer, size_t stackPointer, size_t callFrame, const void* pc, bool hasFrameRecord = true) {
        std::optional<SampledTop> top = findSampledTop(loaded, { &stack[framePointer], inStub, nullptr, &stack[stackPointer] }, nullptr, nullptr, Thread::currentSingleton().stack());
        return top && top->callFrame == std::bit_cast<CallFrame*>(&stack[callFrame]) && top->pc == pc && !top->framelessTopCallerPC
            && top->framePointer == (hasFrameRecord ? nullptr : std::bit_cast<CallFrame*>(&stack[framePointer]));
    };

    void* startOfStubs = const_cast<uint8_t*>(loaded.code().data());
    AOT_TEST_CHECK(loaded.locate(startOfStubs).kind == CodeKind::Stub);
    for (void* caller : { addressIn(1, afterIndirectCall), addressIn(1) }) {
        stack[returnAddress] = caller;
        for (void* pushed : { static_cast<void*>(nullptr), static_cast<void*>(&stack[recordOfCaller]), startOfStubs, findReturnSiteInStubs(loaded, StubFrameKind::AtStubEntry) }) {
            stack[returnAddress - 2] = pushed;
            AOT_TEST_CHECK(isTop(recordOfCaller, returnAddress, recordOfCaller, caller));
            AOT_TEST_CHECK(isTop(recordOfCaller, returnAddress - 2, recordOfCaller, caller));
        }
    }

    // A stub that calls a stub has a frame record.
    stack[returnAddress - 2] = inStub;
    AOT_TEST_CHECK(isTop(recordOfCaller, returnAddress - 2, recordOfCaller, inStub));

    stack[returnAddress + 1] = addressIn(2);
    AOT_TEST_CHECK(isTop(recordOfCaller, returnAddress, recordOfCaller, addressIn(1)));
    stack[returnAddress + 1] = nullptr;
    stack[returnAddress - 2] = nullptr;

    // The function has returned, and the frame record is that of a stub which is called like one.
    AOT_TEST_CHECK(isTop(recordOfCaller, returnAddress - 1, recordOfCaller, inStub));
    AOT_TEST_CHECK(isTop(recordOfCaller, returnAddress - 3, recordOfCaller, inStub));

    stack[returnAddress - 1] = &stack[recordOfCaller];
    AOT_TEST_CHECK(isTop(returnAddress - 1, returnAddress - 3, returnAddress - 1, inStub));

    // The caller has no frame.
    stack[returnAddress] = addressIn(1, afterIndirectCall);
    stack[returnAddress - 1] = addressIn(3);
    AOT_TEST_CHECK(isTop(recordOfCaller, returnAddress - 1, returnAddress - 1, addressIn(3), false));
    AOT_TEST_CHECK(isTop(recordOfCaller, returnAddress - 3, returnAddress - 1, addressIn(3), false));

    // It has got a frame record.
    stack[returnAddress - 1] = &stack[recordOfCaller];
    stack[returnAddress - 2] = addressIn(3);
    AOT_TEST_CHECK(isTop(returnAddress - 1, returnAddress - 2, returnAddress - 1, addressIn(3)));

    // It lies below the arguments that it has spilled.
    constexpr size_t movedReturnAddress = returnAddress - sizeOfSpilledArgumentRegisters / sizeof(void*);
    stack.fill(nullptr);
    stack[recordOfCaller] = &stack[recordOfOuter];
    stack[recordOfCaller + 1] = addressIn(2);
    stack[movedReturnAddress] = addressIn(1, afterIndirectCall);
    stack[movedReturnAddress - 1] = addressIn(4);
    AOT_TEST_CHECK(isTop(recordOfCaller, movedReturnAddress - 3, movedReturnAddress - 1, addressIn(4), false));
}

#if CPU(X86_64)
namespace {

class SampledThread {
public:
    enum Function : uint32_t { WithRecord = 1, Outer, OtherWithRecord, WithoutFrame, WithoutRecord, SpillsWithoutFrame, SpillsWithoutRecord, End };

    static constexpr size_t record = 40;
    static constexpr size_t returnToWithRecord = record - 32 / sizeof(void*) - 1;

    explicit SampledThread(std::span<const ArgumentMoves> thunksOfOuter = { })
        : m_build(End)
    {
        auto body = [&](Function function, std::optional<unsigned> frameSize, bool hasFrameRecord = true, bool spillsArgumentRegisters = false) {
            CodeSketch sketch(function);
            if (frameSize)
                sketch.withFrame(*frameSize);
            auto site = [&] {
                return frameSize ? sketch.addSite() : OptionalIndex();
            };
            sketch.plain(0x1000 + function);
            sketch.callStub(writeBarrier(), site());
            sketch.callStub(callValue(1), site());
            AOT_TEST_CHECK(sketch.pinnedBytes(sizeOfRoomForCode) == offsetOfRoomForCode);
            if (function == WithRecord)
                sketch.code().sites.faultingAccesses.append(FaultingAccess { sketch.pinnedBytes(2 * sizeOfNearCall), site(), { 1 } });
            if (function == Outer) {
                for (const ArgumentMoves& moves : thunksOfOuter)
                    sketch.callThunk(moves, site());
                sketch.callFunction(WithRecord, site());
                sketch.callStub(constructValue(ConstructSyntax::NewExpression), site());
            }
            sketch.end();
            sketch.code().frame.hasFrameRecord = hasFrameRecord;
            sketch.code().frame.spillsArgumentRegisters = spillsArgumentRegisters;
            return sketch.take();
        };
        m_image = m_build.build(bodies(body(WithRecord, 32), body(Outer, 48), body(OtherWithRecord, 64), body(WithoutFrame, std::nullopt), body(WithoutRecord, 16, false), body(SpillsWithoutFrame, std::nullopt, true, true), body(SpillsWithoutRecord, 16, false, true)));
        reset();
    }

    void reset(void* returnAddressInRecord = nullptr)
    {
        stack.fill(nullptr);
        stack[record] = &stack[record + 8];
        stack[record + 1] = returnAddressInRecord ? returnAddressInRecord : afterCall(Outer);
        macroScratchRegister = nullptr;
        entryFrame = nullptr;
    }

    uint8_t* afterCallOfStub(Function function) { return address(function, sizeof(uint32_t) + sizeOfNearCall); }
    uint8_t* afterCall(Function function) { return address(function, offsetOfRoomForCode); }
    uint8_t* behindRoomForCode(Function function, unsigned numberOfCalls) { return address(function, offsetOfRoomForCode + sizeOfRoomForCode + numberOfCalls * sizeOfNearCall); }
    void* returnSite(StubFrameKind kind) { return findReturnSiteInStubs(*m_image.image, kind, startOfStubs() + 2 * sizeOfRoomForCode); }

    uint8_t* codeIn(Function function, std::initializer_list<uint8_t> code = { }) { return write(address(function, offsetOfRoomForCode), code); }
    uint8_t* codeInStub(std::initializer_list<uint8_t> code = { }) { return write(startOfStubs(), code); }
    uint8_t* afterCodeInOtherStub(std::initializer_list<uint8_t> code) { return write(startOfStubs() + sizeOfRoomForCode, code) + code.size(); }

    struct Top {
        size_t callFrame;
        const void* pc;
        const void* framelessTopCallerPC { nullptr };
    };

    std::optional<SampledTop> find(void* pc, size_t stackPointer, size_t framePointer = record)
    {
        return findSampledTop(*m_image.image, { &stack[framePointer], pc, nullptr, &stack[stackPointer], macroScratchRegister }, nullptr, static_cast<EntryFrame*>(entryFrame), Thread::currentSingleton().stack());
    }

    bool finds(const Top& expected, void* pc, size_t stackPointer, size_t framePointer = record)
    {
        std::optional<SampledTop> top = find(pc, stackPointer, framePointer);
        return top && top->callFrame == std::bit_cast<CallFrame*>(&stack[expected.callFrame]) && top->pc == expected.pc && top->framelessTopCallerPC == expected.framelessTopCallerPC
            && top->framePointer == (expected.callFrame < record ? std::bit_cast<CallFrame*>(&stack[record]) : nullptr);
    }

    bool findsOwnerOfRecord(void* pc, size_t stackPointer) { return finds({ record, pc }, pc, stackPointer); }

    alignas(16) std::array<void*, 64> stack;
    void* macroScratchRegister;
    void* entryFrame;

private:
    static constexpr uint32_t offsetOfRoomForCode = sizeof(uint32_t) + 2 * sizeOfNearCall;
    static constexpr uint32_t sizeOfRoomForCode = 32;

    uint8_t* address(Function function, uint32_t offset) { return const_cast<uint8_t*>(m_image.address(function, offset)); }
    uint8_t* startOfStubs() { return const_cast<uint8_t*>(m_image.image->code().data()); }

    static uint8_t* write(uint8_t* room, std::initializer_list<uint8_t> code)
    {
        constexpr uint8_t noOperation = 0x90;
        std::span bytes = unsafeMakeSpan(room, sizeOfRoomForCode);
        std::ranges::fill(bytes, noOperation);
        std::ranges::copy(code, bytes.begin() + sizeOfRoomForCode / 2);
        return room + sizeOfRoomForCode / 2;
    }

    TestBuild m_build;
    TestImage m_image;
};

} // anonymous namespace

static SUPPRESS_ASAN void testSampleAtEdgeOfFrame()
{
    using enum SampledThread::Function;
    using Code = std::initializer_list<uint8_t>;
    constexpr size_t record = SampledThread::record;
    constexpr size_t returnAddress = SampledThread::returnToWithRecord;
    SampledThread thread;
    auto& stack = thread.stack;

    stack[returnAddress] = thread.afterCall(WithRecord);
    for (Code code : { Code { }, Code { 0x55, 0x48, 0x89, 0xe5 }, Code { 0xc3 } }) {
        void* pc = thread.codeIn(OtherWithRecord, code);
        AOT_TEST_CHECK(thread.finds({ record, pc, stack[returnAddress] }, pc, returnAddress));
    }
    for (Code code : { Code { 0xc9 }, Code { 0x48, 0x89, 0xec }, Code { 0x5b }, Code { 0x41, 0x54 }, Code { 0x48, 0x83, 0xc4, 0x08 }, Code { 0x48, 0x83, 0xec, 0x08 }, Code { 0x48, 0x81, 0xec, 0x08, 0x01, 0x00, 0x00 } })
        AOT_TEST_CHECK(thread.findsOwnerOfRecord(thread.codeIn(OtherWithRecord, code), returnAddress));
    AOT_TEST_CHECK(thread.findsOwnerOfRecord(thread.codeIn(WithRecord), returnAddress));

    // What only looks like a return address.
    void* pc = thread.codeIn(OtherWithRecord);
    stack[record + 1] = thread.afterCallOfStub(Outer);
    AOT_TEST_CHECK(thread.findsOwnerOfRecord(pc, returnAddress));
    thread.reset();
    stack[returnAddress] = thread.codeIn(WithRecord);
    AOT_TEST_CHECK(thread.findsOwnerOfRecord(pc, returnAddress));
    stack[returnAddress - 2] = thread.afterCall(WithRecord);
    AOT_TEST_CHECK(thread.findsOwnerOfRecord(pc, returnAddress - 2));
    stack[returnAddress] = thread.afterCall(WithRecord);
    stack[returnAddress - 1] = thread.afterCallOfStub(WithoutFrame);
    AOT_TEST_CHECK(thread.findsOwnerOfRecord(pc, returnAddress - 1));

    // A frame without a record that is being set up, or taken down.
    thread.reset();
    stack[returnAddress] = thread.afterCall(WithRecord);
    auto findsFrameWithoutRecord = [&](size_t wordsBelowReturnAddress, Code code) {
        pc = thread.codeIn(WithoutRecord, code);
        return thread.finds({ returnAddress - 1, pc }, pc, returnAddress - wordsBelowReturnAddress);
    };
    AOT_TEST_CHECK(findsFrameWithoutRecord(0, { }));
    AOT_TEST_CHECK(findsFrameWithoutRecord(0, { 0x53, 0x41, 0x54, 0x55 }));
    AOT_TEST_CHECK(findsFrameWithoutRecord(1, { 0x48, 0x83, 0xec, 0x10 }));
    AOT_TEST_CHECK(findsFrameWithoutRecord(1, { 0x48, 0x81, 0xec, 0x10, 0x00, 0x00, 0x00 }));
    AOT_TEST_CHECK(findsFrameWithoutRecord(2, { 0x55 }));
    AOT_TEST_CHECK(findsFrameWithoutRecord(3, { }));
    AOT_TEST_CHECK(findsFrameWithoutRecord(3, { 0x5b, 0x48, 0x83, 0xc4, 0x10 }));
    AOT_TEST_CHECK(findsFrameWithoutRecord(2, { 0x41, 0x5c, 0x5b }));
    AOT_TEST_CHECK(findsFrameWithoutRecord(1, { 0x5b, 0x50 }));
    AOT_TEST_CHECK(findsFrameWithoutRecord(1, { 0x5b, 0x41, 0x5b, 0x48, 0x83, 0xc4, 0x20, 0x41, 0x53 }));
    AOT_TEST_CHECK(thread.findsOwnerOfRecord(thread.codeIn(WithoutRecord), returnAddress - 7));

    // The return address is in a register, and an argument that was not passed looks like one.
    constexpr size_t spilledArguments = returnAddress + 1 - numberOfArgumentGPRs;
    thread.reset();
    thread.macroScratchRegister = stack[spilledArguments + 2] = thread.afterCall(WithRecord);
    pc = thread.codeIn(SpillsWithoutRecord, { 0x41, 0x53 });
    AOT_TEST_CHECK(thread.finds({ record, pc, thread.macroScratchRegister }, pc, spilledArguments));

    constexpr size_t frameOfCaller = returnAddress - 1;
    constexpr size_t movedReturnAddress = frameOfCaller - 16 / sizeof(void*) - numberOfArgumentGPRs - 1;
    thread.reset();
    stack[returnAddress] = thread.afterCall(WithRecord);
    thread.macroScratchRegister = stack[movedReturnAddress] = stack[movedReturnAddress + numberOfArgumentGPRs] = thread.afterCall(WithoutRecord);
    auto findsFrameAboveSpilledArguments = [&](size_t stackPointer, Code code) {
        pc = thread.codeIn(SpillsWithoutFrame, code);
        return thread.finds({ frameOfCaller, pc, thread.macroScratchRegister }, pc, stackPointer);
    };
    AOT_TEST_CHECK(findsFrameAboveSpilledArguments(movedReturnAddress, { }));
    AOT_TEST_CHECK(findsFrameAboveSpilledArguments(movedReturnAddress + 1, { 0x41, 0x53 }));
    AOT_TEST_CHECK(findsFrameAboveSpilledArguments(movedReturnAddress + 1, { 0x48, 0x83, 0xc4, 0x20, 0x41, 0x53 }));

    thread.reset();
    stack[returnAddress] = thread.afterCallOfStub(WithRecord);
    AOT_TEST_CHECK(thread.finds({ record, stack[returnAddress] }, thread.codeInStub({ 0x4c, 0x89, 0x0c, 0x24, 0xc3 }), returnAddress));
    AOT_TEST_CHECK(thread.findsOwnerOfRecord(thread.codeInStub({ 0x4c, 0x89, 0x0c, 0x24 }), returnAddress));
    for (Code code : { Code { 0x50 }, Code { 0x58 } })
        AOT_TEST_CHECK(thread.finds({ record, stack[returnAddress] }, thread.codeInStub(code), returnAddress - 1));
    stack[returnAddress] = thread.behindRoomForCode(WithRecord, 1);
    AOT_TEST_CHECK(thread.finds({ record, stack[returnAddress] }, thread.codeInStub(), returnAddress));

    stack[returnAddress] = thread.afterCall(WithRecord);
    stack[returnAddress - 1] = thread.afterCallOfStub(WithoutFrame);
    AOT_TEST_CHECK(thread.finds({ returnAddress - 1, stack[returnAddress - 1] }, thread.codeInStub({ 0x55, 0x48, 0x89, 0xe5 }), returnAddress - 1));

    // A stub polls at its entry, and it has not spilled what is above its return address.
    constexpr size_t returnToLargerFrame = record - 64 / sizeof(void*) - 1;
    stack[returnToLargerFrame] = thread.afterCallOfStub(OtherWithRecord);
    stack[returnToLargerFrame - 1] = thread.returnSite(StubFrameKind::AtStubEntry);
    stack[returnToLargerFrame - 2] = &stack[record];
    AOT_TEST_CHECK(thread.finds({ returnToLargerFrame - 1, stack[returnToLargerFrame - 1] }, thread.codeInStub(), returnToLargerFrame - 2, returnToLargerFrame - 2));
    stack[returnToLargerFrame] = nullptr;
    stack[returnAddress] = thread.codeIn(WithRecord);
    AOT_TEST_CHECK(thread.finds({ returnToLargerFrame - 1, stack[returnToLargerFrame - 1] }, thread.codeInStub(), returnToLargerFrame - 2, returnToLargerFrame - 2));
}

static SUPPRESS_ASAN void testSampleBelowFrameRecordOfOther()
{
    using enum SampledThread::Function;
    constexpr size_t record = SampledThread::record;
    SampledThread thread;
    auto& stack = thread.stack;
    void* inStub = thread.codeInStub();
    void* nativeCode = reinterpret_cast<void*>(&testSampleBelowFrameRecordOfOther);
    void* inInterpreter = LLInt::getCodePtr<JSEntryPtrTag>(llint_program_prologue).untaggedPtr();

    constexpr size_t returnAddress = SampledThread::returnToWithRecord;
    thread.reset(thread.afterCallOfStub(Outer));
    stack[returnAddress] = thread.afterCallOfStub(WithRecord);
    AOT_TEST_CHECK(thread.findsOwnerOfRecord(inStub, returnAddress));
    AOT_TEST_CHECK(thread.findsOwnerOfRecord(inStub, returnAddress - 2));

    auto findsFunctionCalledAt = [&](void* returnSite, void* returnAddressInRecord, void* pc, size_t bytesBelowRecord = AdapterFrame::saveAreaSize, void* codeBlock = nullptr) {
        size_t frame = record - bytesBelowRecord / sizeof(void*) - 2;
        size_t returnToFunction = frame - 16 / sizeof(void*) - 1;
        thread.reset(returnAddressInRecord);
        stack[record + static_cast<int>(CallFrameSlot::codeBlock)] = codeBlock;
        stack[frame + 1] = returnSite;
        stack[returnToFunction] = thread.afterCallOfStub(WithoutRecord);
        return thread.finds({ frame, stack[returnToFunction] }, pc, returnToFunction - 2);
    };
    for (void* pc : { inStub, nativeCode }) {
        for (StubFrameKind kind : { StubFrameKind::Adapter, StubFrameKind::ConstructAdapter, StubFrameKind::HandlerOfJob }) {
            void* adapter = thread.returnSite(kind);
            AOT_TEST_CHECK(findsFunctionCalledAt(adapter, inInterpreter, pc));
            AOT_TEST_CHECK(findsFunctionCalledAt(adapter, inInterpreter, pc, AdapterFrame::saveAreaSize + 32));
            AOT_TEST_CHECK(findsFunctionCalledAt(adapter, thread.returnSite(StubFrameKind::EngineCallee), pc));
            AOT_TEST_CHECK(!findsFunctionCalledAt(adapter, inInterpreter, pc, AdapterFrame::saveAreaSize - 16));
            AOT_TEST_CHECK(!findsFunctionCalledAt(adapter, inInterpreter, pc, AdapterFrame::saveAreaSize, nativeCode));
            for (void* returnAddressInRecord : { nativeCode, thread.returnSite(StubFrameKind::MethodRead), static_cast<void*>(thread.afterCall(Outer)) })
                AOT_TEST_CHECK(!findsFunctionCalledAt(adapter, returnAddressInRecord, pc));
        }
        for (StubFrameKind kind : { StubFrameKind::ArgumentList, StubFrameKind::PlainFunctionConstruct }) {
            AOT_TEST_CHECK(findsFunctionCalledAt(thread.returnSite(kind), thread.afterCall(Outer), pc, 0));
            AOT_TEST_CHECK(!findsFunctionCalledAt(thread.returnSite(kind), thread.afterCallOfStub(Outer), pc));
        }
    }
    AOT_TEST_CHECK(!findsFunctionCalledAt(thread.returnSite(StubFrameKind::Adapter), inInterpreter, inInterpreter));

    thread.entryFrame = stack[record];
    AOT_TEST_CHECK(!thread.find(nativeCode, record - 2));
    stack[record - 1] = thread.returnSite(StubFrameKind::ConstructAdapterMakesThis);
    AOT_TEST_CHECK(thread.finds({ record, stack[record - 1] }, nativeCode, record - 1));
    AOT_TEST_CHECK(thread.findsOwnerOfRecord(inStub, record - 1));
    stack[record + 1] = nativeCode;
    AOT_TEST_CHECK(!thread.find(nativeCode, record - 1));

    // What the engine has entered is native code, in the place of a function that has returned.
    thread.reset(inInterpreter);
    thread.entryFrame = stack[record];
    stack[returnAddress] = thread.afterCallOfStub(WithRecord);
    stack[returnAddress - 1] = &stack[record];
    stack[returnAddress + 1] = thread.entryFrame;
    AOT_TEST_CHECK(!thread.find(nativeCode, returnAddress - 3, returnAddress + 1));
    stack[returnAddress + 1] = nullptr;
    AOT_TEST_CHECK(thread.finds({ record, stack[returnAddress] }, nativeCode, returnAddress - 3, returnAddress + 1));

    constexpr uint16_t offsetOfCode = AOT_INSTANCE_OFFSET(image.code);
    AOT_TEST_CHECK(findsFunctionCalledAt(thread.afterCodeInOtherStub({ 0x49, 0x03, 0xbd, offsetOfCode & 0xff, offsetOfCode >> 8, 0x00, 0x00, 0xff, 0xd7 }), thread.afterCallOfStub(Outer), inStub));
    AOT_TEST_CHECK(findsFunctionCalledAt(thread.afterCodeInOtherStub({ 0x4d, 0x03, 0x8d, offsetOfCode & 0xff, offsetOfCode >> 8, 0x00, 0x00, 0x41, 0xff, 0xd1 }), thread.afterCallOfStub(Outer), inStub));
    AOT_TEST_CHECK(!findsFunctionCalledAt(thread.afterCodeInOtherStub({ 0x49, 0x03, 0xbd, offsetOfCode & 0xff, offsetOfCode >> 8, 0x00, 0x00, 0xff, 0xd7 }), nativeCode, inStub));
    AOT_TEST_CHECK(!findsFunctionCalledAt(thread.afterCodeInOtherStub({ 0xff, 0xd7 }), thread.afterCallOfStub(Outer), inStub));

    // Frames that cannot be where they would have to be.
    thread.reset();
    stack[record - 4] = thread.returnSite(StubFrameKind::ArgumentList);
    stack[record - 8] = thread.afterCallOfStub(WithoutRecord);
    AOT_TEST_CHECK(thread.findsOwnerOfRecord(inStub, record - 10));
    stack[record - 1] = thread.returnSite(StubFrameKind::ArgumentList);
    stack[record - 2] = thread.afterCallOfStub(SpillsWithoutFrame);
    AOT_TEST_CHECK(thread.findsOwnerOfRecord(inStub, record - 2));
    stack[record - 2] = thread.afterCallOfStub(WithoutFrame);
    AOT_TEST_CHECK(thread.finds({ record - 2, stack[record - 2] }, inStub, record - 2));
    AOT_TEST_CHECK(thread.finds({ record + 8, stack[record + 1] }, nativeCode, record - 2));

    thread.reset();
    stack[returnAddress] = thread.afterCallOfStub(WithRecord);
    stack[returnAddress - 2] = thread.returnSite(StubFrameKind::ArgumentList);
    AOT_TEST_CHECK(thread.finds({ record, stack[returnAddress] }, nativeCode, returnAddress - 4));
    stack[returnAddress - 2] = thread.returnSite(StubFrameKind::PlainFunctionConstructMakesThis);
    AOT_TEST_CHECK(thread.finds({ record + 8, stack[record + 1] }, nativeCode, returnAddress - 4));
    stack[record + 1] = thread.behindRoomForCode(Outer, 1);
    AOT_TEST_CHECK(thread.finds({ record, stack[returnAddress] }, nativeCode, returnAddress - 4));
}

static SUPPRESS_ASAN void testSampleBelowManyFramesWithoutRecord()
{
    TestBuild build(2);
    CodeSketch recursive = sketchWithSite(1, 16);
    uint32_t afterRecursiveCall = recursive.callFunction(1, recursive.addSite()) + sizeOfNearCall;
    recursive.code().frame.hasFrameRecord = false;
    recursive.end();
    TestImage image = build.build(bodies(recursive.take()));
    void* afterCallOfStub = const_cast<uint8_t*>(image.address(1, sizeof(uint32_t) + sizeOfNearCall));
    void* nativeCode = reinterpret_cast<void*>(&testSampleBelowManyFramesWithoutRecord);
    void* inInterpreter = LLInt::getCodePtr<JSEntryPtrTag>(llint_program_prologue).untaggedPtr();

    constexpr size_t framesChecked = 256;
    constexpr size_t wordsInFrame = 16 / sizeof(void*) + 2;
    constexpr size_t returnAddress = 3;
    constexpr size_t record = returnAddress + 1 + (framesChecked + 2) * wordsInFrame;
    alignas(16) std::array<void*, record + 8> stack;
    auto findsCaller = [&](void* pc, size_t numberOfFrames) {
        stack.fill(nullptr);
        stack[record] = &stack[record + 4];
        stack[record + 1] = inInterpreter;
        stack[returnAddress] = afterCallOfStub;
        for (size_t frame = 1; frame <= numberOfFrames; ++frame)
            stack[returnAddress + frame * wordsInFrame] = const_cast<uint8_t*>(image.address(1, afterRecursiveCall));
        stack[returnAddress + wordsInFrame / 2] = findReturnSiteInStubs(*image.image, StubFrameKind::ReturnsAfterOperation);
        std::optional<SampledTop> top = findSampledTop(*image.image, { &stack[record], pc, nullptr, &stack[returnAddress - 2] }, nullptr, nullptr, Thread::currentSingleton().stack());
        return top && top->pc == afterCallOfStub && top->callFrame == std::bit_cast<CallFrame*>(&stack[returnAddress + wordsInFrame - 1]);
    };
    for (void* pc : { static_cast<void*>(const_cast<uint8_t*>(image.image->code().data())), nativeCode }) {
        AOT_TEST_CHECK(findsCaller(pc, framesChecked));
        AOT_TEST_CHECK(!findsCaller(pc, framesChecked - 1));
    }
}

static SUPPRESS_ASAN void testSampleFarBelowFrameRecord()
{
    TestBuild build(3);
    CodeSketch outer = sketchWithSite(2, 48);
    uint32_t afterCall = outer.callFunction(1, outer.addSite()) + sizeOfNearCall;
    outer.end();
    TestImage image = build.build(bodies(plainBody(1, 32), outer.take()));
    void* afterCallOfStub = const_cast<uint8_t*>(image.address(1, sizeof(uint32_t) + sizeOfNearCall));
    void* inStub = const_cast<uint8_t*>(image.image->code().data());

    constexpr size_t wordsSearchedAtEitherEnd = 32 * KB / sizeof(void*);
    constexpr size_t firstWord = 1;
    constexpr size_t record = 3 * wordsSearchedAtEitherEnd;
    alignas(16) std::array<void*, record + 8> stack { };
    stack[record] = &stack[record + 4];
    stack[record + 1] = const_cast<uint8_t*>(image.address(2, afterCall));
    stack[record - 32 / sizeof(void*) - 1] = afterCallOfStub;
    // A return address in a stub puts an end to the search.
    auto isSearched = [&](size_t word, size_t stackPointer = firstWord) {
        SetForScope returnAddress(stack[word], findReturnSiteInStubs(*image.image, StubFrameKind::ReturnsAfterOperation));
        std::optional<SampledTop> top = findSampledTop(*image.image, { &stack[record], inStub, nullptr, &stack[stackPointer] }, nullptr, nullptr, Thread::currentSingleton().stack());
        return top->pc != afterCallOfStub;
    };
    AOT_TEST_CHECK(isSearched(firstWord + wordsSearchedAtEitherEnd - 2));
    AOT_TEST_CHECK(!isSearched(firstWord + wordsSearchedAtEitherEnd));
    AOT_TEST_CHECK(!isSearched(record - wordsSearchedAtEitherEnd - 3));
    AOT_TEST_CHECK(isSearched(record - wordsSearchedAtEitherEnd - 1));
    AOT_TEST_CHECK(isSearched(record - wordsSearchedAtEitherEnd / 4 - 1, firstWord + record / 2));
}

static SUPPRESS_ASAN void testSampleBelowCallThroughThunk()
{
    auto thunk = [](StubKey key, std::initializer_list<std::tuple<GPRReg, ArgumentSource, uint64_t>> list) {
        ArgumentMoves moves;
        moves.append(key.bits());
        moves.append(signatureOf(key).immediate ? hasImmediate : 0);
        for (auto [destination, source, payload] : list)
            appendArgumentMove(moves, destination, source, payload);
        return moves;
    };
    using enum ArgumentSource;
    const ArgumentMoves thunks[] = {
        thunk(writeBarrier(), { { signatureOf(writeBarrier()).operands[0].gpr(), Register, X86Registers::ebx } }),
        thunk(callValue(2), { { calleeGPR, Register, X86Registers::ebx }, { thisGPR, Register, X86Registers::r14 } }),
        thunk(callValue(2), { { argumentGPR(0), Register, argumentGPR(1) }, { argumentGPR(1), Register, argumentGPR(0) } }),
        thunk(callValue(2), { { calleeGPR, Register, thisGPR }, { thisGPR, Register, calleeGPR } }),
        thunk(callValue(2), { { calleeGPR, Stack64, -8 }, { thisGPR, Stack32, -1000 } }),
        thunk(callValue(2), { { calleeGPR, Stack64OffStackPointer, 0 }, { thisGPR, Stack32OffStackPointer, 8 }, { argumentGPR(0), Stack64OffStackPointer, 1000 } }),
        thunk(callMethodById(1), { { thisGPR, Register, X86Registers::ebx } }),
        thunk(callMethodById(1), { { thisGPR, Register, X86Registers::r14 } }),
        thunk(constructWithArray(ConstructSyntax::NewExpression), { { calleeGPR, Register, X86Registers::ebx } }),
        thunk(superConstructWithArray(), { { calleeGPR, Register, X86Registers::ebx } }),
        thunk(callValue(4), { { argumentGPR(0), Constant, 0 }, { argumentGPR(1), Constant, 7 }, { argumentGPR(2), Constant, -7 }, { argumentGPR(3), Constant, JSValue::NumberTag }, { calleeGPR, Constant, 7 } }),
    };
    SampledThread thread(thunks);
    void* inStub = thread.codeInStub();
    for (unsigned index = 0; index < std::size(thunks); ++index) {
        thread.reset(thread.behindRoomForCode(SampledThread::Outer, index + 1));
        void* returnAddress = thread.stack[SampledThread::returnToWithRecord] = thread.afterCallOfStub(SampledThread::WithRecord);
        AOT_TEST_CHECK(thread.finds({ SampledThread::record, index ? returnAddress : inStub }, inStub, SampledThread::returnToWithRecord));
    }
}
#endif

static CompiledCode bodyWithFrameMap(uint32_t function, const Vector<uint32_t>& frameMapWords)
{
    CodeSketch sketch = sketchWithSite(function, 16);
    sketch.code().sites.rareLists.ensure().frameMapWords = frameMapWords;
    sketch.end();
    return sketch.take();
}

static void testFrameMaps()
{
    Vector<uint32_t> firstWords { 1, 2, 3 };
    Vector<uint32_t> secondWords { 4, 5 };

    TestBuild build(4);
    TestImage image = build.build(bodies(bodyWithFrameMap(1, firstWords), bodyWithFrameMap(2, secondWords), bodyWithFrameMap(3, { })));
    const Image& loaded = *image.image;

    AOT_TEST_CHECK(std::ranges::equal(loaded.frameMapWords(image.indexOf(1)), firstWords.span()));
    AOT_TEST_CHECK(std::ranges::equal(loaded.frameMapWords(image.indexOf(2)), secondWords.span()));
    AOT_TEST_CHECK(loaded.frameMapWords(image.indexOf(3)).empty());
}

namespace {

struct ReturnOffsetsAroundFrame {
    uint32_t beforeFrame { 0 };
    uint32_t firstInFrame { 0 };
    uint32_t lastInFrame { 0 };
    uint32_t behindFrame { 0 };
    uint32_t inSecondFrame { 0 };
};

} // anonymous namespace

static uint32_t callAndGetReturnOffset(CodeSketch& sketch)
{
    sketch.callStub(writeBarrier(), sketch.addSite());
    return sketch.size();
}

static CompiledCode bodyWithFramesInTheMiddle(uint32_t function, ReturnOffsetsAroundFrame& offsets)
{
    CodeSketch sketch(function);
    CodeRange first { };
    CodeRange second { };
    sketch.pinned(1, 2);
    offsets.beforeFrame = callAndGetReturnOffset(sketch);
    first.begin = sketch.pinned(2, 3);
    offsets.firstInFrame = callAndGetReturnOffset(sketch);
    sketch.pinned(3, 40);
    offsets.lastInFrame = callAndGetReturnOffset(sketch);
    first.end = sketch.pinned(4, 2);
    offsets.behindFrame = callAndGetReturnOffset(sketch);
    second.begin = sketch.pinned(5, 2);
    offsets.inSecondFrame = callAndGetReturnOffset(sketch);
    second.end = sketch.end();
    sketch.code().frame.frameSizeInBytes = 16;
    sketch.code().frame.frameRanges.appendList({ first, second });
    return sketch.take();
}

static void testHasFrameAtReturnOffsets()
{
    TestBuild build(4);
    ReturnOffsetsAroundFrame offsets;
    TestImage image = build.build(bodies(bodyWithFramesInTheMiddle(1, offsets), plainBody(2), jumpToStubBody(3)));
    const Image& loaded = *image.image;
    uint32_t index = image.indexOf(1);

    AOT_TEST_CHECK(!loaded.hasFrameAtReturnOffset(index, 0));
    AOT_TEST_CHECK(!loaded.hasFrameAtReturnOffset(index, offsets.beforeFrame));
    AOT_TEST_CHECK(loaded.hasFrameAtReturnOffset(index, offsets.firstInFrame));
    AOT_TEST_CHECK(loaded.hasFrameAtReturnOffset(index, offsets.lastInFrame));
    AOT_TEST_CHECK(!loaded.hasFrameAtReturnOffset(index, offsets.behindFrame));
    AOT_TEST_CHECK(loaded.hasFrameAtReturnOffset(index, offsets.inSecondFrame));
    AOT_TEST_CHECK(!loaded.hasFrameAtReturnOffset(image.indexOf(3), codeOffsetUnit));
}

static CompiledCode bodyWithEpilogues(uint32_t function, Vector<uint32_t>& returnOffsets)
{
    constexpr unsigned numberOfEpilogues = 6;
    CodeSketch sketch(function);
    Vector<CodeRange, 1> ranges;
    uint32_t begin = sketch.pinned(1, 2);
    for (unsigned epilogue = 0; epilogue < numberOfEpilogues; ++epilogue) {
        sketch.pinned(2, 50);
        returnOffsets.append(callAndGetReturnOffset(sketch));
        ranges.append(CodeRange { begin, sketch.pinned(3, 2) });
        begin = sketch.size();
    }
    sketch.end();
    sketch.code().frame.frameSizeInBytes = 16;
    sketch.code().frame.frameRanges = WTF::move(ranges);
    return sketch.take();
}

#if CPU(X86_64)
static void checkFrameAtFaultingAccesses(unsigned sizeOfAccess)
{
    static_assert(sizeOfNearCall == 5);
    CodeSketch sketch(1);
    uint32_t accessBeforeFrame = sketch.pinnedBytes(sizeOfAccess);
    uint32_t begin = sketch.pinnedBytes(8);
    uint32_t lastAccessInFrame = sketch.pinnedBytes(sizeOfAccess);
    uint32_t end = sketch.pinnedBytes(8);
    sketch.end();
    sketch.code().frame.frameSizeInBytes = 16;
    sketch.code().frame.frameRanges.append(CodeRange { begin, end });
    for (uint32_t access : { accessBeforeFrame, lastAccessInFrame })
        sketch.code().sites.faultingAccesses.append(FaultingAccess { access, sketch.addSite(), { 1 } });

    TestBuild build(2);
    TestImage image = build.build(bodies(sketch.take()));
    const Image& loaded = *image.image;
    uint32_t index = image.indexOf(1);
    uint32_t returnOffsetBeforeFrame = accessBeforeFrame + sizeOfNearCall;
    uint32_t returnOffsetInFrame = lastAccessInFrame + sizeOfNearCall;

    AOT_TEST_CHECK(!loaded.hasFrameAt(index, accessBeforeFrame));
    AOT_TEST_CHECK(!loaded.hasFrameAtReturnOffset(index, returnOffsetBeforeFrame));
    AOT_TEST_CHECK(loaded.hasFrameAt(index, lastAccessInFrame));
    AOT_TEST_CHECK(loaded.hasFrameAtReturnOffset(index, returnOffsetInFrame));
}

static void checkFrameOfLoneFaultingAccess(unsigned sizeOfAccess)
{
    Vector<uint32_t> returnOffsets;
    CompiledCode other = bodyWithEpilogues(1, returnOffsets);
    other.frame.frameSizeInBytes = 48;

    CodeSketch sketch(2);
    uint32_t begin = sketch.pinnedBytes(1);
    uint32_t access = sketch.pinnedBytes(sizeOfAccess);
    uint32_t end = sketch.pinnedBytes(1);
    sketch.end();
    sketch.code().frame.frameSizeInBytes = 16;
    sketch.code().frame.hasFrameRecord = false;
    sketch.code().frame.frameRanges.append(CodeRange { begin + 1, end });
    sketch.code().sites.faultingAccesses.append(FaultingAccess { access, sketch.addSite(), { 1 } });

    TestBuild build(3);
    TestImage image = build.build(bodies(WTF::move(other), sketch.take()));
    const Image& loaded = *image.image;
    uint32_t index = image.indexOf(2);
    uint32_t returnOffset = access + sizeOfNearCall;

    AOT_TEST_CHECK(loaded.frameOf(image.indexOf(1)).frameSizeInBytes() == 48);
    AOT_TEST_CHECK(loaded.hasFrameAt(index, access));
    AOT_TEST_CHECK(loaded.hasFrameAtReturnOffset(index, returnOffset));
    for (const ImageFrame& frame : { loaded.frameOf(index), loaded.frameAtReturnOffset(index, returnOffset), loaded.returnSiteInfoAt(index, returnOffset).frame }) {
        AOT_TEST_CHECK(frame.frameSizeInBytes() == 16);
        AOT_TEST_CHECK(!frame.hasFrameRecord());
    }
}

static void testFrameAtFaultingAccesses()
{
    for (unsigned sizeOfAccess : { 2u, 3u, 4u }) {
        checkFrameAtFaultingAccesses(sizeOfAccess);
        checkFrameOfLoneFaultingAccess(sizeOfAccess);
    }
}
#endif

static String textOfProgramReadingNames(unsigned numberOfNames)
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
        "entry\n"
        "function f0 name=\"main\" kind=Module results=[{type=anything}] {\n"
        "  b0():\n"
        "    v0 = Builtin builtin=GlobalThis\n"
        "    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"
        "    v2 = NewClosure function=f1\n"
        "    v3 = Call v1, -, v2\n"_s);
    for (unsigned name = 0; name < numberOfNames; ++name)
        text.append("    v"_s, name + 4, " = PropGet v1 key=\"n"_s, name, "\"\n"_s);
    text.append("    Return\n}\n"
        "function f1 name=\"f1\" results=[{type=anything}] {\n  b0():\n    Return\n}\n"_s);
    text.append(runtimeFunctionsText(2));
    return text.toString();
}

namespace {

struct NameNumberInCode {
    PropertyKeyID key;
    uint32_t offset { 0 };
};

} // anonymous namespace

static void testNameNumberPastFifteenBits()
{
    constexpr uint32_t firstNumberPastFifteenBits = 1u << 15;
    constexpr unsigned numberOfNames = firstNumberPastFifteenBits + 1000;

    TestBuild build(textOfProgramReadingNames(numberOfNames), { });
    const Typed::Program& program = build.program();
    const Typed::SlotPlan& slots = build.plan().slots();

    CodeSketch sketch(1);
    MentionList mentions;
    Vector<NameNumberInCode> words;
    unsigned numberOfKeys = program.types.numberOfPropertyKeys();
    for (unsigned index = 0; index < numberOfKeys; ++index) {
        PropertyKeyID key(index);
        if (program.at(key).kind != PropertyKeyKind::Text)
            continue;
        uint32_t offset = sketch.pinned(0);
        sketch.code().body.relocations.append(Relocation { offset, RelocationKind::NumberWord, SymbolSpace::Number, primaryEntry, mentions.add(NumberSpace::Name, key) });
        words.append(NameNumberInCode { key, offset });
    }
    AOT_TEST_CHECK(words.size() >= numberOfNames);
    mentions.moveTo(sketch.code().body);
    sketch.end();
    TestImage image = build.build(bodies(sketch.take()));
    AOT_TEST_CHECK(image.image);

    const ImageText& text = image.image->text();
    const IdentifierNumbering& names = image.built.numbers->identifiers();
    const uint8_t* code = image.address(1, 0);

    unsigned numberOfNamesPastFifteenBits = 0;
    for (const NameNumberInCode& word : words) {
        std::optional<uint32_t> number = names.numberOf(word.key);
        AOT_TEST_CHECK(number);
        uint32_t numberInCode = 0;
        memcpy(&numberInCode, code + word.offset, sizeof(numberInCode));
        AOT_TEST_CHECK(numberInCode == *number);
        if (slots.keysWithColumn().get(word.key.index()))
            AOT_TEST_CHECK(*number < names.countWithLookupTableColumn() && RowDisplacementTable::hasColumn(*number));
        if (*number < firstNumberPastFifteenBits)
            continue;

        ++numberOfNamesPastFifteenBits;
        AOT_TEST_CHECK(names.at(*number).key == word.key);
        AOT_TEST_CHECK(*number < text.numberOfIdentifiers());
        AOT_TEST_CHECK(text.identifier(*number).text() == StringID(image.built.numbers->stringNumbers().requiredNumberOf(program.at(word.key).text)));
    }
    AOT_TEST_CHECK(numberOfNamesPastFifteenBits >= numberOfNames - firstNumberPastFifteenBits);
}

static constexpr uint32_t numberOfVariedFunctions = 9000;
static constexpr uint32_t closuresHandedToOneCall = 50;
static constexpr std::array<unsigned, 3> threadCountsOfFunctionTables { 1, 2, 16 };
static constexpr std::array flagsOfVariedFunctions { "IsSloppy"_s, "IsHiddenInStackTraces"_s, "HasExplicitName"_s, "IsNamedByComputedKey"_s };
static constexpr std::array syntaxOfVariedFunctions { "IsEvaluatedScript"_s, "IsShownAsNativeCode"_s, "IsShownInHelperModule"_s };
static constexpr uint32_t numberOfWrittenForms = (1u << flagsOfVariedFunctions.size()) * 2 * 2 * 3;
static constexpr uint32_t lengthThatFitsNoByte = 300;
static constexpr unsigned numberOfCapturesThatFitNoHalfByte = 16;
static_assert(numberOfVariedFunctions > 2 * indicesInRangeOfLoop);
static_assert(numberOfWrittenForms / 2 * 3 > unsharedFunctionRecord);

static bool variedFunctionHasName(uint32_t function) { return function % 3; }
static bool variedFunctionHasCode(uint32_t function) { return function % 11 != 3; }
static bool variedFunctionHasEntry(uint32_t function) { return variedFunctionHasCode(function) && function % 13 != 5; }
static bool variedFunctionHasCoroutineBody(uint32_t function) { return variedFunctionHasCode(function) && function % 17 == 7; }

static uint32_t lengthOfVariedFunction(uint32_t function)
{
    if (!(function % 89))
        return lengthThatFitsNoByte;
    if (function % 97 == 1)
        return countsAreListed;
    return function % 7;
}

static unsigned numberOfCapturesOfVariedFunction(uint32_t function)
{
    if (!(function % 83))
        return numberOfCapturesThatFitNoHalfByte;
    if (!(function % 7))
        return 2;
    return function % 5 ? 0 : 1;
}

static void appendNamesInBrackets(StringBuilder& text, ASCIILiteral attribute, std::span<const ASCIILiteral> names, uint32_t namesToAppend)
{
    if (!namesToAppend)
        return;
    text.append(' ', attribute, "=["_s);
    bool isFirst = true;
    for (size_t index = 0; index < names.size(); ++index) {
        if (!(namesToAppend >> index & 1))
            continue;
        text.append(isFirst ? ""_s : ", "_s, names[index]);
        isFirst = false;
    }
    text.append(']');
}

static void appendVariedFunction(StringBuilder& text, uint32_t function)
{
    uint32_t form = function % numberOfWrittenForms;
    uint32_t flags = form % (1u << flagsOfVariedFunctions.size());
    form >>= flagsOfVariedFunctions.size();
    uint32_t syntax = form & 1;
    bool isArrow = form & 2;
    if (uint32_t shownAs = form >> 2)
        syntax |= 1u << shownAs;

    text.append("function f"_s, function);
    if (variedFunctionHasName(function))
        text.append(" name=\"f"_s, function, '"');
    text.append(" module=m0"_s, isArrow ? " kind=Arrow"_s : ""_s);
    appendNamesInBrackets(text, "flags"_s, flagsOfVariedFunctions, flags);
    if (uint32_t length = lengthOfVariedFunction(function))
        text.append(" length="_s, length);
    appendNamesInBrackets(text, "syntax"_s, syntaxOfVariedFunctions, syntax);
    unsigned numberOfCaptures = numberOfCapturesOfVariedFunction(function);
    if (numberOfCaptures) {
        text.append(" captures=["_s);
        for (unsigned capture = 0; capture < numberOfCaptures; ++capture) {
            text.append(capture ? ", "_s : ""_s, "{type=anything"_s);
            if (!(capture % 2))
                text.append(" variableName=\"kept"_s, capture, '"');
            text.append('}');
        }
        text.append(']');
    }
    text.append(" results=[{type=anything}] {\n  b0("_s);
    for (unsigned capture = 0; capture < numberOfCaptures; ++capture)
        text.append(capture ? ", v"_s : "v"_s, capture, ": anything"_s);
    text.append("):\n    Return\n}\n"_s);
}

static String textOfVariedFunctions()
{
    StringBuilder text;
    text.append("module m0 path=\"main.js\" displayPath=\"main.js\" body=f0\n"
        "entry module=m0\n"
        "function f0 name=\"main\" module=m0 kind=Module results=[{type=anything}] {\n"
        "  b0():\n"
        "    v0 = Builtin builtin=GlobalThis\n"
        "    v1 = PropGet v0 key=\"sink\" [isFreeName]\n"_s);
    for (uint32_t function = 1; function <= numberOfVariedFunctions; ++function) {
        text.append("    v"_s, function + 1, " = NewClosure"_s);
        for (unsigned capture = 0; capture < numberOfCapturesOfVariedFunction(function); ++capture)
            text.append(capture ? ", v1"_s : " v1"_s);
        text.append(" function=f"_s, function, '\n');
    }
    uint32_t nextValue = numberOfVariedFunctions + 2;
    for (uint32_t first = 1; first <= numberOfVariedFunctions; first += closuresHandedToOneCall) {
        text.append("    v"_s, nextValue++, " = Call v1, -"_s);
        for (uint32_t function = first; function < first + closuresHandedToOneCall && function <= numberOfVariedFunctions; ++function)
            text.append(", v"_s, function + 1);
        text.append('\n');
    }
    text.append("    Return\n}\n"_s);
    for (uint32_t function = 1; function <= numberOfVariedFunctions; ++function)
        appendVariedFunction(text, function);
    text.append(runtimeFunctionsText(numberOfVariedFunctions + 1));
    return text.toString();
}

static Vector<CompiledCode> codeOfVariedFunctions(const TestBuild& build)
{
    Vector<CompiledCode> code;
    for (uint32_t function = 1; function <= numberOfVariedFunctions; ++function) {
        if (variedFunctionHasCode(function))
            code.append(plainBody(function));
        if (variedFunctionHasEntry(function))
            code.append(plainCodeOf(build.genericEntrypointOf(function)));
        if (variedFunctionHasCoroutineBody(function))
            code.append(plainCodeOf({ FunctionID(function), Typed::CodeRole::CoroutineBody }));
    }
    return code;
}

static bool lieInSeveralRangesOfLoop(uint32_t firstFunction, uint32_t lastFunction)
{
    return firstFunction / indicesInRangeOfLoop != lastFunction / indicesInRangeOfLoop;
}

static void checkTablesOfVariedFunctions(const TestBuild& build, const TestImage& image, const BodyFactsForTables& bodyFacts)
{
    const Typed::Program& program = build.program();
    const Typed::CallingConventions& conventions = build.plan().conventions();
    const ImageNumbers& numbers = *image.built.numbers;
    const FunctionNumbers& functions = numbers.functions();
    const ProgramTables& tables = image.image->programTables();
    const ClosureFunctionRangeData& closureFunctions = tables.closureFunctions;
    AOT_TEST_CHECK(tables.numberOfFunctions() == functions.numberOfFunctions());
    AOT_TEST_CHECK(closureFunctions.first == functions.firstClosureFunction().index() && closureFunctions.end == functions.endOfClosureFunctions().index());
    AOT_TEST_CHECK(closureFunctions.end < functions.numberOfFunctions());

    size_t numberOfNames = 0;
    size_t numberWithLargeCounts = 0;
    size_t numberThatOnlyReturn = 0;
    Vector<NamedCaptureData> namedCaptures;
    for (uint32_t index = 0; index < functions.numberOfFunctions(); ++index) {
        CodeUnit unit = index < functions.firstDerived() ? bodyUnitOf(index) : conventions.unitAt(functions.unitOf(FunctionID(index)));
        uint32_t source = unit.function.index();
        const Typed::Function& function = program.at(unit.function);
        FunctionTraceRecord trace = Typed::functionTraceOf(program, conventions, unit);
        FunctionRecordData record { build.plan().functionKind(program, Typed::isGenericEntry(unit.role) ? unit : conventions.closureEntryOf(unit.function)), trace };
        AOT_TEST_CHECK(std::bit_cast<uint32_t>(tables.recordOfFunction(index)) == std::bit_cast<uint32_t>(record));
        AOT_TEST_CHECK(tables.sourceFunctionOf(index) == source);
        AOT_TEST_CHECK(tables.builtinImplementedBy(index).has_value() == trace.implements().has_value());

        bool isShownByName = source && source <= numberOfVariedFunctions && variedFunctionHasCode(source);
        std::optional<uint32_t> name = isShownByName && function.name && function.name != program.strings.emptyString() ? numbers.stringNumbers().numberOf(function.name) : std::nullopt;
        AOT_TEST_CHECK(tables.nameOfFunction(index) == name);
        numberOfNames += name && index < functions.firstDerived();

        if (index < closureFunctions.first || index >= closureFunctions.end)
            continue;
        AOT_TEST_CHECK(unit == conventions.closureEntryOf(unit.function));
        uint32_t closureNumber = tables.closureNumberOf(index);
        Typed::ClosureCaptureCounts counts = Typed::closureCaptureCountsOf(program, conventions, unit.function);
        CaptureCounts countsInTable = tables.captureCountsOf(index);
        AOT_TEST_CHECK(countsInTable.references == counts.references && countsInTable.captures == counts.captures);
        AOT_TEST_CHECK(tables.functionLengthOf(index) == lengthOfVariedFunction(source));
        numberWithLargeCounts += lengthOfVariedFunction(source) >= countsAreListed || countsInTable.encoded() == countsAreListed;
        AOT_TEST_CHECK(std::bit_cast<uint16_t>(tables.kindOfClosureNumber(closureNumber)) == std::bit_cast<uint16_t>(record.kind));
        AOT_TEST_CHECK(SparseNumberTable(tables.closureCells()).at(closureNumber) == numbers.closureCellNumber(unit.function));

        AOT_TEST_CHECK(!conventions.genericEntry(unit).constructEntry);
        bool onlyReturns = bodyFacts.functionsThatOnlyReturn.get(source) && record.kind.isConstructiblePlainFunction();
        AOT_TEST_CHECK(tables.closureNumberOnlyReturns(closureNumber) == onlyReturns);
        numberThatOnlyReturn += onlyReturns;

        size_t firstOfFunction = namedCaptures.size();
        for (unsigned capture = 0; capture < function.captures.size(); ++capture) {
            StringID variableName = function.captures[capture].variableName;
            if (variableName && Typed::holdsCells(Typed::closureCaptureRepresentationOf(program, conventions, unit.function, capture)))
                namedCaptures.append({ Number24(closureNumber), static_cast<uint8_t>(Typed::closureCaptureSlotOf(program, conventions, unit.function, capture)), numbers.stringNumbers().requiredNumberOf(variableName) });
        }
        std::ranges::stable_sort(namedCaptures.mutableSpan().subspan(firstOfFunction), { }, &NamedCaptureData::slot);
    }

    AOT_TEST_CHECK(tables.functionNames().size() == numberOfNames && numberOfNames > indicesInRangeOfLoop);
    AOT_TEST_CHECK(tables.functionsWithLargeCounts().size() == numberWithLargeCounts);
    AOT_TEST_CHECK(tables.constructEntrypoints().size() == numberThatOnlyReturn);
    Vector<NamedCaptureData> namedCapturesInTable;
    AOT_TEST_CHECK(forEachNamedCapture(decodeVarintBytes(tables.namedCaptures()).span(), [&](const NamedCaptureData& row) {
        namedCapturesInTable.append(row);
    }));
    AOT_TEST_CHECK(equalSpans(asBytes(namedCapturesInTable.span()), asBytes(namedCaptures.span())));

    AOT_TEST_CHECK(tables.functionRecords().size() == unsharedFunctionRecord);
    AOT_TEST_CHECK(!tables.unsharedFunctionRecords().empty() && !tables.functionsWithLargeCounts().empty() && !tables.constructEntrypoints().empty() && !namedCaptures.isEmpty());
    AOT_TEST_CHECK(lieInSeveralRangesOfLoop(tables.unsharedFunctionRecords().front().function, tables.unsharedFunctionRecords().back().function));
    AOT_TEST_CHECK(lieInSeveralRangesOfLoop(tables.functionsWithLargeCounts().front().function, tables.functionsWithLargeCounts().back().function));
    AOT_TEST_CHECK(lieInSeveralRangesOfLoop(closureFunctions.first + tables.constructEntrypoints().front().function, closureFunctions.first + tables.constructEntrypoints().back().function));
    AOT_TEST_CHECK(lieInSeveralRangesOfLoop(closureFunctions.first + namedCaptures.first().closureNumber, closureFunctions.first + namedCaptures.last().closureNumber));
}

static std::span<const uint8_t> partOfProgramTables(const TestImage& image, TablePartID id)
{
    uint64_t end = 0;
    for (unsigned part = 0; part < static_cast<unsigned>(id); ++part)
        end = roundUpToMultipleOf<8>(end) + image.built.bytesOfTableParts[part];
    const uint8_t* tables = reinterpret_cast<const uint8_t*>(&image.image->programTables());
    return { tables + roundUpToMultipleOf<8>(end), static_cast<size_t>(image.built.bytesOfTableParts[static_cast<unsigned>(id)]) };
}

static Vector<uint8_t> encodeTablesOfVariedFunctions(unsigned threadCount)
{
    AOT_TEST_CHECK(!ParallelLoops::isInsideLoop());
    TestBuild build(textOfVariedFunctions(), { .threadCount = threadCount });
    TestImage image = build.build(codeOfVariedFunctions(build));
    AOT_TEST_CHECK(image.image);
    BuildContext& context = build.context();
    BodyFactsForTables bodyFacts = collectBodyFactsForTables(build.program(), build.plan(), image.built.functions, context);
    checkTablesOfVariedFunctions(build, image, bodyFacts);

    TableInputs tableInputs { build.program(), build.facts(), build.plan(), image.built.functions, bodyFacts, context.options(), &context };
    TableEncoder encoder(tableInputs, *image.built.numbers, nullptr, TablePartID::Functions, context);
    encodeFunctionProgramTables(encoder);
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());
    std::span<const uint8_t> part = encoder.part.bytesAt(0, encoder.part.size());
    AOT_TEST_CHECK(equalSpans(part, partOfProgramTables(image, TablePartID::Functions)));

    NamesOfTables names(build.program());
    nameWhatFunctionTablesName(tableInputs, GatheredMentions { }, names);
    const ProgramTables& tables = image.image->programTables();
    BitVector numbersOfNames;
    for (size_t string : names.strings)
        numbersOfNames.set(image.built.numbers->stringNumbers().requiredNumberOf(StringID(static_cast<uint32_t>(string))));
    for (Number24 name : tables.functionNames())
        AOT_TEST_CHECK(numbersOfNames.get(name));
    AOT_TEST_CHECK(forEachNamedCapture(decodeVarintBytes(tables.namedCaptures()).span(), [&](const NamedCaptureData& row) {
        AOT_TEST_CHECK(numbersOfNames.get(row.name));
    }));

    Vector<uint8_t> bytes(part);
    for (size_t string : names.strings) {
        uint32_t number = static_cast<uint32_t>(string);
        bytes.append(asByteSpan(number));
    }
    return bytes;
}

static void checkNamedVariablesAsBytes(const Vector<Vector<VariableData>>& modules, size_t maxSize)
{
    NamedVariablesWriter writer;
    size_t numberOfRows = 0;
    auto numberOf = [](size_t place) {
        return static_cast<uint32_t>(3 * place);
    };
    auto closureNumbersOf = [](size_t place) {
        uint32_t first = static_cast<uint32_t>(1000 - 10 * place);
        return place % 2 ? Vector<uint32_t> { } : Vector<uint32_t> { first, first + 1, first + 2, first + 5 };
    };
    for (const Vector<VariableData>& module : modules) {
        size_t place = &module - modules.begin();
        writer.beginModule(numberOf(place), module.size());
        for (const VariableData& variable : module)
            writer.append(variable);
        writer.endModule(closureNumbersOf(place).span());
        numberOfRows += module.size();
    }
    AOT_TEST_CHECK(writer.bytes().size() <= maxSize);
    Vector<Vector<VariableData>> read;
    Vector<Vector<uint32_t>> closureNumbersRead;
    AOT_TEST_CHECK(forEachNamedVariable(writer.bytes().span(), [&](uint32_t module, size_t numberOfVariables) {
        AOT_TEST_CHECK(read.size() < modules.size() && module == numberOf(read.size()) && numberOfVariables == modules[read.size()].size());
        read.append(Vector<VariableData> { });
        closureNumbersRead.append(Vector<uint32_t> { });
    }, [&](const VariableData& variable) {
        read.last().append(variable);
    }, [&](uint32_t first, uint32_t length) {
        for (uint32_t number = first; number < first + length; ++number)
            closureNumbersRead.last().append(number);
    }));
    AOT_TEST_CHECK(read.size() == modules.size());
    for (size_t module = 0; module < modules.size(); ++module) {
        AOT_TEST_CHECK(equalSpans(asBytes(read[module].span()), asBytes(modules[module].span())));
        AOT_TEST_CHECK(equalSpans(asBytes(closureNumbersRead[module].span()), asBytes(closureNumbersOf(module).span())));
    }
    for (size_t size = 0; size < writer.bytes().size(); ++size) {
        size_t numberOfRowsRead = 0;
        size_t numberOfModulesRead = 0;
        bool isWhole = forEachNamedVariable(writer.bytes().span().first(size), [&](uint32_t, size_t) {
            ++numberOfModulesRead;
        }, [&](const VariableData&) {
            ++numberOfRowsRead;
        }, [](uint32_t, uint32_t) { });
        AOT_TEST_CHECK(!isWhole || numberOfRowsRead < numberOfRows || numberOfModulesRead < modules.size());
    }
}

static void checkImageFunctions(const Vector<uint32_t>& numbers, const Vector<uint32_t>& sideTables)
{
    Vector<uint8_t> bytes = ImageFunctions::encode(sideTables.span(), numbers.span());
    auto words = spanReinterpretCast<const uint32_t>(bytes.span());
    AOT_TEST_CHECK(ImageFunctions::hasSize(words, numbers.size()) && !ImageFunctions::hasSize(words, numbers.size() + 1));
    ImageFunctions section(words.data(), static_cast<uint32_t>(numbers.size()));
    for (uint32_t index = 0; index < numbers.size(); ++index)
        AOT_TEST_CHECK(section.numberOf(index) == numbers[index] && section.sideTableOf(index) == sideTables[index]);
}

static void checkImageFunctions(const Vector<uint32_t>& numbers)
{
    auto sideTables = [&](const auto& place) {
        return Vector<uint32_t>(numbers.size(), place);
    };
    checkImageFunctions(numbers, sideTables([](size_t index) { return static_cast<uint32_t>(1000 * index); }));
    checkImageFunctions(numbers, sideTables([](size_t index) { return static_cast<uint32_t>(40000 * index); }));
    checkImageFunctions(numbers, sideTables([](size_t index) { return static_cast<uint32_t>(index % 3 ? 70 : 9 * index); }));
}

static void testImageFunctions()
{
    checkImageFunctions({ 5 });
    checkImageFunctions(Vector<uint32_t>(100, [](size_t index) { return static_cast<uint32_t>(index); }));
    checkImageFunctions(Vector<uint32_t>(100, [](size_t index) { return static_cast<uint32_t>(200 - index); }));
    checkImageFunctions(Vector<uint32_t>(40000, [](size_t index) { return static_cast<uint32_t>(index); }));
    checkImageFunctions(Vector<uint32_t>(97, [](size_t index) { return static_cast<uint32_t>(index + index / 5 * 1000); }));
}

static void checkSourceFunctions(const Vector<uint32_t>& functions, size_t maxWords)
{
    Vector<uint32_t> words = AscendingNumberTable::create(functions.span());
    AOT_TEST_CHECK(words.size() <= maxWords);
    for (uint32_t index = 0; index < functions.size(); ++index)
        AOT_TEST_CHECK(AscendingNumberTable(words.span()).at(index) == functions[index]);
}

static void testSourceFunctions()
{
    checkSourceFunctions({ }, 0);
    checkSourceFunctions({ 7 }, 2);
    checkSourceFunctions(Vector<uint32_t>(40, [](size_t) { return 9u; }), 4 + 2);
    checkSourceFunctions(Vector<uint32_t>(1000, [](size_t index) { return static_cast<uint32_t>(index); }), 64 + 63);
    checkSourceFunctions(Vector<uint32_t>(1000, [](size_t index) { return static_cast<uint32_t>(index / 3 + index / 50 * 40); }), 64 + 66);
    checkSourceFunctions(Vector<uint32_t>(70, [](size_t index) { return static_cast<uint32_t>(index < 40 ? index * 16 : 3000 - index); }), 6 + 70);
    checkSourceFunctions(Vector<uint32_t>(70, [](size_t index) { return static_cast<uint32_t>(index * 17); }), 6 + 70);
    checkSourceFunctions(Vector<uint32_t>(96, [](size_t index) { return static_cast<uint32_t>(index + (index < 50 ? 0 : 1 << 20)); }), 6 + 2 + 32 + 2);
}

static void checkClosureCells(const Vector<uint32_t>& cells, size_t maxWords)
{
    Vector<uint32_t> words = SparseNumberTable::create(cells.span());
    AOT_TEST_CHECK(words.size() <= maxWords);
    for (uint32_t number = 0; number < cells.size(); ++number)
        AOT_TEST_CHECK(SparseNumberTable(words.span()).at(number) == cells[number]);
}

static void testClosureCells()
{
    checkClosureCells({ }, 0);
    checkClosureCells({ 0, 1, 0, 2 }, 2);
    checkClosureCells(Vector<uint32_t>(100, [](size_t index) { return static_cast<uint32_t>(index + 1); }), 8);
    checkClosureCells(Vector<uint32_t>(100, [](size_t index) { return index % 3 ? 0 : static_cast<uint32_t>(index / 3 + 1); }), 8);
    checkClosureCells(Vector<uint32_t>(100, [](size_t index) { return index % 2 ? 0 : static_cast<uint32_t>(101 - index); }), 8 + 50);
    checkClosureCells(Vector<uint32_t>(64, [](size_t index) { return index < 32 ? 5 + static_cast<uint32_t>(index) : static_cast<uint32_t>(64 - index); }), 4 + 32);
}

static void testNamedVariablesAsBytes()
{
    auto inStatics = [](uint32_t name, uint32_t offset, SlotRepresentation representation = SlotRepresentation::JSValue) {
        return VariableData { name, SlotData::create(offset, representation) };
    };
    auto closureOf = [](uint32_t name, uint32_t function) {
        return VariableData { name, SlotData::create(function, SlotRepresentation::DeferredClosure) };
    };
    checkNamedVariablesAsBytes({ }, 0);
    checkNamedVariablesAsBytes({ { inStatics(1, 0) } }, 4 + 6);
    checkNamedVariablesAsBytes({ { inStatics(VariableData::noName - 1, SlotData::maxOffset - 7) } }, 2 + 5 + 4 + 6);
    checkNamedVariablesAsBytes({ { }, { }, { inStatics(1, 0) }, { } }, 2 + 6 + 3 + 4 + 5 + 3);
    Vector<VariableData> neighbors;
    for (uint32_t index = 0; index < 50; ++index)
        neighbors.append(inStatics(600000 + index, 80000 + 8 * (index ^ 1)));
    checkNamedVariablesAsBytes({ neighbors }, 2 + 3 + 3 + 2 * 49 + 6);
    checkNamedVariablesAsBytes({
        { inStatics(5, 64), closureOf(6, 12345), inStatics(7, 72), closureOf(9, 12347) },
        { closureOf(2, 3), inStatics(3, 4, SlotRepresentation::Cell32), inStatics(3, 8, SlotRepresentation::Cell32), inStatics(400, 16, SlotRepresentation::Cell) },
        { inStatics(1, 24, SlotRepresentation::Cell) },
    }, 64 + 3 + 6 + 1 + 5);

    auto isRefused = [](std::initializer_list<uint8_t> bytes) {
        Vector<uint8_t> copy(std::span<const uint8_t>(bytes.begin(), bytes.size()));
        return !forEachNamedVariable(copy.span(), [](uint32_t, size_t) { }, [](const VariableData&) { }, [](uint32_t, uint32_t) { });
    };
    AOT_TEST_CHECK(!isRefused({ 4, 0, 0 }));
    AOT_TEST_CHECK(isRefused({ 0 }));
    AOT_TEST_CHECK(isRefused({ 0, 0 }));
    AOT_TEST_CHECK(isRefused({ 4, 0, 0, 0, 0, 0 }));
    AOT_TEST_CHECK(isRefused({ 0, 0, 1, 1, 1 }));
    AOT_TEST_CHECK(isRefused({ 0, 0, 1, 2, 0 }));
    AOT_TEST_CHECK(isRefused({ 0, 1, 1, 2, 0 }));
    AOT_TEST_CHECK(isRefused({ 0, 1, 1, 1, 0x7f, 0 }));
    AOT_TEST_CHECK(isRefused({ 0, 1, 0xff, 0xff, 0xff, 0xff, 0x0f, 0, 0 }));
}

static void testFunctionTablesOverSeveralRanges()
{
    Vector<uint8_t> expected = encodeTablesOfVariedFunctions(threadCountsOfFunctionTables[0]);
    for (unsigned threadCount : std::span(threadCountsOfFunctionTables).subspan(1))
        AOT_TEST_CHECK(encodeTablesOfVariedFunctions(threadCount) == expected);
}

std::span<const NativeTest> imageFunctionTableTests()
{
    static constexpr NativeTest tests[] = {
        { "image/functions/image-function-of"_s, testImageFunctionOf },
        { "image/functions/records"_s, testRecords },
        { "image/functions/sample-in-native-code"_s, testSampleInNativeCode },
#if CPU(ARM64)
        { "image/functions/sample-in-native-code-that-leaves-an-address-in-the-link-register"_s, testSampleInNativeCodeThatLeavesAddressInLinkRegister },
#endif
        { "image/functions/sample-in-native-code-that-the-frame-pointer-does-not-lead-out-of"_s, testSampleInNativeCodeThatFramePointerDoesNotLeadOutOf },
        { "image/functions/sample-in-host-function"_s, testSampleInHostFunction },
        { "image/functions/sample-below-frame-of-engine"_s, testSampleBelowFrameOfEngine },
        { "image/functions/sample-in-stub"_s, testSampleInStub },
#if CPU(X86_64)
        { "image/functions/sample-at-edge-of-frame"_s, testSampleAtEdgeOfFrame },
        { "image/functions/sample-below-frame-record-of-other"_s, testSampleBelowFrameRecordOfOther },
        { "image/functions/sample-below-many-frames-without-record"_s, testSampleBelowManyFramesWithoutRecord },
        { "image/functions/sample-far-below-frame-record"_s, testSampleFarBelowFrameRecord },
        { "image/functions/sample-below-call-through-thunk"_s, testSampleBelowCallThroughThunk },
#endif
        { "image/functions/frame-maps"_s, testFrameMaps },
        { "image/functions/has-frame-at-return-offsets"_s, testHasFrameAtReturnOffsets },
#if CPU(X86_64)
        { "image/functions/frame-at-faulting-accesses"_s, testFrameAtFaultingAccesses },
#endif
        { "image/functions/name-number-past-fifteen-bits"_s, testNameNumberPastFifteenBits },
        { "image/functions/tables-over-several-ranges"_s, testFunctionTablesOverSeveralRanges },
        { "image/functions/named-variables-as-bytes"_s, testNamedVariablesAsBytes },
        { "image/functions/numbers-in-groups"_s, testImageFunctions },
        { "image/functions/source-functions-in-groups"_s, testSourceFunctions },
        { "image/functions/closure-cells-in-groups"_s, testClosureCells },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
