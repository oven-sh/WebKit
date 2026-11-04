/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStraightEmitter.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTModuleWords.h"
#include "AOTStubsCallAdapters.h"
#include "AOTThinSummary.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

OpcodeSet StraightEmitter::opcodesOfModules()
{
    return {
        Opcode::ModuleNamespace, Opcode::ImportModule, Opcode::RequireModule, Opcode::EvaluateModule, Opcode::RecordRequire,
        Opcode::WasLoadedSynchronously, Opcode::SeesMockOf, Opcode::MockSeenBy, Opcode::AreLoaderFunctionsStored, Opcode::CountExecution,
        Opcode::ResolveModule, Opcode::ThrowModuleFailure, Opcode::ImportDynamic, Opcode::RequireDynamic, Opcode::ResolveDynamic
    };
}

// The plan has read no site out of such a block, and ModuleEvaluationPlan::siteOf() stops the build at one. Nothing enters it.
static bool standsInBlockNeverEnteredInStraightModule(StraightEmitter& emitter)
{
    const Vector<BlockID>& blocks = emitter.facts().summaries().at(emitter.function()).blocksNeverEntered;
    return !blocks.isEmpty() && std::ranges::binary_search(blocks, emitter.currentBlock().id());
}

static StraightArgument moduleIndexInStraightForm(ModuleID module)
{
    return StraightArgument::word(module.index());
}

static StraightArgument operandOrUndefinedInStraightModule(ValueID operand)
{
    return operand ? StraightArgument(operand) : StraightArgument::boxed(jsUndefined());
}

static StraightArgument stringNumberInStraightModule(StraightEmitter& emitter, StringID string)
{
    return StraightArgument::number(emitter.mentions().add(NumberSpace::String, string));
}

static void mentionRequireEdgeInStraightForm(StraightEmitter& emitter, ModuleID referrer, ModuleID module, StringID specifier)
{
    uint32_t key[] { referrer.index(), module.index(), specifier.index() };
    emitter.mentions().add(NumberSpace::RequireEdge, std::span<const uint32_t>(key));
}

static CCallHelpers::TrustedImm32 storedStatusInStraightForm(StoredModuleStatus status)
{
    return CCallHelpers::TrustedImm32(static_cast<int32_t>(status));
}

static CCallHelpers::Address loadAddressOfModuleStatusInStraightForm(StraightEmitter& emitter, ModuleID module)
{
    static_assert(sizeof(StoredModuleStatus) == sizeof(uint8_t));
    GPRReg statuses = StraightEmitter::temporary(0);
    emitter.jit().loadPtr(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(modules.status)), statuses);
    return CCallHelpers::Address(statuses, static_cast<int32_t>(module.index()));
}

static void beginModuleInStraightForm(StraightEmitter& emitter, const BodyValue& value, Origin origin, ModuleID module)
{
    static_assert(StoredModuleStatus::Unlinked < StoredModuleStatus::InRecord && StoredModuleStatus::Linked < StoredModuleStatus::InRecord);
    static_assert(StoredModuleStatus::InRecord < StoredModuleStatus::Evaluated && StoredModuleStatus::Evaluated < StoredModuleStatus::Evaluating);
    CCallHelpers& jit = emitter.jit();
    GPRReg mark = StraightEmitter::temporary(1);
    CCallHelpers::Address status = loadAddressOfModuleStatusInStraightForm(emitter, module);
    CCallHelpers::JumpList isDone;
    isDone.append(jit.branch8(CCallHelpers::Above, status, storedStatusInStraightForm(StoredModuleStatus::InRecord)));
    if (!ModuleEvaluationPlan::beginAsksRunTime(emitter.program(), module)) {
        CCallHelpers::Jump isInRecord = jit.branch8(CCallHelpers::Equal, status, storedStatusInStraightForm(StoredModuleStatus::InRecord));
        jit.load32(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(modules.evaluatingMark)), mark);
        jit.store8(mark, status);
        isDone.append(jit.jump());
        isInRecord.link(&jit);
    }
    emitter.callOperation(value, RuntimeOperation::BeginModuleEvaluation, { moduleIndexInStraightForm(module) }, { .origin = origin });
    isDone.link(&jit);
}

static void finishModuleInStraightForm(StraightEmitter& emitter, ModuleID module)
{
    CCallHelpers& jit = emitter.jit();
    CCallHelpers::Address status = loadAddressOfModuleStatusInStraightForm(emitter, module);
    CCallHelpers::Jump staysInRecord;
    if (emitter.plan().moduleEvaluation().finishLeavesStatusInRecord(emitter.program(), module))
        staysInRecord = jit.branch8(CCallHelpers::Equal, status, storedStatusInStraightForm(StoredModuleStatus::InRecord));
    jit.store8(storedStatusInStraightForm(StoredModuleStatus::Evaluated), status);
    if (staysInRecord.isSet())
        staysInRecord.link(&jit);
}

static void callStartOfModuleInStraightForm(StraightEmitter& emitter, const BodyValue& value, FunctionID body, const StraightCallOptions& options)
{
    const ModuleStart* start = emitter.plan().moduleEvaluation().findStartOfBody(body);
    if (!start || !emitter.conventions().exists({ body, CodeRole::ModuleStart })) {
        emitter.fail(value);
        return;
    }
    emitter.callStub(value, startModule(), { StraightArgument::word(ModuleEvaluationPlan::numberOf(*start)) }, options);
}

// Whoever evaluates a module has nothing to pass but `this`, which is undefined.
static void callBodyOfModuleInStraightForm(StraightEmitter& emitter, const BodyValue& value, FunctionID body, const StraightCallOptions& options)
{
    const Program& program = emitter.program();
    const CallingConventions& conventions = emitter.conventions();
    CodeUnit entered = conventions.directEntryOf(body);
    if (!program.at(body).hasBody() || !conventions.exists(entered)) {
        emitter.fail(value);
        return;
    }
    ArgumentLocations locations = conventions.locationsOf(program, entered);
    if (std::max(conventions.stackWordsReservedByCallersOf(program, entered), locations.numberOfStackWords()) > emitter.wordsOfScratchArea()) {
        emitter.fail(value);
        return;
    }
    for (const ArgumentLocation& location : locations.parameters) {
        if (location.kind == ArgumentLocation::Kind::None)
            continue;
        if (location.kind != ArgumentLocation::Kind::GPR || location.representation != Representation::JSValue) {
            emitter.fail(value);
            return;
        }
        emitter.jit().move(CCallHelpers::TrustedImm64(JSValue::ValueUndefined), location.reg.gpr());
    }
    uint32_t site = emitter.addSite(*options.origin, options, Opcode::CallDirect);
    emitter.noteSiteMayThrow(site);
    emitter.records().stubCalls.callFunction(emitter.jit(), entered, primaryEntry, OptionalIndex { site });
}

static void generateEvaluateModule(StraightEmitter& emitter, const BodyValue& value)
{
    CCallHelpers& jit = emitter.jit();
    const ModuleEvaluationPlan& evaluation = emitter.plan().moduleEvaluation();
    ModuleID module = value.module();
    const Module& target = emitter.program().at(module);
    Origin origin = value.origin();
    origin.isTailCall = false;
    StraightCallOptions options { .origin = origin, .calledBuiltin = value.isCallOfRequire() ? functionOfHostThatRequireCallsFor(target) : BuiltinID() };

    ModuleSitePlan site = evaluation.siteOf(emitter.function(), value.id(), module);
    if (site.asksLoader) {
        mentionRequireEdgeInStraightForm(emitter, value.referrer(), module, target.path);
        CCallHelpers::Jump hasRun;
        if (!emitter.plan().importsWhatMayBeMocked(module))
            hasRun = jit.branch8(CCallHelpers::Equal, loadAddressOfModuleStatusInStraightForm(emitter, module), storedStatusInStraightForm(StoredModuleStatus::Evaluated));
        emitter.callOperation(value, RuntimeOperation::RequireModule, { moduleIndexInStraightForm(module), moduleIndexInStraightForm(ModuleID()) }, options);
        if (hasRun.isSet())
            hasRun.link(&jit);
        return;
    }

    for (const ModuleStep& step : site.steps) {
        if (step.callee && step.calleeIsStart)
            callStartOfModuleInStraightForm(emitter, value, step.callee, options);
        else if (step.callee) {
            if (!evaluation.leavesOutCallOf(step.callee))
                callBodyOfModuleInStraightForm(emitter, value, step.callee, options);
        } else if (step.moduleThatBegins)
            beginModuleInStraightForm(emitter, value, origin, step.moduleThatBegins);
        else {
            RELEASE_ASSERT(step.moduleThatHasRun);
            beginModuleInStraightForm(emitter, value, origin, step.moduleThatHasRun);
            finishModuleInStraightForm(emitter, step.moduleThatHasRun);
        }
    }
}

static void generateRequireModule(StraightEmitter& emitter, const BodyValue& value)
{
    ModuleID module = value.module();
    ModuleID referrer = value.referrer();
    mentionRequireEdgeInStraightForm(emitter, referrer, module, value.specifier());
    if (value.ifHeldForImport())
        emitter.callOperation(value, RuntimeOperation::RequireIfHeldForImport, { moduleIndexInStraightForm(module), moduleIndexInStraightForm(referrer) });
    else
        emitter.callOperation(value, RuntimeOperation::RequireModule, { moduleIndexInStraightForm(module), moduleIndexInStraightForm(referrer) }, { .calledBuiltin = functionOfHostThatRequireCallsFor(emitter.program().at(module)) });
    emitter.define(value);
}

void StraightEmitter::generateModule(const BodyValue& value)
{
    if (standsInBlockNeverEnteredInStraightModule(*this))
        return;
    switch (value.opcode()) {
    case Opcode::ModuleNamespace:
        if (value.ofModuleItself())
            callOperation(value, RuntimeOperation::GetNamespaceOfModuleItself, { moduleIndexInStraightForm(value.module()) });
        else
            callOperation(value, RuntimeOperation::GetModuleNamespace, { moduleIndexInStraightForm(value.module()), StraightArgument::word(static_cast<uint32_t>(value.phase())) });
        define(value);
        return;
    case Opcode::ImportModule:
        callOperation(value, RuntimeOperation::ImportModule, { moduleIndexInStraightForm(value.module()), operandOrUndefinedInStraightModule(value.optionsOperand()), moduleIndexInStraightForm(value.referrer()) });
        define(value);
        return;
    case Opcode::RequireModule:
        generateRequireModule(*this, value);
        return;
    case Opcode::EvaluateModule:
        generateEvaluateModule(*this, value);
        return;
    case Opcode::RecordRequire:
        if (!plan().moduleEvaluation().recordsNothingThatIsSeen(value.module()))
            callOperation(value, RuntimeOperation::RecordRequire, { moduleIndexInStraightForm(value.module()), moduleIndexInStraightForm(value.referrer()) });
        return;
    case Opcode::WasLoadedSynchronously:
        callOperation(value, RuntimeOperation::WasLoadedSynchronously, { moduleIndexInStraightForm(value.module()) });
        defineBoolean(value);
        return;
    case Opcode::SeesMockOf:
        callOperation(value, RuntimeOperation::SeesMockOf, { moduleIndexInStraightForm(value.module()), moduleIndexInStraightForm(value.referrer()) });
        defineBoolean(value);
        return;
    case Opcode::MockSeenBy:
        callOperation(value, RuntimeOperation::MockSeenBy, { moduleIndexInStraightForm(value.module()), moduleIndexInStraightForm(value.referrer()) });
        define(value);
        return;
    case Opcode::AreLoaderFunctionsStored:
        callOperation(value, RuntimeOperation::AreLoaderFunctionsStored, { });
        defineBoolean(value);
        return;
    case Opcode::CountExecution:
        callOperation(value, RuntimeOperation::CountExecution, { moduleIndexInStraightForm(value.module()), stringNumberInStraightModule(*this, value.description()), StraightArgument::word(value.counter()) });
        return;
    case Opcode::ResolveModule:
        callOperation(value, RuntimeOperation::ResolveModule, { moduleIndexInStraightForm(value.module()), moduleIndexInStraightForm(value.referrer()), stringNumberInStraightModule(*this, value.specifier()), StraightArgument::word(value.isURL()) });
        define(value);
        return;
    case Opcode::ThrowModuleFailure:
        callOperation(value, RuntimeOperation::ThrowModuleFailure, { StraightArgument::word(value.failure().index()), StraightArgument::word(value.isForRequire()) }, { .neverReturns = true });
        return;
    case Opcode::ImportDynamic:
        callOperation(value, RuntimeOperation::ImportDynamic, { value.specifierOperand(), operandOrUndefinedInStraightModule(value.optionsOperand()), moduleIndexInStraightForm(value.referrer()) });
        define(value);
        return;
    case Opcode::RequireDynamic:
        callOperation(value, RuntimeOperation::RequireDynamic, { value.specifierOperand(), moduleIndexInStraightForm(value.referrer()) });
        define(value);
        return;
    case Opcode::ResolveDynamic:
        callOperation(value, RuntimeOperation::ResolveDynamic, { value.specifierOperand(), moduleIndexInStraightForm(value.referrer()), StraightArgument::word(value.isURL()), StraightArgument::word(value.isForRequire()) });
        define(value);
        return;
    default:
        fail(value);
        return;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
