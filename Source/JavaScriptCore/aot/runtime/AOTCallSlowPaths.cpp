/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeTable.h"

#if ENABLE(AOT)

#include "AOTCallSiteCache.h"
#include "AOTCompiledFrame.h"
#include "AOTImage.h"
#include "AOTOperationScope.h"
#include "AOTStackTrace.h"
#include "AOTState.h"
#include "AOTStubDescriptor.h"
#include "AOTUnwinding.h"
#include "CallFrameInlines.h"
#include "DeferGCInlines.h"
#include "ExceptionHelpers.h"
#include "OwedOutOfMemoryError.h"
#include "VMEntryRecord.h"

#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
#include <sanitizer/asan_interface.h>
#endif

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTThrowStackOverflowError, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowStackOverflowError);
    DeferGCForAWhile deferGC(vm);
    if (throwOutOfMemoryErrorIfOwed(vm, globalObject, scope)) [[unlikely]] {
        OPERATION_RETURN(scope);
    }
    throwStackOverflowError(globalObject, scope);
    OPERATION_RETURN(scope);
}

// A caller that relies on the type has a landing for what is not of it. Any other caller takes whatever is returned, at its return address.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFindLandingOfMisfit, void*, (Instance* instance, void* returnAddress, EncodedJSValue misfit))
{
    const VMState& state = instance->vm->aotState();
    const CallSiteCache::Entry* site = state.callSiteCache->find(returnAddress);
    if (!site)
        site = state.callSiteCache->resolve(*state.image, returnAddress);
    if (!site || site->isStubReturnSite() || !site->handler || site->handler->catchesExceptions())
        return returnAddress;
    instance->scratch.misfit = misfit;
    return const_cast<uint8_t*>(state.image->codeOf(site->code.index())) + site->handler->codeOffset;
}

static ALWAYS_INLINE bool hasHandler(const CompiledUnwindResult& result)
{
    switch (result.status) {
    case CompiledUnwindStatus::FoundHandler:
        RELEASE_ASSERT(result.handler.code);
        return true;
    case CompiledUnwindStatus::LeftCompiledCode:
    case CompiledUnwindStatus::ReachedEntryFrame:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTLookupExceptionHandler, UGPRPair, (VM* vmPointer, Instance* instance))
{
    VM& vm = *vmPointer;
    CallerFrameAndPC* frameRecord = AOT_OPERATION_FRAME_RECORD();
    CallFrame* callFrame = frameRecord->callerFrame;
    ExceptionWords& exceptions = instance->runtime.exceptions;
    Exception* exception = vm.exceptionForInspection();
    bool needsException = false;
    Exception* rethrownException = nullptr;
    if (exception)
        exceptions.thrownValue = 0;
    else {
        JSValue thrown = JSValue::decode(exceptions.thrownValue);
        needsException = needsExceptionForEveryThrow(*instance);
        if (thrown.isCell() && thrown.asCell()->type() == CellType)
            rethrownException = uncheckedDowncast<Exception>(thrown.asCell());
    }
    bool isTermination = exception && vm.isTerminationException(exception);
    std::optional<CompiledFrameIterator> frames = isTermination ? std::nullopt : findCompiledCallers(vm, *instance, callFrame);
    CompiledUnwindResult found { CompiledUnwindStatus::LeftCompiledCode, { } };
    if (frames) [[likely]]
        found = unwindCompiledFrames(vm, *frames, *vmEntryRecord(vm.topEntryFrame), false);
    bool isHandled = hasHandler(found);
    bool takesException = found.handler.kind == CompiledHandlerKind::Finally;
    UGPRPair handlerCodeAndFrame = makeUGPRPair(std::bit_cast<UCPURegister>(found.handler.code), std::bit_cast<UCPURegister>(found.handler.framePointer));
    if (rethrownException && isHandled && !needsException) {
        if (!takesException)
            exceptions.thrownValue = JSValue::encode(rethrownException->value());
        return handlerCodeAndFrame;
    }
    bool stackIsUnobserved = !exception && !rethrownException && isHandled && !needsException && (!takesException || isCaughtByCompiledCode(vm, *frames));
    if (isHandled && (exception || !(takesException || needsException))) [[likely]]
        return handlerCodeAndFrame;
    AOTOperationPrologueCallFrameTracer tracer(vm, frameRecord);
    if (!exception) {
        JSValue thrown = JSValue::decode(std::exchange(exceptions.thrownValue, 0));
        RELEASE_ASSERT(thrown);
        auto scope = DECLARE_THROW_SCOPE(vm);
        Exception* thrownException = throwException(instance->globalObject, scope, rethrownException ? thrown : JSValue(Exception::create(vm, thrown, stackIsUnobserved ? Exception::StackCaptureAction::DoNotCaptureStack : Exception::StackCaptureAction::CaptureStack)));
        if (isHandled && takesException && !needsException) {
            auto topScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            exceptions.thrownValue = JSValue::encode(thrownException);
            topScope.clearException();
        }
    }
    if (isHandled)
        return handlerCodeAndFrame;
    unwindBeyondCompiledFrames(vm, std::bit_cast<CallFrame*>(frameRecord), found.status);
    return makeUGPRPair(0, 0);
}

// First what the function has allocated below its return address, zero if it has a frame record. Second what its entry has put above its return address.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTBytesAllocatedAtPrologueCheck, UGPRPair, (Instance*, const void* returnAddress))
{
    const Image& image = *Image::current();
    std::optional<PrologueCheck> check = image.prologueCheckAt(returnAddress);
    RELEASE_ASSERT(check);
    const ImageFrame& frame = image.frameOf(check->function);
    size_t bytesBelowReturnAddress = frame.hasFrameRecord() ? 0 : frame.frameSizeInBytes() + sizeof(void*);
    size_t bytesAboveReturnAddress = image.function(check->function).has(ImageFunctionFlag::SpillsArgumentRegisters) ? sizeOfSpilledArgumentRegisters : 0;
    return makeUGPRPair(bytesBelowReturnAddress, bytesAboveReturnAddress);
}

#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

// Behind the report of a check whose slow half lies at the end of the function stands a jump to the access.
static const void* addressOfAccess(const void* returnAddress)
{
    constexpr uint8_t breakpoint = 0xcc;
    constexpr uint8_t nearJump = 0xe9;
    auto* bytes = static_cast<const uint8_t*>(returnAddress);
    if (bytes[0] != breakpoint || bytes[1] != nearJump)
        return returnAddress;
    int32_t distance;
    memcpy(&distance, bytes + 2, sizeof(distance));
    return bytes + 2 + sizeof(distance) + distance;
}

static bool isBehindCall(const void* address)
{
    constexpr uint8_t nearCall = 0xe8;
    constexpr uint8_t group5 = 0xff;
    auto* bytes = static_cast<const uint8_t*>(address);
    if (bytes[-5] == nearCall)
        return true;
    for (int length : { 2, 3, 4, 6, 7 }) {
        if (bytes[-length] == group5 && (bytes[1 - length] & 0x38) == 0x10)
            return true;
    }
    return false;
}

static void printNameOfStubAt(const Image& image, uint32_t offsetInSharedCode)
{
    const StubAddressInImage* stub = nullptr;
    for (const StubAddressInImage& candidate : image.section<StubAddressInImage>(ImageSection::StubAddresses)) {
        if (candidate.offset <= offsetInSharedCode && (!stub || candidate.offset > stub->offset))
            stub = &candidate;
    }
    if (!stub)
        return;
    StubKey key = StubKey::fromBits(stub->key);
    const StubDescriptor& descriptor = descriptorOf(key.family());
    dataLog(" ", descriptor.name);
    if (key.form() < descriptor.formNames.size())
        dataLog(".", descriptor.formNames[key.form()]);
}

// A stub without a frame record, and a function without one, have their return address somewhere between the stack pointer and the frame record above.
SUPPRESS_ASAN static void collectCompiledFrames(const Image& image, bool hasFrameRecord, void* const* stackPointer, void* const* frameRecord, WTF::Vector<const void*, 64>& addresses)
{
    const StackBounds& stack = Thread::currentSingleton().stack();
    if (!stack.contains(const_cast<void**>(frameRecord)))
        return;
    if (!hasFrameRecord && stack.contains(const_cast<void**>(stackPointer))) {
        for (void* const* word = stackPointer; word < frameRecord; ++word) {
            if (image.locate(*word).kind == CodeKind::Function && isBehindCall(*word))
                addresses.append(*word);
        }
    }
    while (addresses.size() < addresses.capacity()) {
        addresses.append(frameRecord[1]);
        auto* next = static_cast<void* const*>(frameRecord[0]);
        if (next <= frameRecord || !stack.contains(const_cast<void**>(next)))
            break;
        frameRecord = next;
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReportPoisonedAccess, void, (Instance*, void* address, uint32_t sizeAndIsStore, void* const* stackPointer))
{
    void* returnAddress = __builtin_return_address(0);
    void* frame = __builtin_frame_address(1);
    const Image& image = *Image::current();
    CodeLocation location = image.locate(returnAddress);
    WTF::Vector<const void*, 64> addresses;
    bool hasFrameRecord = false;
    if (location.kind == CodeKind::Stub) {
        dataLog("AOT: compiled code touches poisoned memory: a stub");
        printNameOfStubAt(image, location.offset);
        dataLogLn(", at ", imageCodeOffset(returnAddress).value_or(0), " bytes into the code of the image");
    } else {
        dataLogLn("AOT: compiled code touches poisoned memory: function ", location.function, ", at ", imageCodeOffset(returnAddress).value_or(0), " bytes into the code of the image");
        const void* access = addressOfAccess(returnAddress);
        addresses.append(access);
        CodeLocation locationOfAccess = image.locate(access);
        hasFrameRecord = image.frameOf(location.function).hasFrameRecord() && image.hasFrameAt(locationOfAccess.function, locationOfAccess.offset);
    }
    collectCompiledFrames(image, hasFrameRecord, stackPointer, static_cast<void* const*>(frame), addresses);
    dataLog(describeCodeAddresses(addresses.span()));
    __asan_report_error(returnAddress, frame, frame, address, sizeAndIsStore >> 8, sizeAndIsStore & 0xff);
    CRASH();
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
