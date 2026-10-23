/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTGenerator.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTSavedFrame.h"
#include "DeferTermination.h"
#include "JSAsyncGenerator.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewGenerator, JSCell*, (Instance* instance, EncodedJSValue closure, uint32_t isAsync))
{
    constexpr Entry entry = Entry::operationAOTNewGenerator;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferTerminationForAWhile deferTermination(vm);
    JSObject* generatorFunction = objectArgument(closure, entry);
    JSObject* generator = createGenerator(globalObject, *instance, generatorFunction, isAsync ? CoroutineKind::AsyncGenerator : CoroutineKind::Generator);
    // The prototype property of a generator function is a data property that cannot be reconfigured, so reading it runs no code.
    scope.releaseAssertNoException();
    return generator;
}

static CoroutineKind generatorKind(JSObject* generator, Entry entry)
{
    if (generator->inherits<Generator>())
        return CoroutineKind::Generator;
    if (!generator->inherits<JSAsyncGenerator>()) [[unlikely]]
        crashOnWrongOperand(entry);
    return CoroutineKind::AsyncGenerator;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTakeGeneratorPrototype, void, (Instance* instance, JSCell* generator, EncodedJSValue closure))
{
    constexpr Entry entry = Entry::operationAOTTakeGeneratorPrototype;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferTerminationForAWhile deferTermination(vm);
    JSObject* object = cellArgument<JSObject>(generator, entry);
    Structure* structure = structureOfGenerator(globalObject, *instance, objectArgument(closure, entry), generatorKind(object, entry));
    scope.releaseAssertNoException();
    if (object->structure() != structure)
        object->setStructure(vm, structure);
}

JSC_DEFINE_JIT_OPERATION(operationAOTSaveFrameAndYield, JSCell*, (Instance* instance, JSCell* generator, uint64_t* frame, const uint64_t* bottom, void* resumeAddress, const uint64_t* suspensionWords))
{
    constexpr Entry entry = Entry::operationAOTSaveFrameAndYield;
    AOT_OPERATION_BEGIN(instance, entry);
    JSObject* object = cellArgument<JSObject>(generator, entry);
    CoroutineKind kind = generatorKind(object, entry);
    RELEASE_ASSERT(!savedFrameOfGenerator(object));
    SuspendedMachineFrame machineFrame { frame, bottom, resumeAddress, reinterpret_cast<const SuspensionWords*>(suspensionWords) };
    SavedFrame* saved = SavedFrame::create(vm, *instance, kind, machineFrame);
    saved->save(vm, machineFrame, SuspensionKind::Yield);
    setSavedFrameOfGenerator(vm, object, saved);
    OPERATION_RETURN(scope, saved);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
