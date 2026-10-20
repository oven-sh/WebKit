/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectSummaries.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectSummariesInternal.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIR.h"
#include "AOTThreads.h"
#include <wtf/CommaPrinter.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT { namespace Typed {

WTF_MAKE_TZONE_ALLOCATED_IMPL(EffectSummaries);

const ParameterSummary& FunctionSummary::parameter(uint32_t entryParameter) const
{
    static constexpr ParameterSummary unknownParameter { };
    return entryParameter < parameters.size() ? parameters[entryParameter] : unknownParameter;
}

EffectsOnEdges FunctionSummary::onCall() const
{
    EffectsOnEdges effects = elsewhere;
    effects.normal.reads.merge(throughParameters.reads);
    effects.normal.writes.merge(throughParameters.writes);
    if (elsewhere.mayThrow()) {
        effects.handler.reads.merge(throughParameters.reads);
        effects.handler.writes.merge(throughParameters.writes);
    }
    return effects;
}

EffectSummaries::EffectSummaries(ParallelArray<FunctionSummary>&& summaries, const Effects& unknownCode)
    : m_summaries(WTF::move(summaries))
    , m_unknownCode(unknownCode)
{
}

const FunctionSummary& EffectSummaries::of(FunctionID id) const
{
    if (id && id.index() < m_summaries.size())
        return m_summaries[id.index()];
    return m_summaryOfUnknownFunction;
}

bool describeByRole(const Program& program, const Function& function, EffectDescription& description)
{
    const BuiltinTable& table = program.builtins();
    switch (function.role) {
    case FunctionRole::Serializer: {
        LayoutID layout = function.creator.as<CreatorRecord::Serializer>().layout;
        if (!layout)
            return false;
        describeOverload(program, table.overloadIDsOf(Builtins::JSONStringify).at(0), Type::forLayoutSet(program.declaredMembersOf(layout)), description);
        for (Opcode row : { Opcode::JSONHasStackSpace, Opcode::StringBuilderReserve, Opcode::StringBuilderCommit, Opcode::StringBuilderCheckpoint, Opcode::StringBuilderRollBack, Opcode::StringBuilderAppend, Opcode::JSONAppendString })
            description.addOwn(declaredBoundOf(row));
        return true;
    }
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
        describeOverload(program, table.overloadIDsOf(Builtins::JSONParse).at(0), program.at(TypeTable::string), description);
        for (Opcode row : { Opcode::JSONHasStackSpace, Opcode::JSONParseValue })
            description.addOwn(declaredBoundOf(row));
        return true;
    case FunctionRole::ChainStep:
        description.addFunction(function.derivation.source);
        description.addOwn(declaredBoundOf(Opcode::NewArray));
        return true;
    default:
        return false;
    }
}

// A built-in reads whatever its operand reaches, and the table need not say so: nothing moves a store across a call of it.
static FunctionSummary summarizeByRole(const EffectContext& context, const Function& function, const EffectDescription& description)
{
    EffectsOnEdges effects = resolveEffects(context, description);
    effects.normal.reads.merge(LocationMasks { perObjectHeaps });
    if (effects.mayThrow())
        effects.handler.reads.merge(LocationMasks { perObjectHeaps });

    ParameterSummary parameter;
    parameter.memory.writes = effects.onAnyEdge().writes.restrictedTo(perObjectHeaps);
    Vector<ParameterSummary> parameters(function.numberOfEntryParameters(), [&](size_t) {
        return parameter;
    });
    return createFunctionSummary(effects.normal, effects.handler, WTF::move(parameters), { });
}

CalleeSummary::CalleeSummary(const EffectContext& context, FunctionID id)
    : m_computed(&context.summaries.of(id))
{
    if (!id || context.summaries.contains(id))
        return;
    const Function& function = context.program.at(id);
    EffectDescription description;
    if (describeByRole(context.program, function, description))
        m_summaryByRole = summarizeByRole(context, function, description);
}

static void dumpParameter(PrintStream& out, unsigned index, const ParameterSummary& parameter)
{
    out.print(" p"_s, index, ": "_s);
    if (parameter == ParameterSummary()) {
        out.print("?"_s);
        return;
    }
    out.print("["_s);
    CommaPrinter comma;
    for (OperandUse use : allMembersOf<OperandUse>) {
        if (parameter.operand.uses.contains(use))
            out.print(comma, nameOf(use));
    }
    out.print("]"_s);
    if (parameter.operand.uses.contains(OperandUse::Retains)) {
        switch (parameter.operand.retainedIn) {
        case RetainedIn::UnknownMemory:
            break;
        case RetainedIn::Result:
            out.print(" in result"_s);
            break;
        case RetainedIn::Operand:
            out.print(" in p"_s, parameter.operand.retainingOperand);
            break;
        }
    }
    if (!parameter.memory.reads.isEmpty())
        out.print(" reads("_s, parameter.memory.reads, ")"_s);
    if (!parameter.memory.writes.isEmpty())
        out.print(" writes("_s, parameter.memory.writes, ")"_s);
}

void EffectSummaries::dump(PrintStream& out, const Program& program) const
{
    out.println("unknown code: "_s, m_unknownCode);
    for (unsigned i = 0; i < program.functions.size(); ++i) {
        if (!program.functions[i].hasBody())
            continue;
        const FunctionSummary& summary = of(FunctionID(i));
        out.print("f"_s, i, " return: "_s, summary.elsewhere.normal, " throw: "_s, summary.elsewhere.handler);
        for (unsigned parameter = 0; parameter < summary.parameters.size(); ++parameter)
            dumpParameter(out, parameter, summary.parameters[parameter]);
        if (summary.unknownCodeWitness) {
            out.print(" unknown code from f"_s, summary.unknownCodeWitness.function.index());
            if (summary.unknownCodeWitness.value)
                out.print(" v"_s, summary.unknownCodeWitness.value.index());
        }
        out.println();
    }
}

EffectSummaries createSummariesFromFlow(const Program& program, const FlowResults& flow, BuildContext& build)
{
    ParallelArray<FunctionSummary> summaries(build.parallelLoops(), program.functions.size(), [&](unsigned i) {
        FunctionSummary summary;
        summary.parameters.grow(program.functions[i].numberOfEntryParameters());
        for (unsigned parameter = 0; parameter < summary.parameters.size(); ++parameter)
            summary.parameters[parameter].operand = publishedParameterSummary(flow, FunctionID(i), parameter);
        return summary;
    });
    return EffectSummaries(WTF::move(summaries), Effects::unknownCode());
}

EffectSummaries computeEffectSummaries(const Program& program, const FlowResults& flow, BuildContext& build, BodySummaries* kept)
{
    return EffectSummarizer(program, flow, build, kept).run();
}

FunctionSummary createFunctionSummary(Effects onReturn, Effects onThrow, Vector<ParameterSummary>&& parameters, UnknownCodeWitness witness)
{
    onReturn = onReturn.withoutSuspension();
    onThrow = onThrow.withoutSuspension();

    bool mayThrow = onReturn.mayThrow || onThrow.mayThrow;
    bool mayCollect = onReturn.mayCollect || onThrow.mayCollect;
    bool mayBeTerminated = onReturn.mayBeTerminated || onThrow.mayBeTerminated;
    for (Effects* effects : { &onReturn, &onThrow }) {
        effects->mayThrow = mayThrow;
        effects->mayCollect = mayCollect;
        effects->mayBeTerminated = mayBeTerminated;
    }
    if (!mayThrow)
        onThrow = Effects::none();

    FunctionSummary summary;
    summary.elsewhere = { onReturn, onThrow };
    summary.throughParameters = OperandMemory::none();
    for (const ParameterSummary& parameter : parameters)
        summary.throughParameters.merge(parameter.memory);
    summary.parameters = WTF::move(parameters);
    if (onReturn.mayCallUnknownCode || onThrow.mayCallUnknownCode)
        summary.unknownCodeWitness = witness;
    return summary;
}

static Effects effectsOfMemory(const OperandMemory& memory)
{
    Effects effects = Effects::none();
    effects.reads = memory.reads;
    effects.writes = memory.writes;
    return effects;
}

static Effects valueOf(const EffectContext& context, const SummaryNode& node)
{
    switch (node.part) {
    case SummaryPart::OnReturn:
        return context.summaries.of(node.function).elsewhere.normal;
    case SummaryPart::OnThrow:
        return context.summaries.of(node.function).elsewhere.handler;
    case SummaryPart::Parameter:
        return effectsOfMemory(context.summaries.of(node.function).parameter(node.parameter).memory);
    case SummaryPart::UnknownCode:
        return context.summaries.unknownCode();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Effects valueOf(const BodySummary& body, const SummaryNode& node)
{
    switch (node.part) {
    case SummaryPart::OnReturn:
        return body.onReturn;
    case SummaryPart::OnThrow:
        return body.onThrow;
    case SummaryPart::Parameter:
        return effectsOfMemory(body.parameters[node.parameter].memory);
    case SummaryPart::UnknownCode:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool mergeInto(BodySummary& body, const SummaryNode& target, const Effects& source)
{
    switch (target.part) {
    case SummaryPart::OnReturn:
        return body.onReturn.merge(source);
    case SummaryPart::OnThrow:
        return body.onThrow.merge(source);
    case SummaryPart::Parameter: {
        OperandMemory& memory = body.parameters[target.parameter].memory;
        bool changed = memory.reads.merge(source.reads.restrictedTo(perObjectHeaps));
        changed |= memory.writes.merge(source.writes.restrictedTo(perObjectHeaps));
        return changed;
    }
    case SummaryPart::UnknownCode:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static UnknownCodeWitness witnessOf(const EffectContext& context, FunctionID callee)
{
    UnknownCodeWitness witness = context.summaries.of(callee).unknownCodeWitness;
    return witness ? witness : UnknownCodeWitness { callee, ValueID() };
}

static bool isRecursiveCall(const SummaryDependency& ownDependency)
{
    return ownDependency.source.part == SummaryPart::OnReturn;
}

FunctionSummary summarizeBody(const EffectContext& context, FunctionID id, FunctionAnalyses& analyses)
{
    ASSERT(id);
    BodySummary body = summarizeBodyLocally(context, id, analyses);

    Vector<SummaryDependency, 4> ownDependencies;
    for (const SummaryDependency& dependency : body.dependencies) {
        ASSERT(dependency.target.function == id);
        if (dependency.source.function == id) {
            ownDependencies.append(dependency);
            continue;
        }
        Effects source = valueOf(context, dependency.source);
        mergeInto(body, dependency.target, source);
        if (source.mayCallUnknownCode && !body.unknownCodeWitness)
            body.unknownCodeWitness = witnessOf(context, dependency.source.function);
    }

    for (const SummaryDependency& dependency : ownDependencies) {
        if (isRecursiveCall(dependency))
            body.onReturn.mayThrow = true;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const SummaryDependency& dependency : ownDependencies)
            changed |= mergeInto(body, dependency.target, valueOf(body, dependency.source));
    }

    return createFunctionSummary(body.onReturn, body.onThrow, WTF::move(body.parameters), body.unknownCodeWitness);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
