/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTModuleNamespace.h"
#include "AOTModules.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeTable.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

static AbstractModuleRecord::ModulePhase modulePhaseOf(ImportPhase phase)
{
    switch (phase) {
    case ImportPhase::Evaluation:
        return AbstractModuleRecord::ModulePhase::Evaluation;
    case ImportPhase::Defer:
        return AbstractModuleRecord::ModulePhase::Defer;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ResolvedSpecifierForm specifierFormOf(uint32_t isURL)
{
    return isURL ? ResolvedSpecifierForm::URL : ResolvedSpecifierForm::Path;
}

JSC_DEFINE_JIT_OPERATION(operationAOTModuleBuiltinCell, JSCell*, (Instance* instance, uint32_t builtin, uint32_t module))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTModuleBuiltinCell);
    Modules& modules = modulesOf(*instance);
    OPERATION_RETURN(scope, modules.builtinCell(BuiltinID(builtin), ModuleID(module)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetModuleNamespace, JSCell*, (Instance* instance, uint32_t module, uint32_t phase))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetModuleNamespace);
    Modules& modules = modulesOf(*instance);
    OPERATION_RETURN(scope, modules.moduleNamespace(ModuleID(module), modulePhaseOf(static_cast<ImportPhase>(phase))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTIsNamespaceOfModule, size_t, (JSCell* cell, uint32_t module))
{
    auto* moduleNamespace = dynamicDowncast<ModuleNamespace>(cell);
    return moduleNamespace && moduleNamespace->hasNamespaceLayoutOf(ModuleID(module));
}

JSC_DEFINE_JIT_OPERATION(operationAOTImportModule, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTImportModule);
    Modules& modules = modulesOf(*instance);
    OPERATION_RETURN(scope, JSValue::encode(modules.import(ModuleID(module), ModuleID(referrer))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRequireModule, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRequireModule);
    Modules& modules = modulesOf(*instance);
    OPERATION_RETURN(scope, JSValue::encode(modules.require(ModuleID(module))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRecordRequire, void, (Instance* instance, uint32_t module, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRecordRequire);
    modulesOf(*instance).recordRequire(ModuleID(module), ModuleID(referrer));
    scope.assertNoException();
}

JSC_DEFINE_JIT_OPERATION(operationAOTResolveModule, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t, uint32_t, uint32_t isURL))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTResolveModule);
    Modules& modules = modulesOf(*instance);
    OPERATION_RETURN(scope, JSValue::encode(modules.resolve(ModuleID(module), specifierFormOf(isURL))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowModuleFailure, void, (Instance* instance, uint32_t failure, uint32_t isForRequire))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowModuleFailure);
    Modules& modules = modulesOf(*instance);
    modules.throwFailure(ModuleFailureID(failure), isForRequire);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTBeginModuleEvaluation, size_t, (Instance* instance, uint32_t module))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTBeginModuleEvaluation);
    Modules& modules = modulesOf(*instance);
    OPERATION_RETURN(scope, modules.beginEvaluationOfModuleInRecord(ModuleID(module)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAbandonModuleEvaluation, void, (Instance* instance, uint32_t module, JSCell* exception))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAbandonModuleEvaluation);
    modulesOf(*instance).abandonEvaluation(ModuleID(module), cellArgument<Exception>(exception, Entry::operationAOTAbandonModuleEvaluation)->value());
    scope.assertNoException();
}

JSC_DEFINE_JIT_OPERATION(operationAOTImportDynamic, EncodedJSValue, (Instance* instance, EncodedJSValue specifier, EncodedJSValue options, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTImportDynamic);
    Modules& modules = modulesOf(*instance);
    OPERATION_RETURN(scope, JSValue::encode(modules.importDynamic(JSValue::decode(specifier), JSValue::decode(options), ModuleID(referrer))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRequireDynamic, EncodedJSValue, (Instance* instance, EncodedJSValue specifier, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRequireDynamic);
    Modules& modules = modulesOf(*instance);
    OPERATION_RETURN(scope, JSValue::encode(modules.requireDynamic(JSValue::decode(specifier), ModuleID(referrer))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTResolveDynamic, EncodedJSValue, (Instance* instance, EncodedJSValue specifier, uint32_t referrer, uint32_t isURL, uint32_t isForRequire))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTResolveDynamic);
    Modules& modules = modulesOf(*instance);
    OPERATION_RETURN(scope, JSValue::encode(modules.resolveDynamic(JSValue::decode(specifier), ModuleID(referrer), isForRequire ? ResolvedSpecifierForm::PathForRequire : specifierFormOf(isURL))));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
