/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsCheck.h"

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTIR.h"
#include "AOTVerifier.h"
#include <algorithm>
#include <span>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static void addGroupMember(EffectsOnEdges& group, const EffectsOnEdges& member)
{
    group.merge(member);
    if (group.mayThrow())
        group.handler.merge(group.normal);
}

void ReplacedEffects::add(const EffectsOnEdges& effects, Opcode operation)
{
    addGroupMember(m_effects, effects);
    if (!m_operations.contains(operation))
        m_operations.append(operation);
}

void ReplacedEffects::add(const EffectContext& context, const Function& function, const Value& value)
{
    add(effectsOf(context, function, value), value.opcode);
}

void ReplacedEffects::add(const EffectContext& context, const Function& function, const Terminator& terminator)
{
    addGroupMember(m_effects, effectsOf(context, function, terminator));
    if (!m_terminators.contains(terminator.kind))
        m_terminators.append(terminator.kind);
}

namespace {

struct ReplacementReport {
    const Program& program;
    const Function& function;
    const ReplacementSite& site;
    ASCIILiteral replacement;
    Origin origin;
    Diagnostics& diagnostics;

    void add(InternalErrorKind, const EffectsOnEdges&, const ReplacedEffects&) const;
    void addGroupBeyondFusedOperation(const EffectsOnEdges&, const ReplacedEffects&) const;
    void add(InternalErrorKind, String&& detail) const;
};

} // anonymous namespace

static LocationMasks locationsBeyond(const LocationMasks& locations, const LocationMasks& allowed)
{
    LocationMasks beyond;
    for (unsigned index = 0; index < numberOfHeaps; ++index) {
        auto heap = static_cast<AbstractHeap>(index);
        beyond.add(heap, locations.maskOf(heap) & ~allowed.maskOf(heap));
    }
    return beyond;
}

static Effects effectsBeyond(const Effects& effects, const Effects& allowed)
{
    return {
        .reads = locationsBeyond(effects.reads, allowed.reads),
        .writes = locationsBeyond(effects.writes, allowed.writes),
        .mayThrow = effects.mayThrow && !allowed.mayThrow,
        .mayCollect = effects.mayCollect && !allowed.mayCollect,
        .mayCallUnknownCode = effects.mayCallUnknownCode && !allowed.mayCallUnknownCode,
        .maySuspend = effects.maySuspend && !allowed.maySuspend,
        .mayBeTerminated = effects.mayBeTerminated && !allowed.mayBeTerminated,
    };
}

static String describeReplacement(ASCIILiteral replacement, const EffectsOnEdges& effects, const ReplacedEffects& replaced)
{
    StringPrintStream out;
    out.print(replacement, " replaced "_s);
    CommaPrinter comma;
    for (Opcode operation : replaced.operations())
        out.print(comma, nameOf(operation));
    for (TerminatorKind terminator : replaced.terminators())
        out.print(comma, nameOf(terminator));
    if (!comma.didPrint())
        out.print("operations without effects"_s);

    const EffectsOnEdges& allowed = replaced.effects();
    out.print("; beyond the group: "_s, effectsBeyond(effects.normal, allowed.normal));
    if (!effects.handler.isSubsetOf(allowed.handler))
        out.print("; beyond it where it throws: "_s, effectsBeyond(effects.handler, allowed.handler));
    out.print("; the group: "_s, allowed);
    return out.toString();
}

void ReplacementReport::add(InternalErrorKind kind, const EffectsOnEdges& effects, const ReplacedEffects& replaced) const
{
    add(kind, describeReplacement(replacement, effects, replaced));
}

void ReplacementReport::addGroupBeyondFusedOperation(const EffectsOnEdges& effects, const ReplacedEffects& replaced) const
{
    const EffectsOnEdges& group = replaced.effects();
    add(InternalErrorKind::EffectsOfPreparedTooNarrow, makeString(replacement, " does all that the group it replaced did; the group beyond it: "_s, toString(effectsBeyond(group.normal, effects.normal)), "; where it throws: "_s, toString(effectsBeyond(group.handler, effects.handler))));
}

void ReplacementReport::add(InternalErrorKind kind, String&& detail) const
{
    InternalError error { .kind = kind, .location = function.locationOf(origin), .detail = WTF::move(detail) };
    if (ProvisionalFunctions::contains(site.function)) {
        error.function = findFunctionAsReadOfProvisional(program, function);
    } else {
        error.function = site.function;
        error.block = site.block;
        error.value = site.value;
    }
    diagnostics.add(error);
}

template<typename Entry>
static bool mayReplace(Entry replacement, const ReplacedEffects& replaced)
{
    std::span<const Opcode> operations = replacedOperationsOf(replacement);
    std::span<const TerminatorKind> terminators = replacedTerminatorsOf(replacement);
    return std::ranges::all_of(replaced.operations(), [&](Opcode operation) { return std::ranges::contains(operations, operation); })
        && std::ranges::all_of(replaced.terminators(), [&](TerminatorKind terminator) { return std::ranges::contains(terminators, terminator); });
}

static void checkReplacementEffects(const EffectsOnEdges& effects, bool isNamedReplacement, const ReplacedEffects& replaced, const ReplacementReport& report)
{
    if (effects == EffectsOnEdges::none())
        return;
    if (!isNamedReplacement)
        report.add(InternalErrorKind::EffectsOfPreparedTooNarrow, effects, replaced);
    if (!effects.isSubsetOf(replaced.effects()))
        report.add(InternalErrorKind::EffectsExceedReplacedGroup, effects, replaced);
}

void checkReplacement(const EffectContext& context, const Function& function, const ReplacementSite& site, const Value& replacement, const ReplacedEffects& replaced, Diagnostics& diagnostics)
{
    if (familyOf(replacement.opcode) == OperationFamily::RuntimeValidation)
        return;
    EffectsOnEdges effects = effectsOf(context, function, replacement);
    ReplacementReport report { context.program, function, site, nameOf(replacement.opcode), replacement.origin(), diagnostics };
    checkReplacementEffects(effects, mayReplace(replacement.opcode, replaced), replaced, report);
    // A fused access leaves out nothing of its group.
    bool doesAllOfGroup = familyOf(replacement.opcode) == OperationFamily::FusedAccess && declaredBoundOf(replacement.opcode).mayCallUnknownCode;
    if (doesAllOfGroup && !replaced.effects().isSubsetOf(effects))
        report.addGroupBeyondFusedOperation(effects, replaced);
}

void checkReplacement(const EffectContext& context, const Function& function, const ReplacementSite& site, const Terminator& replacement, const ReplacedEffects& replaced, Diagnostics& diagnostics)
{
    checkReplacementEffects(effectsOf(context, function, replacement), mayReplace(replacement.kind, replaced), replaced, { context.program, function, site, nameOf(replacement.kind), replacement.origin(), diagnostics });
}

template<typename Entry>
static Effects fullBoundOf(Entry entry)
{
    Effects bound = declaredBoundOf(entry);
    if (mayRunOperandCode(entry))
        bound.merge(Effects::unknownCode());
    return bound;
}

template<typename Entry>
static bool liesWithinReplacedEntries(Entry entry)
{
    std::span<const Opcode> operations = replacedOperationsOf(entry);
    std::span<const TerminatorKind> terminators = replacedTerminatorsOf(entry);
    if (operations.empty() && terminators.empty())
        return true;
    Effects allowed = Effects::none();
    for (Opcode operation : operations)
        allowed.merge(fullBoundOf(operation));
    for (TerminatorKind terminator : terminators)
        allowed.merge(fullBoundOf(terminator));
    Effects bound = fullBoundOf(entry);
    // checkReplacement() sees which operands a replacement is given.
    if constexpr (std::is_same_v<Entry, Opcode>)
        bound.writes.remove(heapsWrittenForOptionalOperands(entry));
    return bound.isSubsetOf(allowed);
}

template<typename Entry>
static void checkEntries(Diagnostics& diagnostics)
{
    for (Entry entry : allMembersOf<Entry>) {
        if (!liesWithinReplacedEntries(entry))
            diagnostics.add(InternalError { .kind = InternalErrorKind::EffectsOfPreparedTooNarrow, .count = std::to_underlying(entry) });
    }
}

void checkDeclaredEffects(Diagnostics& diagnostics)
{
    checkEntries<Opcode>(diagnostics);
    checkEntries<TerminatorKind>(diagnostics);
}

void checkSummaryCoversBody(const EffectContext& context, FunctionID id, FunctionAnalyses& analyses, Diagnostics& diagnostics)
{
    FunctionSummary fresh = summarizeBody(context, id, analyses);
    EffectsOnEdges claimed = context.summaries.of(id).onCall();
    Effects onAnyEdge = claimed.onAnyEdge();
    bool coversParameters = fresh.throughParameters.reads.isSubsetOf(onAnyEdge.reads) && fresh.throughParameters.writes.isSubsetOf(onAnyEdge.writes);
    auto withMemoryOfObjectsOnAnyEdge = [&](Effects effects) {
        effects.reads.merge(onAnyEdge.reads.restrictedTo(perObjectHeaps));
        effects.writes.merge(onAnyEdge.writes.restrictedTo(perObjectHeaps));
        return effects;
    };
    claimed = { withMemoryOfObjectsOnAnyEdge(claimed.normal), withMemoryOfObjectsOnAnyEdge(claimed.handler) };
    if (fresh.elsewhere.isSubsetOf(claimed) && coversParameters)
        return;

    StringPrintStream detail;
    detail.print("beyond it: "_s, effectsBeyond(fresh.elsewhere.normal, claimed.normal));
    if (!fresh.elsewhere.handler.isSubsetOf(claimed.handler))
        detail.print("; where it throws: "_s, effectsBeyond(fresh.elsewhere.handler, claimed.handler));
    if (!coversParameters) {
        Effects throughParameters = Effects::none();
        throughParameters.reads = fresh.throughParameters.reads;
        throughParameters.writes = fresh.throughParameters.writes;
        detail.print("; through parameters: "_s, effectsBeyond(throughParameters, onAnyEdge));
    }
    diagnostics.add(InternalError { .kind = InternalErrorKind::EffectsExceedSummary, .function = id, .detail = detail.toString() });
}

static Effects effectsOfEntering(const Program& program, FunctionID target, BuiltinCallMode mode)
{
    CalleeList callees = CalleeList::none();
    callees.add(target);
    EffectDescription description;
    describeCallees(program, callees, mode, description);
    return description.own.onAnyEdge();
}

static bool entersTarget(const Function& entrypoint)
{
    return std::ranges::any_of(entrypoint.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            return effectRuleOf(entrypoint.at(id).opcode) == EffectRule::Call;
        });
    });
}

void checkCallByValueCoversEntrypoint(const Program& program, FunctionID target, const Function& entrypoint, Diagnostics& diagnostics)
{
    if (!entersTarget(entrypoint))
        return;

    EffectSummaries unknownSummaries;
    EffectContext context { program, unknownSummaries };
    bool takesNewTarget = entrypoint.creator.as<CreatorRecord::Entrypoint>().takesNewTarget;
    Effects claimed = effectsOfEntering(program, target, takesNewTarget ? BuiltinCallMode::Construct : BuiltinCallMode::Call);
    for (unsigned index = 0; index < entrypoint.blocks.size(); ++index) {
        for (ValueID id : entrypoint.blocks[index].values) {
            const Value& value = entrypoint.at(id);
            if (effectRuleOf(value.opcode) == EffectRule::Call || familyOf(value.opcode) == OperationFamily::RuntimeValidation)
                continue;
            if (value.opcode == Opcode::GetConstructPrototype)
                continue;
            if (!effectsOf(context, entrypoint, value).onAnyEdge().isSubsetOf(claimed))
                diagnostics.add(InternalError { .kind = InternalErrorKind::EffectsExceedSummary, .function = target, .block = BlockID(index), .value = id });
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
