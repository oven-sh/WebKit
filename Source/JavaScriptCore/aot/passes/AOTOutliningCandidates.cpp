/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOutliningCandidates.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTable.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTDecidedBranches.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardFactDetails.h"
#include "AOTHeat.h"
#include "AOTOutliningNormalForm.h"
#include "AOTPass.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class BackwardExtension : uint8_t { ConstantsOnly, UsedValues, AllValues };

struct FinderInputs {
    AnalysisContext contextOf(FunctionID id) const { return { build, program, &inputs.flow, &inputs.effects, id }; }

    const Program& program;
    const OptimizationInputs& inputs;
    BuildContext& build;
};

class GuardsInUse {
    WTF_MAKE_NONCOPYABLE(GuardsInUse);
public:
    explicit GuardsInUse(FunctionAnalyses&);

    bool isAnyLiveAt(ProgramPoint);
    bool isAnyLiveOnEdge(const Target&);

private:
    bool contains(ValueID id) const { return id && m_guards.quickGet(id.index()); }
    bool isAnyLiveAtHead(BlockID);
    const Liveness& liveness();
    uint32_t findEndOfUses(BlockID);

    const Function& m_function;
    const CFG& m_cfg;
    const UseLists& m_useLists;
    BitVector m_guards;
    bool m_hasGuards { false };
    std::optional<Liveness> m_liveness;
    Vector<std::optional<uint32_t>> m_endOfUses;
};

// What another rule wants in the function where it stands.
// JSONObjectFinish takes an object that its own function allocated, so neither of the two leaves without the other.
// A guard on an allocation lasts across unknown code while nothing else holds the object, which is seen only where the allocation is.
BitVector findValuesBoundToFunction(FunctionAnalyses& analyses)
{
    const Function& function = analyses.function();
    BitVector bound(function.values.size());
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (value.opcode == Opcode::JSONObjectFinish) {
                bound.quickSet(id.index());
                for (ValueID link : CastChain(function, value.objectOperand()))
                    bound.quickSet(link.index());
                continue;
            }
            if (!provesForStretch(provesOf(value.opcode)) || !value.numberOfOperands() || !value.operand(subjectOperandIndex))
                continue;
            ValueID subject = analyses.identityOf(value.operand(subjectOperandIndex));
            if (freshnessOf(analyses.effectContext(), function, function.at(subject)) != ResultFreshness::Unknown)
                bound.quickSet(subject.index());
            // The subject may be one of several allocations that meet in a block parameter, or that were read out of another one.
            const EscapeAnalysis& escape = analyses.escape();
            for (ObjectID object : escape.pointsTo(subject).objects)
                bound.quickSet(escape.at(object).definition.index());
        }
    }
    return bound;
}

struct FunctionScan {
    FunctionScan(const FinderInputs& finder, FunctionID id, FunctionAnalyses& analyses)
        : program(finder.program)
        , build(finder.build)
        , id(id)
        , analyses(analyses)
        , function(analyses.function())
        , cfg(analyses.cfg())
        , useLists(analyses.useLists())
        , subject(analyses.subject())
        , framePointers(function)
        , guards(analyses)
        , valuesBoundToFunction(findValuesBoundToFunction(analyses))
        , valuesInSites(function.values.size())
        , blocksInSites(function.blocks.size())
    {
    }

    const Program& program;
    BuildContext& build;
    FunctionID id;
    FunctionAnalyses& analyses;
    const Function& function;
    const CFG& cfg;
    const UseLists& useLists;
    Subject subject;
    FramePointers framePointers;
    GuardsInUse guards;
    BitVector valuesBoundToFunction;
    BitVector valuesInSites;
    BitVector blocksInSites;
};

struct ValueRange {
    BlockID block;
    uint32_t begin { 0 };
    uint32_t end { 0 };
};

struct SiteRange {
    uint32_t begin { 0 };
    uint32_t end { 0 };
    bool hasUnrepeatedSite { false };
};

} // anonymous namespace

GuardsInUse::GuardsInUse(FunctionAnalyses& analyses)
    : m_function(analyses.function())
    , m_cfg(analyses.cfg())
    , m_useLists(analyses.useLists())
    , m_guards(guardValuesOf(m_function, m_cfg))
    , m_hasGuards(!m_guards.isEmpty())
    , m_endOfUses(m_function.blocks.size())
{
}

const Liveness& GuardsInUse::liveness()
{
    if (!m_liveness)
        m_liveness.emplace(m_function, m_cfg, m_useLists, LivenessOptions { .trackedValues = &m_guards });
    return *m_liveness;
}

bool GuardsInUse::isAnyLiveAtHead(BlockID block)
{
    return m_hasGuards && !liveness().liveAtHead(block).empty();
}

bool GuardsInUse::isAnyLiveOnEdge(const Target& target)
{
    return isAnyLiveAtHead(target.block) || std::ranges::any_of(target.arguments, [&](ValueID argument) {
        return contains(argument);
    });
}

uint32_t GuardsInUse::findEndOfUses(BlockID id)
{
    const Block& block = m_function.at(id);
    bool isUsedAtEnd = std::ranges::any_of(m_cfg.successors(id), [&](BlockID successor) {
        return isAnyLiveAtHead(successor);
    });
    block.forEachChildOutsideValues([&](ValueID child) {
        isUsedAtEnd |= contains(child);
    });

    uint32_t end = block.values.size();
    if (isUsedAtEnd)
        return end + 1;
    for (; end; --end) {
        bool usesGuard = false;
        m_function.at(block.values[end - 1]).forEachChild([&](ValueID operand) {
            usesGuard |= contains(operand);
        });
        if (usesGuard)
            break;
    }
    return end;
}

bool GuardsInUse::isAnyLiveAt(ProgramPoint point)
{
    if (!m_hasGuards)
        return false;
    std::optional<uint32_t>& end = m_endOfUses[point.block.index()];
    if (!end)
        end = findEndOfUses(point.block);
    return point.index < *end;
}

static bool dependsOnInlineFrame(const Value& value)
{
    return std::ranges::any_of(payloadFieldsOf(value.opcode), [&](const PayloadFieldInfo& field) {
        return field.kind == PayloadKind::Origin && value.payloadWordAt(field.firstWord + 1);
    });
}

static bool dependsOnTopErrorFrame(const Program& program, const Function& function, const Value& value)
{
    if (!builtinCallOpcodes.contains(value.opcode))
        return false;
    if (program.builtins().at(value.overload()).code.implementation.inlineBuiltin != InlineBuiltin::NewError)
        return false;
    const Function& source = program.sourceFunctionOf(function, value);
    return source.isHiddenInStackTraces() && source.enclosingRuntimeFunction;
}

// Flow's candidates for such an operand are found by the access site of the call, and equal operations of several sites share one outlined body.
static bool hasOperandKeyedByAccessSite(const Program& program, const Value& value)
{
    if (!builtinCallOpcodes.contains(value.opcode))
        return false;
    return std::ranges::any_of(program.builtins().at(value.overload()).parameters, [](const BuiltinParameter& parameter) {
        return parameter.roles.contains(BuiltinOperandRole::ContentKey);
    });
}

static bool isCallOfReturnCall(const FunctionScan& scan, ValueID id)
{
    const Terminator& terminator = scan.function.at(scan.useLists.blockOf(id)).terminator;
    return terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == id;
}

// The mark hides the frame that makes the call, which in an outlined body is not the frame of the source function.
static bool isDemotedTailCall(const Value& value)
{
    return FlagMask::isDemotedTailCall(value.opcode) && value.isDemotedTailCall();
}

static bool isMovable(const FunctionScan& scan, ValueID id)
{
    const Value& value = scan.function.at(id);
    if (boundToOf(value.opcode) != BoundTo::Nothing || value.type == TypeTable::never)
        return false;
    if (isCallOfReturnCall(scan, id) || isDemotedTailCall(value))
        return false;
    if (dependsOnInlineFrame(value) || dependsOnTopErrorFrame(scan.program, scan.function, value) || hasOperandKeyedByAccessSite(scan.program, value))
        return false;
    if (hasFrameStorage(value) || scan.valuesBoundToFunction.quickGet(id.index()))
        return false;
    return !scan.analyses.effectsOf(id).normal.maySuspend;
}

static bool canCrossCallBoundary(const Program& program, TypeID type)
{
    return program.at(type).isJavaScriptValue();
}

// No use of an Unspecified value is reached, and an operand or the result of the call of the shared function would be one.
static bool canBeLiveIn(const FunctionScan& scan, ValueID id)
{
    const Value& value = scan.function.at(id);
    return canCrossCallBoundary(scan.program, value.type) && !scan.framePointers.contains(id) && branchesOf(value.opcode) != Branches::Only && !isDeferredClosure(scan.function, id) && !mayBeUnspecified(scan.analyses, id);
}

static BitVector findColdBlocks(const FinderInputs& finder, FunctionAnalyses& analyses)
{
    const BlockCycles& cycles = analyses.blockCycles();
    BitVector coldBlocks(analyses.function().blocks.size());
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        if (!cycles.isInCycle(block) && isCold(heatClassOf(finder.inputs.heat, analyses, block)))
            coldBlocks.quickSet(block.index());
    }
    return coldBlocks;
}

static bool isUseInSite(const FunctionScan& scan, const OutliningSite& site, const Use& use)
{
    bool isInEntryBlock = use.block == site.entryBlock;
    switch (use.kind) {
    case Use::Kind::Operand: {
        if (!isInEntryBlock)
            return site.interiorBlocks.contains(use.block);
        uint32_t index = scan.useLists.locationOf(use.user).index;
        return index >= site.firstIndex && index - site.firstIndex < site.numberOfEntryValues;
    }
    case Use::Kind::TerminatorOperand:
    case Use::Kind::EdgeArgument:
        return isInEntryBlock ? site.isRegion() : site.interiorBlocks.contains(use.block);
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasOnlyUsesInSite(const FunctionScan& scan, const OutliningSite& site, ValueID id)
{
    return std::ranges::all_of(scan.useLists.usesOf(id), [&](const Use& use) {
        return isUseInSite(scan, site, use);
    });
}

static bool isDefinedInSite(const FunctionScan& scan, const OutliningSite& site, ValueID id)
{
    ValueLocation location = scan.useLists.locationOf(id);
    if (location.block != site.entryBlock)
        return site.interiorBlocks.contains(location.block);
    return location.placement == ValuePlacement::InBlock && location.index >= site.firstIndex && location.index - site.firstIndex < site.numberOfEntryValues;
}

static bool anyValueInSite(const Function& function, const OutliningSite& site, const Invocable<bool(ValueID)> auto& matches)
{
    if (std::ranges::any_of(entryValuesOf(function, site), matches))
        return true;
    return std::ranges::any_of(site.interiorBlocks, [&](BlockID block) {
        return std::ranges::any_of(function.at(block).values, matches);
    });
}

static bool namesGuard(const Function& function, const OutliningSite& site)
{
    return anyValueInSite(function, site, [&](ValueID id) {
        const Value& value = function.at(id);
        bool hasGuardOperand = false;
        value.forEachOperand([&](ValueID, uint32_t operandIndex) {
            hasGuardOperand |= value.operandKind(operandIndex) == OperandKind::Guard;
        });
        return hasGuardOperand;
    });
}

static bool namesGuardDefinedOutside(const FunctionScan& scan, const OutliningSite& site)
{
    return anyValueInSite(scan.function, site, [&](ValueID id) {
        const Value& value = scan.function.at(id);
        bool renewsGuards = provesOf(value.opcode) == Proof::EpochCurrent;
        for (uint32_t operandIndex = 0; operandIndex < value.numberOfOperands(); ++operandIndex) {
            ValueID operand = value.operand(operandIndex);
            if (!operand || isDefinedInSite(scan, site, operand))
                continue;
            if (renewsGuards || value.operandKind(operandIndex) == OperandKind::Guard)
                return true;
        }
        return false;
    });
}

static bool runsUnknownCode(const FunctionScan& scan, ValueID id)
{
    const EffectsOnEdges& effects = scan.analyses.effectsOf(id);
    return effects.normal.mayCallUnknownCode && effects.handler.mayCallUnknownCode;
}

static bool alwaysRunsUnknownCode(const FunctionScan& scan, const OutliningSite& site)
{
    const Function& function = scan.function;
    auto anyRunsUnknownCode = [&](std::span<const ValueID> values) {
        return std::ranges::any_of(values, [&](ValueID id) {
            return runsUnknownCode(scan, id);
        });
    };
    if (anyRunsUnknownCode(entryValuesOf(function, site)))
        return true;
    if (!site.isRegion())
        return false;

    BitVector isReachedWithoutUnknownCode(site.interiorBlocks.size());
    auto reachesExit = [&](const Terminator& terminator) {
        for (const Target& target : terminator.targets) {
            if (target.block == site.exitBlock)
                return true;
            size_t interiorIndex = site.interiorBlocks.find(target.block);
            RELEASE_ASSERT(interiorIndex != notFound);
            isReachedWithoutUnknownCode.quickSet(interiorIndex);
        }
        return false;
    };

    if (reachesExit(function.at(site.entryBlock).terminator))
        return false;
    for (size_t index = 0; index < site.interiorBlocks.size(); ++index) {
        const Block& block = function.at(site.interiorBlocks[index]);
        if (!isReachedWithoutUnknownCode.quickGet(index) || anyRunsUnknownCode(block.values.span()))
            continue;
        if (reachesExit(block.terminator))
            return false;
    }
    return true;
}

static bool isAnyGuardLiveAfter(FunctionScan& scan, const OutliningSite& site)
{
    const Function& function = scan.function;
    if (!site.isRegion())
        return scan.guards.isAnyLiveAt({ site.entryBlock, site.firstIndex + site.numberOfEntryValues });

    const std::optional<Target>& handler = function.at(site.entryBlock).handler;
    if (handler && scan.guards.isAnyLiveOnEdge(*handler))
        return true;

    auto exitsWithLiveGuard = [&](BlockID block) {
        return std::ranges::any_of(function.at(block).terminator.targets, [&](const Target& target) {
            return target.block == site.exitBlock && scan.guards.isAnyLiveOnEdge(target);
        });
    };
    return exitsWithLiveGuard(site.entryBlock) || std::ranges::any_of(site.interiorBlocks, exitsWithLiveGuard);
}

static bool mayBeLocalAllocation(const FunctionScan& scan, ValueID id)
{
    const EscapeAnalysis& escape = scan.analyses.escape();
    for (ObjectID object : escape.pointsTo(id).objects) {
        if (escape.at(object).kind == ObjectKind::Allocation)
            return true;
    }
    return false;
}

// Analyses may treat the call as unknown code, and a local allocation passed to it is a parameter in the body, so it no longer counts as unescaped.
static bool mayEndGuardInUse(FunctionScan& scan, const OutliningSite& site, std::span<const ValueID> liveIns)
{
    bool crossesLiveGuard = isAnyGuardLiveAfter(scan, site);
    if (crossesLiveGuard && !alwaysRunsUnknownCode(scan, site))
        return true;
    if (!crossesLiveGuard && !namesGuard(scan.function, site))
        return false;
    return std::ranges::any_of(liveIns, [&](ValueID liveIn) {
        return mayBeLocalAllocation(scan, liveIn);
    });
}

static bool canStartRegion(TerminatorKind kind)
{
    switch (kind) {
    case TerminatorKind::Branch:
    case TerminatorKind::TestAndBranch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::SwitchOnShape:
        return true;
    case TerminatorKind::Jump:
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::Unreachable:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canEndInteriorBlock(TerminatorKind kind)
{
    switch (kind) {
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::TestAndBranch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::Throw:
    case TerminatorKind::Unreachable:
        return true;
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Rethrow:
    case TerminatorKind::TryCallInline:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canBeInterior(const FunctionScan& scan, const BitVector& coldBlocks, const OutliningSite& region, BlockID blockID)
{
    if (!coldBlocks.quickGet(blockID.index()) || scan.blocksInSites.quickGet(blockID.index()))
        return false;
    for (Edge edge : scan.cfg.incomingEdges(blockID)) {
        if (edge.from != region.entryBlock && !region.interiorBlocks.contains(edge.from))
            return false;
    }
    const Block& block = scan.function.at(blockID);
    if (block.isHandlerBlock() || block.handler != scan.function.at(region.entryBlock).handler)
        return false;
    return canEndInteriorBlock(block.terminator.kind);
}

static bool addPendingTargets(FunctionScan& scan, const OutliningSite& region, const Terminator& terminator, Vector<uint32_t, 8>& pending)
{
    for (const Target& target : terminator.targets) {
        uint32_t order = scan.cfg.reversePostOrderIndex(target.block);
        std::span<const uint32_t> ordered = pending.span();
        size_t position = std::ranges::lower_bound(ordered, order) - ordered.begin();
        if (position < pending.size() && pending[position] == order)
            continue;
        pending.insert(position, order);
        if (scan.build.exceeds(LimitID::MaxOutlinedRegionValues, region.interiorBlocks.size() + pending.size() - 1, scan.subject))
            return false;
    }
    return true;
}

// In a stack trace the call of the body stands for every operation of the site. It lies in one inline frame and has one position.
static Origin originOfBodyCall(const Function& function, const OutliningSite& site)
{
    return site.root ? function.at(site.root).origin() : function.at(site.entryBlock).terminator.origin();
}

static bool isShownInStackTraces(const FunctionScan& scan, ValueID id)
{
    const Effects& effects = scan.analyses.effectsOf(id).normal;
    return effects.mayThrow || effects.mayCallUnknownCode;
}

static bool isShownAt(const FunctionScan& scan, ValueID id, Origin origin)
{
    const Value& value = scan.function.at(id);
    return value.inlineFrame == origin.inlineFrame && (value.position == origin.position || !isShownInStackTraces(scan, id));
}

static bool findRegionBlocks(FunctionScan& scan, const BitVector& coldBlocks, OutliningSite& region, uint32_t& numberOfValues)
{
    Origin origin = originOfBodyCall(scan.function, region);
    Vector<uint32_t, 8> pending;
    if (!addPendingTargets(scan, region, scan.function.at(region.entryBlock).terminator, pending))
        return false;

    while (pending.size() > 1) {
        BlockID blockID = scan.cfg.reversePostOrder()[pending.first()];
        pending.removeAt(0);
        if (!canBeInterior(scan, coldBlocks, region, blockID))
            return false;

        const Block& block = scan.function.at(blockID);
        numberOfValues += block.values.size();
        if (scan.build.exceeds(LimitID::MaxOutlinedRegionValues, numberOfValues, scan.subject))
            return false;
        if (block.terminator.inlineFrame != origin.inlineFrame)
            return false;
        for (ValueID id : block.values) {
            if (!isRematerializable(scan.function.at(id)) && (!isMovable(scan, id) || !isShownAt(scan, id, origin)))
                return false;
        }

        region.interiorBlocks.append(blockID);
        if (!addPendingTargets(scan, region, block.terminator, pending))
            return false;
    }

    if (pending.isEmpty() || region.interiorBlocks.isEmpty())
        return false;
    region.exitBlock = scan.cfg.reversePostOrder()[pending.first()];
    return true;
}

static uint32_t extendBackward(FunctionScan& scan, OutliningSite& site, uint32_t lowestIndex, BackwardExtension extension, uint32_t numberOfValues)
{
    const Block& block = scan.function.at(site.entryBlock);
    Origin origin = originOfBodyCall(scan.function, site);
    uint32_t numberOfOperations = 0;
    while (site.firstIndex > lowestIndex) {
        ValueID id = block.values[site.firstIndex - 1];
        const Value& value = scan.function.at(id);
        bool isConstant = isRematerializable(value);
        if (scan.valuesInSites.quickGet(id.index()))
            break;
        if (!isConstant && (extension == BackwardExtension::ConstantsOnly || !isMovable(scan, id) || !isShownAt(scan, id, origin)))
            break;
        if (extension == BackwardExtension::UsedValues && !scan.useLists.useCount(id))
            break;
        if (!hasOnlyUsesInSite(scan, site, id))
            break;
        if (scan.build.exceeds(LimitID::MaxOutlinedRegionValues, ++numberOfValues, scan.subject))
            break;
        --site.firstIndex;
        ++site.numberOfEntryValues;
        numberOfOperations += !isConstant;
    }
    return numberOfOperations;
}

static bool isComputedByRegion(const FunctionScan& scan, const OutliningSite& region, std::span<const Arguments* const> exitEdges, uint32_t position)
{
    ValueID first = (*exitEdges.front())[position];
    bool allEdgesPassFirst = std::ranges::all_of(exitEdges, [&](const Arguments* arguments) {
        return (*arguments)[position] == first;
    });
    if (allEdgesPassFirst && !isDefinedInSite(scan, region, first))
        return false;

    const Value& firstValue = scan.function.at(first);
    if (!isRematerializable(firstValue))
        return true;
    return !std::ranges::all_of(exitEdges, [&](const Arguments* arguments) {
        const Value& value = scan.function.at((*arguments)[position]);
        return value.opcode == firstValue.opcode && value.hasSamePayloadAs(firstValue);
    });
}

static bool findExitArgument(const FunctionScan& scan, OutliningSite& region)
{
    const Function& function = scan.function;
    Vector<const Arguments*, 4> exitEdges;
    auto collectExitEdges = [&](const Terminator& terminator) {
        for (const Target& target : terminator.targets) {
            if (target.block != region.exitBlock)
                continue;
            if (!target.proofs.isEmpty())
                return false;
            exitEdges.append(&target.arguments);
        }
        return true;
    };

    if (!collectExitEdges(function.at(region.entryBlock).terminator))
        return false;
    for (BlockID blockID : region.interiorBlocks) {
        const Block& block = function.at(blockID);
        for (ValueID parameter : block.parameters) {
            if (!hasOnlyUsesInSite(scan, region, parameter))
                return false;
        }
        for (ValueID id : block.values) {
            if (!hasOnlyUsesInSite(scan, region, id))
                return false;
        }
        if (!collectExitEdges(block.terminator))
            return false;
    }

    uint32_t numberOfPositions = function.at(region.exitBlock).parameters.size();
    for (uint32_t position = 0; position < numberOfPositions; ++position) {
        if (!isComputedByRegion(scan, region, exitEdges.span(), position))
            continue;
        if (region.exitArgument)
            return false;
        region.exitArgument = position;
    }
    return true;
}

static uint32_t computeCodeBytes(const FunctionScan& scan, const OutliningSite& site)
{
    const Function& function = scan.function;
    uint32_t bytes = 0;
    auto addValues = [&](std::span<const ValueID> values) {
        for (ValueID id : values) {
            const Value& value = function.at(id);
            if (!isRematerializable(value))
                bytes += estimatedCodeBytes(scan.program, function, value);
        }
    };

    addValues(entryValuesOf(function, site));
    if (!site.isRegion())
        return bytes;
    bytes += estimatedCodeBytes(function.at(site.entryBlock).terminator);
    for (BlockID blockID : site.interiorBlocks) {
        const Block& block = function.at(blockID);
        addValues(block.values.span());
        bytes += estimatedCodeBytes(block.terminator);
    }
    return bytes;
}

static bool completeSite(FunctionScan& scan, OutliningSite& site)
{
    const Function& function = scan.function;
    site.resultType = TypeTable::undefined;
    ValueID result;
    if (site.rootIsUsed)
        result = site.root;
    else if (site.exitArgument)
        result = function.at(site.exitBlock).parameters[*site.exitArgument];
    if (result)
        site.resultType = function.at(result).type;
    if (!canCrossCallBoundary(scan.program, site.resultType) || (result && mayBeUnspecified(scan.analyses, result)))
        return false;
    if (namesGuardDefinedOutside(scan, site))
        return false;

    OutliningNormalForm form(function, site, scan.build.limit(LimitID::MaxOutlinedOperands));
    if (form.hasRequiredDefinitionOutside())
        return false;
    for (ValueID liveIn : form.liveIns()) {
        if (!canBeLiveIn(scan, liveIn))
            return false;
    }
    if (mayEndGuardInUse(scan, site, form.liveIns().span()))
        return false;
    if (scan.build.exceeds(LimitID::MaxOutlinedOperands, form.liveIns().size(), scan.subject))
        return false;

    site.liveIns = form.liveIns();
    site.codeBytes = computeCodeBytes(scan, site);
    site.hash = form.hash();
    return true;
}

static void markSite(FunctionScan& scan, const OutliningSite& site)
{
    for (ValueID id : entryValuesOf(scan.function, site))
        scan.valuesInSites.quickSet(id.index());
    if (!site.isRegion())
        return;
    scan.blocksInSites.quickSet(site.entryBlock.index());
    for (BlockID blockID : site.interiorBlocks) {
        scan.blocksInSites.quickSet(blockID.index());
        for (ValueID id : scan.function.at(blockID).values)
            scan.valuesInSites.quickSet(id.index());
    }
}

static std::optional<OutliningSite> tryCreateRegion(FunctionScan& scan, const BitVector& coldBlocks, BlockID blockID)
{
    const Block& block = scan.function.at(blockID);
    if (scan.blocksInSites.quickGet(blockID.index()) || !canStartRegion(block.terminator.kind))
        return std::nullopt;

    OutliningSite region;
    region.function = scan.id;
    region.entryBlock = blockID;
    region.firstIndex = block.values.size();
    uint32_t numberOfValues = 0;
    if (!findRegionBlocks(scan, coldBlocks, region, numberOfValues))
        return std::nullopt;
    extendBackward(scan, region, 0, BackwardExtension::UsedValues, numberOfValues);
    if (!findExitArgument(scan, region) || !completeSite(scan, region))
        return std::nullopt;
    return region;
}

static bool canUseCallResult(const Function& function, const Use& use)
{
    auto acceptsAnyDefinition = [](OperandKind kind) {
        return !mustBeDefinition(kind) && kind != OperandKind::Guard;
    };
    switch (use.kind) {
    case Use::Kind::Operand:
        return acceptsAnyDefinition(function.at(use.user).operandKind(use.operandIndex));
    case Use::Kind::TerminatorOperand: {
        const Terminator& terminator = function.at(use.block).terminator;
        return terminator.kind != TerminatorKind::TestAndBranch && acceptsAnyDefinition(terminator.operandKind(use.operandIndex));
    }
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        return true;
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canBeRoot(const FunctionScan& scan, ValueID id)
{
    const Function& function = scan.function;
    const Value& value = function.at(id);
    if (!isMovable(scan, id) || branchesOf(value.opcode) == Branches::Only || familyOf(value.opcode) == OperationFamily::GuardValue)
        return false;
    if (isConditionOfBranchWithProofs(function, scan.useLists, id))
        return false;
    // A deferred closure has no code and no value: it moves only together with the store that names it.
    if (isDeferredClosure(function, id))
        return false;

    std::span<const Use> uses = scan.useLists.usesOf(id);
    if (!uses.empty() && numberOfResultsOf(scan.program, function, value))
        return false;
    return std::ranges::all_of(uses, [&](const Use& use) {
        return canUseCallResult(function, use);
    });
}

static OutliningSite createSingleValueRun(const FunctionScan& scan, BlockID block, uint32_t rootIndex)
{
    OutliningSite run;
    run.function = scan.id;
    run.entryBlock = block;
    run.firstIndex = rootIndex;
    run.numberOfEntryValues = 1;
    run.root = scan.function.at(block).values[rootIndex];
    run.rootIsUsed = !!scan.useLists.useCount(run.root);
    return run;
}

static std::optional<OutliningSite> tryCreateRun(FunctionScan& scan, const ValueRange& range, uint32_t rootIndex, BackwardExtension extension)
{
    OutliningSite run = createSingleValueRun(scan, range.block, rootIndex);
    uint32_t numberOfOperationsAdded = extendBackward(scan, run, range.begin, extension, 1);
    if (completeSite(scan, run))
        return run;
    if (!numberOfOperationsAdded)
        return std::nullopt;

    run = createSingleValueRun(scan, range.block, rootIndex);
    extendBackward(scan, run, range.begin, BackwardExtension::ConstantsOnly, 1);
    if (completeSite(scan, run))
        return run;
    return std::nullopt;
}

static void findRuns(FunctionScan& scan, const ValueRange& range, BackwardExtension extension, Vector<OutliningSite>& sites)
{
    const Block& block = scan.function.at(range.block);
    for (uint32_t index = range.end; index-- > range.begin;) {
        ValueID root = block.values[index];
        if (scan.valuesInSites.quickGet(root.index()) || !canBeRoot(scan, root))
            continue;
        std::optional<OutliningSite> run = tryCreateRun(scan, range, index, extension);
        if (!run)
            continue;
        markSite(scan, *run);
        index = run->firstIndex;
        sites.append(WTF::move(*run));
    }
}

static void findSitesInFunction(const FinderInputs& finder, FunctionID id, Vector<OutliningSite>& sites)
{
    const Function& function = finder.program.at(id);
    if (!function.hasBody() || !finder.build.isEnabled(PassID::OutlineColdOperations, id))
        return;

    FunctionAnalyses analyses(finder.contextOf(id), function);
    BitVector coldBlocks = findColdBlocks(finder, analyses);
    if (coldBlocks.isEmpty())
        return;

    FunctionScan scan(finder, id, analyses);
    for (BlockID block : scan.cfg.reversePostOrder()) {
        if (!coldBlocks.quickGet(block.index()))
            continue;
        size_t firstSiteOfBlock = sites.size();
        if (std::optional<OutliningSite> region = tryCreateRegion(scan, coldBlocks, block)) {
            markSite(scan, *region);
            sites.append(WTF::move(*region));
        }
        findRuns(scan, { block, 0, static_cast<uint32_t>(function.at(block).values.size()) }, BackwardExtension::AllValues, sites);
        std::ranges::reverse(sites.mutableSubspan(firstSiteOfBlock));
    }
}

static bool canBeSplit(const Program& program, const OutliningSite& site)
{
    if (site.isRegion())
        return true;
    if (site.numberOfEntryValues < 2)
        return false;
    const Function& function = program.at(site.function);
    return std::ranges::count_if(entryValuesOf(function, site), [&](ValueID id) {
        return !isRematerializable(function.at(id));
    }) > 1;
}

static BitVector findUnrepeatedSites(const FinderInputs& finder, const Vector<OutliningSite>& sites)
{
    const ParallelLoops& loops = finder.build.parallelLoops();
    Vector<uint32_t> candidates = findIndicesInParallel(loops, sites.size(), [&](unsigned index) {
        return canBeSplit(finder.program, sites[index]);
    });
    parallelSort(loops, candidates.mutableSpan(), [&](uint32_t a, uint32_t b) {
        return sites[a].hash < sites[b].hash;
    });

    BitVector unrepeatedSites(sites.size());
    for (size_t index = 0; index < candidates.size(); ++index) {
        unsigned hash = sites[candidates[index]].hash;
        bool equalsPrevious = index && sites[candidates[index - 1]].hash == hash;
        bool equalsNext = index + 1 < candidates.size() && sites[candidates[index + 1]].hash == hash;
        if (!equalsPrevious && !equalsNext)
            unrepeatedSites.quickSet(candidates[index]);
    }
    return unrepeatedSites;
}

static void splitSitesInFunction(const FinderInputs& finder, std::span<OutliningSite> sites, SiteRange range, const BitVector& unrepeatedSites, Vector<OutliningSite>& result)
{
    if (!range.hasUnrepeatedSite) {
        for (OutliningSite& site : sites.subspan(range.begin, range.end - range.begin))
            result.append(WTF::move(site));
        return;
    }

    FunctionID id = sites[range.begin].function;
    FunctionAnalyses analyses(finder.contextOf(id), finder.program.at(id));
    FunctionScan scan(finder, id, analyses);
    size_t firstSiteOfFunction = result.size();
    for (uint32_t index = range.begin; index < range.end; ++index) {
        OutliningSite& site = sites[index];
        if (!unrepeatedSites.quickGet(index)) {
            result.append(WTF::move(site));
            continue;
        }
        findRuns(scan, { site.entryBlock, site.firstIndex, site.firstIndex + site.numberOfEntryValues }, BackwardExtension::ConstantsOnly, result);
        for (BlockID block : site.interiorBlocks)
            findRuns(scan, { block, 0, static_cast<uint32_t>(scan.function.at(block).values.size()) }, BackwardExtension::ConstantsOnly, result);
    }
    std::ranges::sort(result.mutableSubspan(firstSiteOfFunction), { }, [&](const OutliningSite& site) {
        return std::pair { scan.cfg.reversePostOrderIndex(site.entryBlock), site.firstIndex };
    });
}

static Vector<OutliningSite> splitUnrepeatedSites(const FinderInputs& finder, Vector<OutliningSite>&& sites)
{
    BitVector unrepeatedSites = findUnrepeatedSites(finder, sites);
    if (unrepeatedSites.isEmpty())
        return WTF::move(sites);

    Vector<SiteRange> functions;
    for (uint32_t index = 0; index < sites.size(); ++index) {
        if (functions.isEmpty() || sites[functions.last().begin].function != sites[index].function)
            functions.append(SiteRange { index, index, false });
        functions.last().end = index + 1;
        functions.last().hasUnrepeatedSite |= unrepeatedSites.quickGet(index);
    }

    return collectInParallel<OutliningSite>(finder.build.parallelLoops(), functions.size(), [&](unsigned index, Vector<OutliningSite>& result) {
        splitSitesInFunction(finder, sites.mutableSpan(), functions[index], unrepeatedSites, result);
    });
}

Vector<OutliningSite> findOutliningSites(const Program& program, const OptimizationInputs& inputs, BuildContext& build)
{
    FinderInputs finder { program, inputs, build };
    Vector<OutliningSite> sites = collectInParallel<OutliningSite>(build.parallelLoops(), program.functions.size(), largeFunctions(program, build.parallelLoops()).span(), [&](unsigned index, Vector<OutliningSite>& found) {
        findSitesInFunction(finder, FunctionID(index), found);
    });
    return splitUnrepeatedSites(finder, WTF::move(sites));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
