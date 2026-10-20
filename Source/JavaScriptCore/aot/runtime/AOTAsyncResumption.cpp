/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAsyncResumption.h"

#if ENABLE(AOT)

#include "AOTCompiledCall.h"
#include "AOTCoroutineTable.h"
#include "AOTInstance.h"
#include "AOTJobs.h"
#include "AOTSavedFrame.h"
#include "AOTState.h"
#include "AOTStubsCoroutines.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "TopExceptionScope.h"
#include "VMInlines.h"

namespace JSC { namespace AOT {

JSValue enterSavedFrame(VM& vm, Instance& instance, SavedFrame* saved, ResumeMode mode, JSValue sent)
{
    static_assert(static_cast<int32_t>(JSGenerator::Argument::Generator) == 1 && static_cast<int32_t>(JSGenerator::Argument::State) == 2);
    static_assert(static_cast<int32_t>(JSGenerator::Argument::Value) == 3 && static_cast<int32_t>(JSGenerator::Argument::ResumeMode) == 4);
    ASSERT(&instance == Instance::of(saved->realm()));
    CallData callData = CallData::forCompiledCode(instance.state->coroutines->savedFrameEntrypoint(), instance.globalObject);
    return enterCompiledCode(vm, callData, saved, jsUndefined(), saved, jsUndefined(), jsNumber(0), sent, jsNumber(static_cast<int32_t>(engineResumeModeOf(mode))));
}

void resumeSavedAsyncFunction(JSGlobalObject* globalObject, VM& vm, Instance& instance, SavedFrame* saved, ResumeMode mode, JSValue resolution)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
        saved->willBeRestoredToStack();
        saved->reject(globalObject, vm, createStackOverflowError(globalObject));
        return;
    }
    enterSavedFrame(vm, instance, saved, mode, resolution);
    if (Exception* exception = scope.exception()) [[unlikely]] {
        JSValue error = exception->value();
        if (scope.clearExceptionExceptTermination())
            saved->reject(globalObject, vm, error);
    }
}

void resumeAsyncFunction(JSGlobalObject* globalObject, VM& vm, JSValue context, JSValue resolution, bool isRejected, bool isEnteredByJobLoop)
{
    auto* saved = uncheckedDowncast<SavedFrame>(context);
    Instance& instance = *Instance::of(saved->realm());
    saved->setIsEnteredByJobLoop(isEnteredByJobLoop);
    resumeSavedAsyncFunction(globalObject, vm, instance, saved, isRejected ? ResumeMode::Throw : ResumeMode::Next, resolution);
    if (instance.coroutines.waiterToResume)
        resumeRemainingWaiters(instance);
}

void resumeDriver(VM& vm, JSObject* driver, JSValue resolution, bool isRejected, bool isEnteredByJobLoop)
{
    RELEASE_ASSERT(driver->classInfo() == SavedFrame::info());
    resumeAsyncFunction(driver->realm(), vm, driver, resolution, isRejected, isEnteredByJobLoop);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
