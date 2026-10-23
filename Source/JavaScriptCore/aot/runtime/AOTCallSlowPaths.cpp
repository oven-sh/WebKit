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
#include "AOTState.h"
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTBytesAllocatedAtPrologueCheck, size_t, (Instance*, const void* returnAddress))
{
    const Image& image = *Image::current();
    std::optional<PrologueCheck> check = image.prologueCheckAt(returnAddress);
    RELEASE_ASSERT(check);
    const ImageFrame& frame = image.frameOf(check->function);
    return frame.hasFrameRecord() ? 0 : frame.frameSizeInBytes() + sizeof(void*);
}

#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReportPoisonedAccess, void, (Instance*, void* address, uint32_t sizeAndIsStore))
{
    void* returnAddress = __builtin_return_address(0);
    void* frame = __builtin_frame_address(1);
    CodeLocation location = Image::current()->locate(returnAddress);
    if (location.kind == CodeKind::Stub)
        dataLogLn("AOT: compiled code touches poisoned memory: a stub, at ", imageCodeOffset(returnAddress).value_or(0), " bytes into the code of the image");
    else
        dataLogLn("AOT: compiled code touches poisoned memory: function ", location.function, ", at ", imageCodeOffset(returnAddress).value_or(0), " bytes into the code of the image");
    __asan_report_error(returnAddress, frame, frame, address, sizeAndIsStore >> 8, sizeAndIsStore & 0xff);
    CRASH();
}
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
