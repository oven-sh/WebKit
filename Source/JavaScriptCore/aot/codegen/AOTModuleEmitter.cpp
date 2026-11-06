/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleEmitter.h"

#if ENABLE(AOT)

#include "AOTAsyncAwaitConditions.h"
#include "AOTBarrierEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTModuleWords.h"
#include "AOTPlacement.h"
#include "AOTStaticsEmitter.h"
#include <algorithm>

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

bool ModuleEmitter::hasSingleNamespaceInValueWord(ModuleID id)
{
    const Module& module = m_program.at(id);
    if (module.kind != ModuleKind::SourceText && module.kind != ModuleKind::Data)
        return false;
    if (module.exportsVariable || !m_placement.hasModuleValue(id))
        return false;
    return std::ranges::none_of(module.requests, [](const ModuleRequest& request) {
        return request.kind == ModuleRequestKind::Require;
    });
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

Boxed ModuleEmitter::loadRequireCacheValue(ModuleID module)
{
    const B3::AbstractHeap& heap = m_heaps.entire(AbstractHeap::RuntimeState);
    return Boxed(m_out.load64(emitter<StaticsEmitter>().inStatics(heap, m_placement.requireCacheValueOffset(module))));
}

void ModuleEmitter::generateModuleNamespace(const ValueContext& value)
{
    ModuleID module = value->module();
    ImportPhase phase = value->phase();
    auto getInSlowPath = [&](const CallOptions& options) {
        return callOperation<CellPointer>(value, RuntimeOperation::GetModuleNamespace, { moduleIndex(module), word32(static_cast<int32_t>(phase)) }, options);
    };
    if (!keepsNamespaceInValueWord(m_program.at(module), phase) || !m_placement.hasModuleValue(module)) {
        std::optional<uint32_t> offset = phase == ImportPhase::Evaluation ? m_placement.moduleNamespaceOffset(module) : std::nullopt;
        if (!offset) {
            define(value, getInSlowPath({ }));
            return;
        }
        // `fs.existsSync(path)` behind `import * as fs` hands the object on as `this`, at every call. It is made once.
        TypedPointer cellWord = emitter<StaticsEmitter>().inStatics(m_heaps.entire(AbstractHeap::AllocatorState), *offset);
        define(value, emitter<ConstantEmitter>().permanentCell(cellWord, false, [&] {
            CellPointer cell = getInSlowPath({ .isOnRarePath = true });
            m_out.store64(cell.value(), cellWord);
            emitter<BarrierEmitter>().markCardOfStatics(*offset);
            return cell;
        }));
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
    Boxed options = inOptional<Boxed>(value->optionsOperand()).value_or(undefined());
    define(value, unboxCell(callOperation<Boxed>(value, RuntimeOperation::ImportModule, { moduleIndex(value->module()), options, moduleIndex(value->referrer()) })));
}

void ModuleEmitter::generateRequireModule(const ValueContext& value)
{
    ModuleID module = value->module();
    const Module& target = m_program.at(module);
    if (value->ifHeldForImport()) {
        define(value, callOperation<Boxed>(value, RuntimeOperation::RequireIfHeldForImport, { moduleIndex(module), moduleIndex(value->referrer()) }));
        return;
    }
    auto requireInSlowPath = [&](CallOptions options) {
        options.calledBuiltin = functionOfHostThatRequireCallsFor(target);
        return callOperation<Boxed>(value, RuntimeOperation::RequireModule, { moduleIndex(module), moduleIndex(value->referrer()) }, options);
    };
    bool addsToRequireCache = requireAddsToRequireCache(m_program, value->referrer(), module);
    bool isAnsweredByValueWord = requireReadsModuleValue(m_program, target) && (addsToRequireCache ? m_placement.hasRequireCacheValue(module) : m_placement.hasModuleValue(module));
    if (!isAnsweredByValueWord || value.shape() == CodeShape::Compact) {
        define(value, requireInSlowPath({ }));
        return;
    }

    Boxed stored = addsToRequireCache ? loadRequireCacheValue(module) : loadModuleValue(module);
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

void ModuleEmitter::generateAreLoaderFunctionsStored(const ValueContext& value)
{
    defineTest(value, callWithoutSite<Condition>(RuntimeOperation::AreLoaderFunctionsStored, { }));
}

void ModuleEmitter::generateWasLoadedSynchronously(const ValueContext& value)
{
    defineTest(value, callWithoutSite<Condition>(RuntimeOperation::WasLoadedSynchronously, { moduleIndex(value->module()) }));
}

void ModuleEmitter::generateCountExecution(const ValueContext& value)
{
    Operand arguments[] { moduleIndex(value->module()), word32(static_cast<int32_t>(value->description().index())), word32(static_cast<int32_t>(value->counter())) };
    callWithoutSite(RuntimeOperation::CountExecution, arguments, Representation::Void, { });
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
            Word32 evaluatingMark = loadMutable32(AOT_INSTANCE_OFFSET(modules.evaluatingMark));
            m_out.store32As8(evaluatingMark.value(), statusOf(module));
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
    ModuleID module = value->module();
    LValue evaluated = m_out.constInt32(static_cast<int32_t>(StoredModuleStatus::Evaluated));
    OptionSet<ModuleFlags> flags = m_program.at(module).flags;
    // A module without a start of its own is marked behind the site that requires it, whatever BeginModuleEvaluation has answered there: the loader may hold its status in a record.
    bool isMarkedWhereRequired = flags.contains(ModuleFlags::IsListedInCache) && !flags.contains(ModuleFlags::EvaluatesItself);
    if (!flags.contains(ModuleFlags::MayBeEvicted) && !isMarkedWhereRequired) {
        m_out.store32As8(evaluated, statusOf(module));
        return;
    }
    // The file may have taken itself out of require.cache while it ran. The mark of that stays, and the next require() asks the run time.
    Word32 status(m_out.load8ZeroExt32(statusOf(module)));
    ifElse(equal(status, word32(static_cast<int32_t>(StoredModuleStatus::InRecord))), [] { }, [&] {
        m_out.store32As8(evaluated, statusOf(module));
    });
}

// Whatever queues a job calls the run time, so the words of the queue are run-time state.
void ModuleEmitter::generateStopLoadingBehindFile(const ValueContext& value)
{
    const B3::AbstractHeap& queueWords = m_heaps.entire(AbstractHeap::RuntimeState);
    LValue queue = m_out.loadPtr(mutableInstanceField(offsetOfMicrotaskQueue));
    Integer start(m_out.load64(m_out.address(queueWords, queue, MicrotaskQueue::offsetOfStart())));
    Integer end(m_out.load64(m_out.address(queueWords, queue, MicrotaskQueue::offsetOfEnd())));
    defineTest(value, either<Condition>(equal(start, end), [&] {
        return boolean(false);
    }, [&] {
        return callOperation<Condition>(value, RuntimeOperation::StopLoadingBehindFile, { moduleIndex(value->module()) }, { .isOnRarePath = true });
    }));
}

void ModuleEmitter::generateEvaluateRequiredModule(const ValueContext& value)
{
    ModuleID module = value->module();
    // The operations around the site record who required whom, also where the module has run and the run time is not asked.
    auto askRunTime = [&](const CallOptions& options) {
        callOperation<Boxed>(value, RuntimeOperation::RequireModule, { moduleIndex(module), moduleIndex(ModuleID()) }, options);
    };
    if (value.shape() == CodeShape::Compact) {
        askRunTime({ });
        return;
    }
    Word32 status(m_out.load8ZeroExt32(statusOf(module)));
    ifElse(equal(status, word32(static_cast<int32_t>(StoredModuleStatus::Evaluated))), [] { }, [&] {
        askRunTime({ .isOnRarePath = true });
    });
}

void ModuleEmitter::generateAbandonModuleEvaluation(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::AbandonModuleEvaluation, { moduleIndex(value->module()), get(value->exceptionOperand()) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
