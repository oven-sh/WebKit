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
#include "AOTIRProgram.h"
#include "AOTModuleWords.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

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

bool ModuleEmitter::asksRunTimeForNamespaceAtEveryRead(const Program& program, const Plan& plan, ModuleID module, ImportPhase phase)
{
    if (keepsNamespaceInValueWord(program.at(module), phase) && plan.hasModuleValue(module))
        return false;
    return phase != ImportPhase::Evaluation || !plan.hasModuleNamespaceWord(module);
}

bool ModuleEmitter::hasSingleNamespaceInValueWord(ModuleID id)
{
    const Module& module = m_program.at(id);
    if (module.kind != ModuleKind::SourceText && module.kind != ModuleKind::Data)
        return false;
    if (module.exportsVariable || !m_plan.hasModuleValue(id))
        return false;
    return std::ranges::none_of(module.requests, [](const ModuleRequest& request) {
        return request.kind == ModuleRequestKind::Require;
    });
}

Word32 ModuleEmitter::moduleIndex(ModuleID module)
{
    return word32(static_cast<int32_t>(module.index()));
}

TypedPointer ModuleEmitter::valueWordOf(ModuleID module, int32_t offsetFromValue)
{
    return instanceWord(m_heaps.AOTInstance_moduleValues, m_mentions.add(NumberSpace::ModuleValue, module, offsetFromValue));
}

Boxed ModuleEmitter::loadModuleValue(ModuleID module)
{
    return Boxed(m_out.load64(valueWordOf(module, 0)));
}

Boxed ModuleEmitter::loadRequireCacheValue(ModuleID module)
{
    return Boxed(m_out.load64(valueWordOf(module, static_cast<int32_t>(sizeof(EncodedJSValue)))));
}

void ModuleEmitter::mentionRequireEdge(const ValueContext& value)
{
    uint32_t key[] { value->referrer().index(), value->module().index(), value->specifier().index() };
    m_mentions.add(NumberSpace::RequireEdge, std::span<const uint32_t>(key));
}

void ModuleEmitter::generateModuleNamespace(const ValueContext& value)
{
    ModuleID module = value->module();
    ImportPhase phase = value->phase();
    auto getInSlowPath = [&](const CallOptions& options) {
        return callOperation<CellPointer>(value, RuntimeOperation::GetModuleNamespace, { moduleIndex(module), word32(static_cast<int32_t>(phase)) }, options);
    };
    if (value->ofModuleItself()) {
        define(value, callOperation<CellPointer>(value, RuntimeOperation::GetNamespaceOfModuleItself, { moduleIndex(module) }));
        return;
    }
    if (value->asksAnew()) {
        define(value, getInSlowPath({ }));
        return;
    }
    if (!keepsNamespaceInValueWord(m_program.at(module), phase) || !m_plan.hasModuleValue(module)) {
        if (asksRunTimeForNamespaceAtEveryRead(m_program, m_plan, module, phase)) {
            define(value, getInSlowPath({ }));
            return;
        }
        uint32_t key[] { module.index() };
        define(value, emitter<ConstantEmitter>().numberedPermanentCell(pureFunction(PureOperation::ModuleNamespace, module.index()), false, [&] {
            return instanceWord(m_heaps.entire(AbstractHeap::AllocatorState), m_mentions.add(NumberSpace::ModuleNamespace, module));
        }, [&](TypedPointer cellWord) {
            CellPointer cell = getInSlowPath({ .isOnRarePath = true });
            m_out.store64(cell.value(), cellWord);
            emitter<BarrierEmitter>().markCard(m_mentions.cardOfWord(NumberSpace::ModuleNamespace, key));
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
    mentionRequireEdge(value);
    if (value->onlyIfImported()) {
        define(value, callOperation<Boxed>(value, RuntimeOperation::RequireIfImported, { moduleIndex(module), moduleIndex(value->referrer()) }));
        return;
    }
    if (std::optional<Boxed> requirerObject = inOptional<Boxed>(value->requirerObjectOperand())) {
        CallOptions options;
        options.calledBuiltin = hostRequireFunctionFor(target);
        define(value, callOperation<Boxed>(value, RuntimeOperation::RequireModuleWithRequirer, { moduleIndex(module), moduleIndex(value->referrer()), *requirerObject }, options));
        return;
    }
    auto requireInSlowPath = [&](CallOptions options) {
        options.calledBuiltin = hostRequireFunctionFor(target);
        return callOperation<Boxed>(value, RuntimeOperation::RequireModule, { moduleIndex(module), moduleIndex(value->referrer()) }, options);
    };
    bool addsToRequireCache = requireAddsToRequireCache(m_program, value->referrer(), module);
    bool hasWordThatAnswers = m_plan.hasModuleValue(module) && (!addsToRequireCache || m_plan.hasRequireCacheValue(module));
    if (!requireReadsModuleValue(m_program, target) || !hasWordThatAnswers) {
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
    if (std::optional<Boxed> requirerObject = inOptional<Boxed>(value->requirerObjectOperand())) {
        callVoidOperation(value, RuntimeOperation::RecordRequireWithRequirer, { moduleIndex(value->module()), moduleIndex(value->referrer()), word32(value->fromCreatedRequire()), word32(value->followsLoad()), *requirerObject });
        return;
    }
    callVoidOperation(value, RuntimeOperation::RecordRequire, { moduleIndex(value->module()), moduleIndex(value->referrer()), word32(value->fromCreatedRequire()) });
}

void ModuleEmitter::generateAreLoaderFunctionsStored(const ValueContext& value)
{
    defineTest(value, callWithoutSite<Condition>(RuntimeOperation::AreLoaderFunctionsStored, { }));
}

void ModuleEmitter::generateWasLoadedSynchronously(const ValueContext& value)
{
    defineTest(value, callWithoutSite<Condition>(RuntimeOperation::WasLoadedSynchronously, { moduleIndex(value->module()) }));
}

void ModuleEmitter::generateSeesMockOf(const ValueContext& value)
{
    defineTest(value, callWithoutSite<Condition>(RuntimeOperation::SeesMockOf, { moduleIndex(value->module()), moduleIndex(value->referrer()), moduleIndex(value->reader()), word32(value->keepingOriginalNames()) }));
}

void ModuleEmitter::generateIsLinkedBelow(const ValueContext& value)
{
    defineTest(value, callWithoutSite<Condition>(RuntimeOperation::IsLinkedBelow, { moduleIndex(value->module()), moduleIndex(value->referrer()) }));
}

void ModuleEmitter::generateMockSeenBy(const ValueContext& value)
{
    define(value, callWithoutSite<Boxed>(RuntimeOperation::MockSeenBy, { moduleIndex(value->module()), moduleIndex(value->referrer()), moduleIndex(value->reader()) }));
}

void ModuleEmitter::generateNamespaceSeenBy(const ValueContext& value)
{
    define(value, callOperation<Boxed>(value, RuntimeOperation::NamespaceSeenBy, { moduleIndex(value->module()), moduleIndex(value->referrer()), moduleIndex(value->reader()) }));
}

void ModuleEmitter::generateInstanceNamespace(const ValueContext& value)
{
    define(value, callOperation<Boxed>(value, RuntimeOperation::InstanceNamespace, { in<CellPointer>(value->instanceOperand()), moduleIndex(value->module()), word32(static_cast<int32_t>(value->phase())) }));
}

void ModuleEmitter::generateInstanceImportMeta(const ValueContext& value)
{
    define(value, callOperation<Boxed>(value, RuntimeOperation::InstanceImportMeta, { in<CellPointer>(value->instanceOperand()), moduleIndex(value->module()) }));
}

void ModuleEmitter::generateOriginalArgument(const ValueContext& value)
{
    define(value, callWithoutSite<Boxed>(RuntimeOperation::OriginalArgument, { moduleIndex(value->module()), word32(static_cast<int32_t>(value->index())) }));
}

void ModuleEmitter::generateLinkedMock(const ValueContext& value)
{
    define(value, callWithoutSite<Boxed>(RuntimeOperation::LinkedMock, { in<Boxed>(value->wordOperand()) }));
}

void ModuleEmitter::generateKeepsOriginalNames(const ValueContext& value)
{
    defineTest(value, callWithoutSite<Condition>(RuntimeOperation::KeepsOriginalNames, { in<Boxed>(value->wordOperand()) }));
}

void ModuleEmitter::generateHasNameInMock(const ValueContext& value)
{
    defineTest(value, callOperation<Condition>(value, RuntimeOperation::HasNameInMock, { in<Boxed>(value->wordOperand()), identifierNumber(value->key()), word32(value->outsideCycles()) }));
}

void ModuleEmitter::generateCountExecution(const ValueContext& value)
{
    Operand arguments[] { moduleIndex(value->module()), numberOf(m_mentions.add(NumberSpace::String, value->description())), word32(static_cast<int32_t>(value->counter())) };
    callWithoutSite(RuntimeOperation::CountExecution, arguments, Representation::Void, { });
}

void ModuleEmitter::generateResolveModule(const ValueContext& value)
{
    Word32 specifier = numberOf(m_mentions.add(NumberSpace::String, value->specifier()));
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
    if (std::optional<Boxed> requirerObject = inOptional<Boxed>(value->requirerObjectOperand())) {
        define(value, callOperation<Boxed>(value, RuntimeOperation::RequireDynamicWithRequirer, { in<Boxed>(value->specifierOperand()), moduleIndex(value->referrer()), *requirerObject }));
        return;
    }
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

Condition ModuleEmitter::hasBegun(ModuleID module)
{
    static_assert(StoredModuleStatus::InRecord < StoredModuleStatus::Evaluated && StoredModuleStatus::Evaluated < StoredModuleStatus::Evaluating);
    return above(Word32(m_out.load8ZeroExt32(statusOf(module))), word32(static_cast<int32_t>(StoredModuleStatus::InRecord)));
}

void ModuleEmitter::generateBeginModuleEvaluation(const ValueContext& value)
{
    static_assert(StoredModuleStatus::Unlinked < StoredModuleStatus::InRecord && StoredModuleStatus::Linked < StoredModuleStatus::InRecord);
    static_assert(StoredModuleStatus::InRecord < StoredModuleStatus::Evaluated && StoredModuleStatus::Evaluated < StoredModuleStatus::Evaluating);
    ModuleID module = value->module();
    Word32 inRecord = word32(static_cast<int32_t>(StoredModuleStatus::InRecord));
    Word32 status(m_out.load8ZeroExt32(statusOf(module)));
    bool isComparedWithDisk = ModuleEvaluationPlan::beginAsksRunTime(m_program, module);
    defineTest(value, selectByBranch<Condition>(belowOrEqual(status, inRecord), [&] {
        auto askRunTime = [&] {
            return callOperation<Condition>(value, RuntimeOperation::BeginModuleEvaluation, { moduleIndex(module) }, { .isOnRarePath = true });
        };
        if (isComparedWithDisk)
            return askRunTime();
        return either<Condition>(notEqual(status, inRecord), [&] {
            Word32 evaluatingMark = loadMutable32(AOT_INSTANCE_OFFSET(modules.evaluatingMark));
            m_out.store32As8(evaluatingMark.value(), statusOf(module));
            return boolean(true);
        }, askRunTime);
    }, [&] {
        return boolean(false);
    }));
}

void ModuleEmitter::generateFinishModuleEvaluation(const ValueContext& value)
{
    ModuleID module = value->module();
    LValue evaluated = m_out.constInt32(static_cast<int32_t>(StoredModuleStatus::Evaluated));
    if (!m_plan.moduleEvaluation().finishLeavesStatusInRecord(m_program, module)) {
        m_out.store32As8(evaluated, statusOf(module));
        return;
    }
    Word32 status(m_out.load8ZeroExt32(statusOf(module)));
    ifElse(equal(status, word32(static_cast<int32_t>(StoredModuleStatus::InRecord))), [] { }, [&] {
        m_out.store32As8(evaluated, statusOf(module));
    });
}

void ModuleEmitter::generateEnterBodyThatRunsOnce(const ValueContext& value)
{
    callVoidOperation(value, RuntimeOperation::EnterBodyThatRunsOnce, { word32(static_cast<int32_t>(value->functionNumber())) });
}

void ModuleEmitter::generateEvaluateRequiredModule(const ValueContext& value)
{
    ModuleID module = value->module();
    mentionRequireEdge(value);
    auto askRunTime = [&](const CallOptions& options) {
        callOperation<Boxed>(value, RuntimeOperation::RequireModule, { moduleIndex(module), moduleIndex(ModuleID()) }, options);
    };
    if (m_plan.importsWhatMayBeMocked(module)) {
        askRunTime({ });
        return;
    }
    Word32 status(m_out.load8ZeroExt32(statusOf(module)));
    ifElse(equal(status, word32(static_cast<int32_t>(StoredModuleStatus::Evaluated))), [] { }, [&] {
        askRunTime({ .isOnRarePath = true });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
