/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTCompiledCall.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTState.h"
#include "Error.h"
#include "Exception.h"
#include "ExceptionHelpers.h"
#include "GlobalObjectMethodTable.h"
#include "JSCInlines.h"
#include "VMInlines.h"
#include <bit>
#include <wtf/StackPointer.h>

namespace JSC { namespace AOT {

static constexpr size_t stackNeededToStartRuntimeModule = 128 * KB;
static_assert(BuiltinCells::stackKeptFromProgram >= 2 * stackNeededToStartRuntimeModule, "the first use of a built-in cell loads a module, which loads others");

static bool canStartRuntimeModule(VM& vm)
{
    size_t availableStack = std::bit_cast<uintptr_t>(currentStackPointer()) - std::bit_cast<uintptr_t>(vm.softStackLimit());
    return availableStack >= stackNeededToStartRuntimeModule && vm.isSafeToRecurseSoft();
}

JSValue Modules::callBody(ModuleID module, JSValue thisValue, const ArgList& arguments)
{
    std::optional<FunctionID> body = data(module).bodyFunction();
    if (!body)
        return jsUndefined();

    SetForScope isAskingHost(m_isAskingHost, false);
    JSObject* closure = m_state.objects->closures.createClosureForOneCall(*m_instance.vm, *body);
    return callCompiledCode(m_instance.globalObject, closure, thisValue, arguments);
}

JSValue Modules::executeValueBody(ModuleID module)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    setStatus(module, ModuleStatus::Evaluating);
    JSValue value = callBody(module, jsUndefined(), ArgList());
    if (Exception* exception = scope.exception()) [[unlikely]] {
        if (vm.isTerminationException(exception))
            setStatus(module, ModuleStatus::Linked);
        else
            setFailed(module, exception->value());
        return { };
    }
    setValue(module, value);
    setStatus(module, ModuleStatus::Evaluated);
    return value;
}

JSValue Modules::executeRuntimeValue(ModuleID module)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    auto* requireBuiltinModule = realm->globalObjectMethodTable()->requireBuiltinModule;
    if (!requireBuiltinModule) [[unlikely]] {
        throwTypeError(realm, scope, withAOTErrorNote("This realm has no builtin modules"_s));
        return { };
    }
    setStatus(module, ModuleStatus::Evaluating);
    JSValue value = requireBuiltinModule(realm, *data(module).registryID());
    if (scope.exception()) [[unlikely]] {
        setStatus(module, ModuleStatus::Unlinked);
        return { };
    }
    setValue(module, value);
    setStatus(module, ModuleStatus::Evaluated);
    return value;
}

JSValue Modules::executeHostValue(ModuleID module)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    JSObject* requirer = moduleObject(*data(module).referrerModule());
    RETURN_IF_EXCEPTION(scope, { });
    setStatus(module, ModuleStatus::Evaluating);
    RequestToHostScope requestToHost(*this);
    JSValue value = host().requireSpecifier(realm, requirer, pathString(module));
    if (scope.exception()) [[unlikely]] {
        setStatus(module, ModuleStatus::Unlinked);
        return { };
    }
    setValue(module, value);
    setStatus(module, ModuleStatus::Evaluated);
    return value;
}

JSValue Modules::execute(ModuleID module)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& moduleData = data(module);

    if (moduleData.kind == ModuleKind::CommonJs) {
        ChildLink childLink = ChildLink::Recorded;
        RELEASE_AND_RETURN(scope, executeCommonJS(module, nullptr, nullptr, childLink));
    }

    switch (status(module)) {
    case ModuleStatus::Unlinked:
    case ModuleStatus::Linked:
        break;
    case ModuleStatus::Evaluating:
        // Bun's registry would create the module again and again until the stack overflows.
        throwStackOverflowError(realm, scope);
        return { };
    case ModuleStatus::Evaluated:
        return value(module);
    case ModuleStatus::Failed:
        scope.release();
        throwEvaluationError(module);
        return { };
    case ModuleStatus::Linking:
    case ModuleStatus::EvaluatingAsync:
        RELEASE_ASSERT_NOT_REACHED();
    }

    switch (moduleData.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
        RELEASE_ASSERT_NOT_REACHED();
    case ModuleKind::RuntimeSource:
        if (!canStartRuntimeModule(vm)) [[unlikely]] {
            throwStackOverflowError(realm, scope);
            return { };
        }
        RELEASE_AND_RETURN(scope, executeValueBody(module));
    case ModuleKind::Runtime:
        RELEASE_AND_RETURN(scope, executeRuntimeValue(module));
    case ModuleKind::External:
        switch (moduleData.externalKind) {
        case ExternalKind::HostValue:
            RELEASE_AND_RETURN(scope, executeHostValue(module));
        case ExternalKind::UncompiledCode:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
