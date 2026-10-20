/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleEmitter.h"

#if ENABLE(AOT)

#include "AOTPlacement.h"
#include "AOTStaticsEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

static bool hasNamespaceInValueSlot(const Module& module, ImportPhase phase)
{
    switch (phase) {
    case ImportPhase::Defer:
        return false;
    case ImportPhase::Evaluation:
        break;
    }

    switch (module.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
        return false;
    case ModuleKind::External:
        break;
    }

    switch (module.externalKind) {
    case ExternalKind::UncompiledCode:
        return true;
    case ExternalKind::HostValue:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ModuleEmitter::ModuleEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

Word32 ModuleEmitter::moduleIndex(ModuleID module)
{
    return word32(static_cast<int32_t>(module.index()));
}

Boxed ModuleEmitter::loadModuleValue(ModuleID module)
{
    const B3::AbstractHeap& heap = m_heaps.entire(AbstractHeap::RuntimeState);
    return Boxed(m_out.load64(emitter<StaticsEmitter>().inStatics(heap, m_placement.moduleValueOffset(module))));
}

void ModuleEmitter::generateModuleNamespace(const ValueContext& value)
{
    ModuleID module = value->module();
    ImportPhase phase = value->phase();
    auto getInSlowPath = [&](const CallOptions& options) {
        return callOperation<CellPointer>(value, RuntimeOperation::GetModuleNamespace, { moduleIndex(module), word32(static_cast<int32_t>(phase)) }, options);
    };
    if (!hasNamespaceInValueSlot(m_program.at(module), phase)) {
        define(value, getInSlowPath({ }));
        return;
    }

    Boxed stored = loadModuleValue(module);
    define(value, either<CellPointer>(isNotEmpty(stored), [&] {
        return unboxCell(stored);
    }, [&] {
        return getInSlowPath({ .isOnRarePath = true });
    }));
}

void ModuleEmitter::generateImportModule(const ValueContext& value)
{
    define(value, unboxCell(callOperation<Boxed>(value, RuntimeOperation::ImportModule, { moduleIndex(value->module()), moduleIndex(value->referrer()) })));
}

void ModuleEmitter::generateRequireModule(const ValueContext& value)
{
    define(value, callOperation<Boxed>(value, RuntimeOperation::RequireModule, { moduleIndex(value->module()), moduleIndex(value->referrer()) }));
}

void ModuleEmitter::generateResolveModule(const ValueContext& value)
{
    Word32 specifier = word32(static_cast<int32_t>(value->specifier().index()));
    define(value, callOperation<Boxed>(value, RuntimeOperation::ResolveModule, { moduleIndex(value->module()), moduleIndex(value->referrer()), specifier, word32(value->isURL()) }));
}

void ModuleEmitter::generateThrowModuleFailure(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::ThrowModuleFailure, { word32(static_cast<int32_t>(value->failure().index())) });
    m_out.unreachable();
}

void ModuleEmitter::generateImportDynamic(const ValueContext& value)
{
    Boxed specifier = in<Boxed>(value->specifierOperand());
    Boxed options = inOptional<Boxed>(value->optionsOperand()).value_or(undefined());
    define(value, unboxCell(callOperation<Boxed>(value, RuntimeOperation::ImportDynamic, { specifier, options, moduleIndex(value->referrer()) })));
}

void ModuleEmitter::generateRequireDynamic(const ValueContext& value)
{
    define(value, callOperation<Boxed>(value, RuntimeOperation::RequireDynamic, { in<Boxed>(value->specifierOperand()), moduleIndex(value->referrer()) }));
}

void ModuleEmitter::generateResolveDynamic(const ValueContext& value)
{
    define(value, callOperation<Boxed>(value, RuntimeOperation::ResolveDynamic, { in<Boxed>(value->specifierOperand()), moduleIndex(value->referrer()), word32(value->isURL()) }));
}

void ModuleEmitter::generateGetRequiredValue(const ValueContext& value)
{
    define(value, loadModuleValue(value->module()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
