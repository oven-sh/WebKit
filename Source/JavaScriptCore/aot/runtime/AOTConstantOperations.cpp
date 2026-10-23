/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTAllocationCounts.h"
#include "AOTBuiltinCells.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTProgramStrings.h"
#include "AOTProgramTables.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "DeferTermination.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include <wtf/DataLog.h>
#include <wtf/WTFProcess.h>

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringConstant, JSCell*, (Instance* instance, uint32_t index))
{
    LeafOperationScope leaf(*instance->vm);
    return instance->state->strings->constant(*instance->vm, index);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewStringConstant, JSCell*, (Instance* instance, uint32_t index))
{
    LeafOperationScope leaf(*instance->vm);
    if (index & ProgramStrings::isPositionOfFunctionName)
        return instance->state->strings->functionNameAt(*instance->vm, index & ~ProgramStrings::isPositionOfFunctionName);
    return instance->state->strings->createConstant(*instance->vm, index);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTKeyConstant, JSCell*, (Instance* instance, uint32_t index))
{
    VM& vm = *instance->vm;
    LeafOperationScope leaf(vm);
    DeferGCForAWhile deferGC(vm);
    return instance->state->strings->keyCell(vm, index);
}

// Compiled code reads through the cell without a test.
static void exitIfRealmLacksHostOf(State& state, uint32_t numberOfCell)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(state.tables.builtinCells()[numberOfCell], table);
    RELEASE_ASSERT(key);
    if (BuiltinBindings::provides(table.at(key->builtin()).origin))
        return;
    dataLogLn("AOT: the program needs the host's ", table.textOf(key->builtin()), ", and this realm has no host");
    exitProcess(4);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTBuiltinCell, JSCell*, (Instance* instance, uint32_t index))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTBuiltinCell);
    DeferGCForAWhile deferGC(vm);
    JSCell* cell = instance->state->builtinCells->cell(index);
    if (!cell) [[unlikely]]
        exitIfRealmLacksHostOf(*instance->state, index);
    OPERATION_RETURN(scope, cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTBigIntConstant, EncodedJSValue, (Instance* instance, uint32_t index))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTBigIntConstant);
    DeferTerminationForAWhile deferTermination(vm);
    DeferGCForAWhile deferGC(vm);
    OPERATION_RETURN(scope, JSValue::encode(instance->state->strings->bigIntConstant(globalObject, index)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSingletonClosure, JSCell*, (Instance* instance, uint32_t function))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSingletonClosure);
    DeferGCForAWhile deferGC(vm);
    OPERATION_RETURN(scope, instance->state->objects->closures.closure(vm, FunctionID(function)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNewTemplateObject, JSCell*, (Instance* instance, uint32_t request))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewTemplateObject);
    ProgramStrings& strings = *instance->state->strings;
    const auto& templateObject = requestAt<TemplateObjectRequest>(instance, request);
    MarkedArgumentBuffer cookedStrings;
    MarkedArgumentBuffer rawStrings;
    for (const auto& part : unsafeMakeSpan(templateObject.parts, templateObject.count)) {
        cookedStrings.append(part.cookedPlusOne ? JSValue(strings.constant(vm, part.cookedPlusOne - 1)) : jsUndefined());
        rawStrings.append(strings.constant(vm, part.raw));
    }
    if (cookedStrings.hasOverflowed() || rawStrings.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    JSArray* cooked = constructArray(globalObject, static_cast<JSC::ArrayAllocationProfile*>(nullptr), cookedStrings);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* raw = constructArray(globalObject, static_cast<JSC::ArrayAllocationProfile*>(nullptr), rawStrings);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    objectConstructorFreeze(globalObject, raw);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    cooked->putDirect(vm, vm.propertyNames->raw, raw, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    objectConstructorFreeze(globalObject, cooked);
    OPERATION_RETURN(scope, cooked);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
