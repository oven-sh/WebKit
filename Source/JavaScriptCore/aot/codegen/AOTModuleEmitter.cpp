/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleEmitter.h"

#if ENABLE(AOT)

#include "AOTModuleWords.h"
#include "AOTPlacement.h"
#include "AOTStaticsEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

// The word of such a module holds its namespace object, once the run time has been asked for it, and nothing else.
static bool keepsNamespaceInValueWord(const Module& module, ImportPhase phase)
{
    switch (phase) {
    case ImportPhase::Defer:
        return false;
    case ImportPhase::Evaluation:
        break;
    }

    switch (module.kind) {
    case ModuleKind::SourceText:
    case ModuleKind::Data:
        return true;
    case ModuleKind::CommonJs:
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
    if (!keepsNamespaceInValueWord(m_program.at(module), phase) || !m_placement.hasModuleValue(module)) {
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
    ModuleID module = value->module();
    auto requireInSlowPath = [&](const CallOptions& options) {
        return callOperation<Boxed>(value, RuntimeOperation::RequireModule, { moduleIndex(module), moduleIndex(value->referrer()) }, options);
    };
    const Module& target = m_program.at(module);
    bool isAnsweredByValueWord = (target.kind == ModuleKind::RuntimeSource || target.kind == ModuleKind::Runtime) && !target.exportsVariable && m_placement.hasModuleValue(module);
    if (!isAnsweredByValueWord || value.shape() == CodeShape::Compact) {
        define(value, requireInSlowPath({ }));
        return;
    }

    Boxed stored = loadModuleValue(module);
    define(value, either<Boxed>(isNotEmpty(stored), [&] {
        return stored;
    }, [&] {
        return requireInSlowPath({ .isOnRarePath = true });
    }));
}

void ModuleEmitter::generateRecordRequire(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::RecordRequire, { moduleIndex(value->module()), moduleIndex(value->referrer()) });
}

void ModuleEmitter::generateResolveModule(const ValueContext& value)
{
    Word32 specifier = word32(static_cast<int32_t>(value->specifier().index()));
    define(value, callOperation<Boxed>(value, RuntimeOperation::ResolveModule, { moduleIndex(value->module()), moduleIndex(value->referrer()), specifier, word32(value->isURL()) }));
}

void ModuleEmitter::generateThrowModuleFailure(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::ThrowModuleFailure, { word32(static_cast<int32_t>(value->failure().index())), word32(value->isForRequire()) });
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
    define(value, callOperation<Boxed>(value, RuntimeOperation::ResolveDynamic, { in<Boxed>(value->specifierOperand()), moduleIndex(value->referrer()), word32(value->isURL()), word32(value->isForRequire()) }));
}

TypedPointer ModuleEmitter::statusOf(ModuleID module)
{
    static_assert(sizeof(StoredModuleStatus) == sizeof(uint8_t));
    Pointer statuses = loadImmutablePointer(AOT_INSTANCE_OFFSET(modules.status));
    return m_out.address(m_heaps.entire(AbstractHeap::ExternalState), statuses.value(), module.index());
}

void ModuleEmitter::generateBeginModuleEvaluation(const ValueContext& value)
{
    static_assert(StoredModuleStatus::Unlinked < StoredModuleStatus::InRecord && StoredModuleStatus::Linked < StoredModuleStatus::InRecord);
    static_assert(StoredModuleStatus::InRecord < StoredModuleStatus::Evaluated && StoredModuleStatus::Evaluated < StoredModuleStatus::Evaluating);
    ModuleID module = value->module();
    Word32 inRecord = word32(static_cast<int32_t>(StoredModuleStatus::InRecord));
    Word32 status(m_out.load8ZeroExt32(statusOf(module)));
    defineTest(value, selectByBranch<Condition>(belowOrEqual(status, inRecord), [&] {
        return either<Condition>(notEqual(status, inRecord), [&] {
            m_out.store32As8(loadMutable32(AOT_INSTANCE_OFFSET(modules.evaluatingMark)).value(), statusOf(module));
            return boolean(true);
        }, [&] {
            return callOperation<Condition>(value, RuntimeOperation::BeginModuleEvaluation, { moduleIndex(module) }, { .isOnRarePath = true });
        });
    }, [&] {
        return boolean(false);
    }));
}

void ModuleEmitter::generateFinishModuleEvaluation(const ValueContext& value)
{
    m_out.store32As8(m_out.constInt32(static_cast<int32_t>(StoredModuleStatus::Evaluated)), statusOf(value->module()));
}

void ModuleEmitter::generateAbandonModuleEvaluation(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::AbandonModuleEvaluation, { moduleIndex(value->module()), get(value->exceptionOperand()) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
