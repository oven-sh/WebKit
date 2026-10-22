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
#include "AOTCodeShapes.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTDecidedBranches.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardFactDetails.h"
#include "AOTHeat.h"
#include "AOTOutliningNormalForm.h"
#include "AOTPass.h"
#include "AOTProgramCaches.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
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
enum class SitesThatCannotPayOff : uint8_t { AreKeptToBeSplit, AreDropped };
enum class NamedGuards : uint8_t { None, AllDefinedInSite, SomeDefinedOutside };

struct FinderInputs {
    AnalysisContext contextOf(FunctionID id) const { return { build, program, &inputs.flow, &inputs.effects, id }; }

    const Program& program;
    const OptimizationInputs inputs;
    BuildContext& build;
    OpcodeSet opcodesThatMayDefineObjects;
    OpcodeSet opcodesThatDefineObjectsThatNeedNotBeFresh;
    OpcodeSet opcodesThatProve;
};

class GuardsInUse {
    WTF_MAKE_NONCOPYABLE(GuardsInUse);
public:
    explicit GuardsInUse(FunctionAnalyses&);

    bool isAnyLiveAt(ProgramPoint);
    bool isAnyLiveOnEdge(const Target&);

private:
    bool hasGuards();
    bool contains(ValueID id) { return id && hasGuards() && m_guards->quickGet(id.index()); }
    bool isAnyLiveAtHead(BlockID);
    const Liveness& liveness();
    uint32_t findEndOfUses(BlockID);

    const Function& m_function;
    const CFG& m_cfg;
    const UseLists& m_useLists;
    // Found when somebody asks.
    std::optional<BitVector> m_guards;
    bool m_hasGuards { false };
    std::optional<Liveness> m_liveness;
    Vector<std::optional<uint32_t>> m_endOfUses;
};

// What another rule wants in the function where it stands.
// JSONObjectFinish takes an object that its own function allocated, so neither of the two leaves without the other.
// A guard on an allocation lasts across unknown code while nothing else holds the object, which is seen only where the allocation is.
// The two copies of `{ ...first, ...second }` get their code together.
static BitVector findValuesBoundToFunction(FunctionAnalyses& analyses)
{
    const Function& function = analyses.function();
    BitVector bound(function.values.size());
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (value.opcode == Opcode::CopyObject) {
                if (ValueID second = findCopyIntoCopy(function, function.at(block), id)) {
                    bound.quickSet(id.index());
                    bound.quickSet(second.index());
                }
                continue;
            }
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

// The objects that a test has made a guard about, each under the value that all its casts lead back to.
static BitVector findSubjectsOfGuards(FunctionAnalyses& analyses)
{
    const Function& function = analyses.function();
    BitVector subjects(function.values.size());
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (provesOf(value.opcode) != Proof::None && value.numberOfOperands() && value.operand(subjectOperandIndex))
                subjects.quickSet(analyses.identityOf(value.operand(subjectOperandIndex)).index());
        }
    }
    return subjects;
}

struct FunctionScan {
    FunctionScan(const FinderInputs& finder, FunctionID id, FunctionAnalyses& analyses)
        : finder(finder)
        , program(finder.program)
        , build(finder.build)
        , id(id)
        , analyses(analyses)
        , function(analyses.function())
        , cfg(analyses.cfg())
        , useLists(analyses.useLists())
        , subject(analyses.subject())
        , guards(analyses)
        , unspecifiedValues(analyses)
        , valuesInSites(function.values.size())
        , blocksInSites(function.blocks.size())
        , cachedIsMovable(function.values.size())
        , cachedCodeBytes(function.values.size())
        , maxRegionValues(finder.build.limit(LimitID::MaxOutlinedRegionValues))
        , mayHaveFrameStorage(analyses.opcodesOfValues().intersects(Value::opcodesWithStorage))
        , mayBindAnyValue(analyses.opcodesOfValues().intersects(OpcodeSet { Opcode::JSONObjectFinish, Opcode::CopyObject }))
        , hasOperationThatProves(analyses.opcodesOfValues().intersects(finder.opcodesThatProve))
    {
    }

    bool isFramePointer(ValueID id)
    {
        if (!mayHaveFrameStorage)
            return false;
        if (!framePointers)
            framePointers.emplace(function);
        return framePointers->contains(id);
    }

    bool exceedsRegionValues(uint64_t numberOfValues) { return numberOfValues > maxRegionValues && build.exceeds(LimitID::MaxOutlinedRegionValues, numberOfValues, subject); }

    // Whether the escape analysis takes the result for an object of its own. Nothing else is bound, but for what belongs to a JSONObjectFinish or to a CopyObject.
    bool mayDefineObject(ValueID id)
    {
        const Value& value = function.at(id);
        if (!finder.opcodesThatMayDefineObjects.contains(value.opcode))
            return false;
        if (finder.opcodesThatDefineObjectsThatNeedNotBeFresh.contains(value.opcode))
            return true;
        auto keptEffects = [&] {
            return analyses.effectsOf(id);
        };
        return freshnessOf(analyses.effectContext(), function, value, ScopedLambda<EffectsOnEdges()>(keptEffects)) != ResultFreshness::Unknown;
    }

    bool isBoundToFunction(ValueID value)
    {
        if (!mayBindAnyValue && !mayDefineObject(value))
            return false;
        if (!valuesBoundToFunction)
            findBoundValues();
        return valuesBoundToFunction->quickGet(value.index());
    }

    bool isSubjectOfGuard(ValueID identity)
    {
        if (!subjectsOfGuards)
            subjectsOfGuards = findSubjectsOfGuards(analyses);
        return subjectsOfGuards->quickGet(identity.index());
    }

    void findBoundValues()
    {
        valuesBoundToFunction = findValuesBoundToFunction(analyses);
        bindAllocationsWrittenUnderGuards();
        if (mayBindAnyValue)
            return;
        for (size_t index : *valuesBoundToFunction)
            RELEASE_ASSERT(function.values[index].opcode == Opcode::Parameter || mayDefineObject(ValueID(index)));
    }

    // A guard about one object lasts over a write to another that is seen to be allocated in this function. What a call returns may be any object, the guarded one among them.
    void bindAllocationsWrittenUnderGuards()
    {
        for (BlockID block : cfg.reversePostOrder()) {
            const auto& values = function.at(block).values;
            for (uint32_t index = 0; index < values.size(); ++index) {
                if (!guards.isAnyLiveAt({ block, index + 1 }))
                    break;
                const EffectsOnEdges& effects = analyses.effectsOf(values[index]);
                if (effects.normal.writes.isEmpty() && effects.handler.writes.isEmpty())
                    continue;
                const EscapeAnalysis& escape = analyses.escape();
                function.at(values[index]).forEachOperand([&](ValueID operand, uint32_t) {
                    for (ObjectID object : escape.pointsTo(operand).objects)
                        valuesBoundToFunction->quickSet(escape.at(object).definition.index());
                });
            }
        }
    }

    const FinderInputs& finder;
    const Program& program;
    BuildContext& build;
    FunctionID id;
    FunctionAnalyses& analyses;
    const Function& function;
    const CFG& cfg;
    const UseLists& useLists;
    Subject subject;
    std::optional<FramePointers> framePointers;
    GuardsInUse guards;
    UnspecifiedValues unspecifiedValues;
    std::optional<BitVector> valuesBoundToFunction;
    std::optional<BitVector> subjectsOfGuards;
    BitVector valuesInSites;
    BitVector blocksInSites;
    Vector<std::optional<bool>> cachedIsMovable;
    Vector<std::optional<uint32_t>> cachedCodeBytes;
    uint64_t maxRegionValues;
    bool mayHaveFrameStorage;
    bool mayBindAnyValue;
    bool hasOperationThatProves;
};

struct ValueRange {
    BlockID block;
    uint32_t begin { 0 };
    uint32_t end { 0 };
};

struct SplittableSite {
    uint32_t site { 0 };
    uint32_t firstPart { 0 };
    uint32_t numberOfParts { 0 };
    bool isRepeated { false };
};

// The parts are what takes the place of a site that no other site in the program is equal to.
struct FunctionSites {
    Vector<OutliningSite> sites;
    Vector<OutliningSite> parts;
    Vector<SplittableSite> splittableSites;
};

struct SplittableSiteReference {
    unsigned hash { 0 };
    uint32_t function { 0 };
    uint32_t splittableSite { 0 };
};

} // anonymous namespace

GuardsInUse::GuardsInUse(FunctionAnalyses& analyses)
    : m_function(analyses.function())
    , m_cfg(analyses.cfg())
    , m_useLists(analyses.useLists())
    , m_endOfUses(m_function.blocks.size())
{
}

bool GuardsInUse::hasGuards()
{
    if (!m_guards) {
        m_guards = guardValuesOf(m_function, m_cfg);
        m_hasGuards = !m_guards->isEmpty();
    }
    return m_hasGuards;
}

const Liveness& GuardsInUse::liveness()
{
    if (!m_liveness)
        m_liveness.emplace(m_function, m_cfg, m_useLists, LivenessOptions { .trackedValues = &*m_guards });
    return *m_liveness;
}

bool GuardsInUse::isAnyLiveAtHead(BlockID block)
{
    return hasGuards() && !liveness().liveAtHead(block).empty();
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
    if (!hasGuards())
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

static bool computeIsMovable(FunctionScan& scan, ValueID id)
{
    const Value& value = scan.function.at(id);
    if (boundToOf(value.opcode) != BoundTo::Nothing || value.type == TypeTable::never)
        return false;
    // Only the body of a coroutine may hold it.
    if (value.opcode == Opcode::TakeGeneratorPrototype)
        return false;
    if (isCallOfReturnCall(scan, id) || isDemotedTailCall(value))
        return false;
    if (dependsOnInlineFrame(value) || dependsOnTopErrorFrame(scan.program, scan.function, value) || hasOperandKeyedByAccessSite(scan.program, value))
        return false;
    if (hasFrameStorage(value) || scan.isBoundToFunction(id))
        return false;
    return !maySuspend(value.opcode) || !scan.analyses.maySuspend(id);
}

static bool isMovable(FunctionScan& scan, ValueID id)
{
    std::optional<bool>& cached = scan.cachedIsMovable[id.index()];
    if (!cached)
        cached = computeIsMovable(scan, id);
    return *cached;
}

static bool canCrossCallBoundary(const Program& program, TypeID type)
{
    return program.at(type).isJavaScriptValue();
}

// No use of an Unspecified value is reached, and an operand or the result of the call of the shared function would be one.
static bool canBeLiveIn(FunctionScan& scan, ValueID id)
{
    const Value& value = scan.function.at(id);
    return canCrossCallBoundary(scan.program, value.type) && !scan.isFramePointer(id) && branchesOf(value.opcode) != Branches::Only && !isDeferredClosure(scan.function, id) && !isBorrowedInnerObject(scan.function, id) && !scan.unspecifiedValues.mayContain(id);
}

static bool usesExecutionCounts(HeatOverride heat)
{
    switch (heat) {
    case HeatOverride::EverythingIsHot:
    case HeatOverride::EverythingIsCold:
        return false;
    case HeatOverride::None:
    case HeatOverride::OnlyLoopsAreHot:
    case HeatOverride::NothingIsHottest:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isColdOutsideCycles(const FinderInputs& finder, FunctionAnalyses& analyses)
{
    const AnalysisContext& context = analyses.context();
    if (usesExecutionCounts(finder.build.recordedOptions().heat()) && finder.inputs.flow.runsAtMostOnce(context.program, context.id))
        return true;
    const Heat& heat = finder.inputs.heat;
    return isCold(heat.classOf(heat.activationsOf(context.program, context.id) * roundingOfFrequencySums)) && runsNoBlockMoreOftenThanEntry(analyses.cfg());
}

static BitVector findColdBlocks(const FinderInputs& finder, FunctionAnalyses& analyses)
{
    bool isColdWithoutLooking = isColdOutsideCycles(finder, analyses);
    BitVector coldBlocks(analyses.function().blocks.size());
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        if (!analyses.isInCycle(block) && (isColdWithoutLooking || isCold(heatClassOf(finder.inputs.heat, analyses, block))))
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

static NamedGuards findNamedGuards(const FunctionScan& scan, const OutliningSite& site)
{
    NamedGuards result = NamedGuards::None;
    bool namesOneDefinedOutside = anyValueInSite(scan.function, site, [&](ValueID id) {
        const Value& value = scan.function.at(id);
        for (uint32_t operandIndex = 0; operandIndex < value.numberOfOperands(); ++operandIndex) {
            ValueID operand = value.operand(operandIndex);
            if (!operand || value.operandKind(operandIndex) != OperandKind::Guard)
                continue;
            if (!isDefinedInSite(scan, site, operand))
                return true;
            result = NamedGuards::AllDefinedInSite;
        }
        return false;
    });
    return namesOneDefinedOutside ? NamedGuards::SomeDefinedOutside : result;
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
static bool mayEndGuardInUse(FunctionScan& scan, const OutliningSite& site, std::span<const ValueID> liveIns, NamedGuards namedGuards)
{
    bool crossesLiveGuard = isAnyGuardLiveAfter(scan, site);
    if (crossesLiveGuard && !alwaysRunsUnknownCode(scan, site))
        return true;
    if (!crossesLiveGuard && namedGuards == NamedGuards::None)
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
        if (scan.exceedsRegionValues(region.interiorBlocks.size() + pending.size() - 1))
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
        if (scan.exceedsRegionValues(numberOfValues))
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
        if (scan.exceedsRegionValues(++numberOfValues))
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

static uint32_t computeCodeBytes(FunctionScan& scan, const OutliningSite& site)
{
    const Function& function = scan.function;
    uint32_t bytes = 0;
    auto addValues = [&](std::span<const ValueID> values) {
        for (ValueID id : values) {
            const Value& value = function.at(id);
            if (isRematerializable(value))
                continue;
            std::optional<uint32_t>& cached = scan.cachedCodeBytes[id.index()];
            if (!cached)
                cached = estimatedCodeBytes(scan.program, function, value);
            bytes += *cached;
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

// In cold code an access by key and a call of a value are the moves of their operands and one call of a stub: what a call of a function is. As a function of its own it gets a frame and two more jumps each time it runs.
static bool isOneCallOfStub(const Program& program, const OutliningSite& site)
{
    static constexpr OpcodeSet accessesByKey { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemDefine, Opcode::ElemDelete };
    if (site.isRegion())
        return false;
    const Function& function = program.at(site.function);
    auto passesConstantThatTakesCode = [&](const Value& call) {
        bool result = false;
        call.forEachOperand([&](ValueID operand, uint32_t) {
            result |= isRematerializable(function.at(operand)) && estimatedCodeBytes(program, function, function.at(operand));
        });
        return result;
    };
    unsigned numberOfOperations = 0;
    for (ValueID id : entryValuesOf(function, site)) {
        const Value& value = function.at(id);
        if (isRematerializable(value) || !estimatedCodeBytes(program, function, value))
            continue;
        bool isCallOfValue = accessClassOf(value.opcode) == AccessClass::IndirectCall && !passesConstantThatTakesCode(value);
        if (!(isCallOfValue || accessesByKey.contains(value.opcode)) || ++numberOfOperations > 1)
            return false;
    }
    return numberOfOperations;
}

// However many equal sites there are, each of them gets no shorter.
static bool canPayOff(const Program& program, const OutliningSite& site)
{
    return site.codeBytes > site.callBytes && !isOneCallOfStub(program, site);
}

// What a call returns is a value of its own. Checks that hand on the object they were given would, as a call, cut the object in two:
// a guard that a test has made about what comes out no longer speaks of what went in, and the function may reach the object from there again, by other casts, and name that guard.
static bool handsOnSubjectOfGuard(FunctionScan& scan, const OutliningSite& site, ValueID result)
{
    if (!scan.hasOperationThatProves)
        return false;
    ValueID identity = scan.analyses.identityOf(result);
    return identity != result && scan.isSubjectOfGuard(identity) && !isDefinedInSite(scan, site, identity);
}

// A parameter is a value of its own too. Two names of one object that both go in are two objects inside, and a guard that a test there makes about the one does not speak of the other.
static bool passesOneObjectUnderTwoNames(FunctionScan& scan, std::span<const ValueID> liveIns)
{
    for (size_t index = 0; index < liveIns.size(); ++index) {
        ValueID identity = scan.analyses.identityOf(liveIns[index]);
        bool isPassedBefore = std::ranges::any_of(liveIns.first(index), [&](ValueID earlier) {
            return scan.analyses.identityOf(earlier) == identity;
        });
        if (isPassedBefore)
            return true;
    }
    return false;
}

static bool canBeSplit(const Program&, const OutliningSite&);

static bool completeSite(FunctionScan& scan, OutliningSite& site, SitesThatCannotPayOff unprofitableSites)
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
    if (!canCrossCallBoundary(scan.program, site.resultType) || (result && scan.unspecifiedValues.mayContain(result)))
        return false;
    NamedGuards namedGuards = findNamedGuards(scan, site);
    if (namedGuards == NamedGuards::SomeDefinedOutside)
        return false;
    if (result && handsOnSubjectOfGuard(scan, site, result))
        return false;

    OutliningNormalForm form(function, site, scan.build.limit(LimitID::MaxOutlinedOperands));
    if (form.hasRequiredDefinitionOutside())
        return false;
    for (ValueID liveIn : form.liveIns()) {
        if (!canBeLiveIn(scan, liveIn))
            return false;
    }
    if (mayEndGuardInUse(scan, site, form.liveIns().span(), namedGuards))
        return false;
    if (namedGuards != NamedGuards::None && passesOneObjectUnderTwoNames(scan, form.liveIns().span()))
        return false;
    if (scan.build.exceeds(LimitID::MaxOutlinedOperands, form.liveIns().size(), scan.subject))
        return false;

    site.liveIns = form.liveIns();
    site.codeBytes = computeCodeBytes(scan, site);
    site.callBytes = estimatedCallBytes(site);
    site.canPayOff = canPayOff(scan.program, site);
    if (site.canPayOff || (unprofitableSites == SitesThatCannotPayOff::AreKeptToBeSplit && canBeSplit(scan.program, site)))
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
    region.orderOfEntryBlock = scan.cfg.reversePostOrderIndex(blockID);
    region.firstIndex = block.values.size();
    uint32_t numberOfValues = 0;
    if (!findRegionBlocks(scan, coldBlocks, region, numberOfValues))
        return std::nullopt;
    extendBackward(scan, region, 0, BackwardExtension::UsedValues, numberOfValues);
    if (!findExitArgument(scan, region) || !completeSite(scan, region, SitesThatCannotPayOff::AreKeptToBeSplit))
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

static bool canBeRoot(FunctionScan& scan, ValueID id)
{
    const Function& function = scan.function;
    const Value& value = function.at(id);
    if (!isMovable(scan, id) || branchesOf(value.opcode) == Branches::Only || familyOf(value.opcode) == OperationFamily::GuardValue)
        return false;
    if (isConditionOfBranchWithProofs(function, scan.useLists, id))
        return false;
    // A deferred closure has no code and no value: it moves only together with the store that names it.
    if (isDeferredClosure(function, id) || isBorrowedInnerObject(function, id))
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
    run.orderOfEntryBlock = scan.cfg.reversePostOrderIndex(block);
    run.firstIndex = rootIndex;
    run.numberOfEntryValues = 1;
    run.root = scan.function.at(block).values[rootIndex];
    run.rootIsUsed = !!scan.useLists.useCount(run.root);
    return run;
}

static bool isOneOperationThatCannotPayOff(FunctionScan& scan, const OutliningSite& run)
{
    const Function& function = scan.function;
    Vector<ValueID, 6> liveIns;
    if (!isRematerializable(function.at(run.root))) {
        function.at(run.root).forEachOperand([&](ValueID operand, uint32_t) {
            if (!isRematerializable(function.at(operand)) && !liveIns.contains(operand))
                liveIns.append(operand);
        });
    }
    if (liveIns.size() > scan.build.limit(LimitID::MaxOutlinedOperands))
        return false;
    return computeCodeBytes(scan, run) <= estimatedDirectCallBytes(liveIns.size()) || isOneCallOfStub(scan.program, run);
}

static std::optional<OutliningSite> tryCreateRun(FunctionScan& scan, const ValueRange& range, uint32_t rootIndex, BackwardExtension extension, SitesThatCannotPayOff unprofitableSites)
{
    OutliningSite run = createSingleValueRun(scan, range.block, rootIndex);
    uint32_t numberOfOperationsAdded = extendBackward(scan, run, range.begin, extension, 1);
    if (!numberOfOperationsAdded && isOneOperationThatCannotPayOff(scan, run))
        return run;
    if (completeSite(scan, run, unprofitableSites))
        return run;
    if (!numberOfOperationsAdded)
        return std::nullopt;

    run = createSingleValueRun(scan, range.block, rootIndex);
    extendBackward(scan, run, range.begin, BackwardExtension::ConstantsOnly, 1);
    if (completeSite(scan, run, unprofitableSites))
        return run;
    return std::nullopt;
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

static void findRuns(FunctionScan& scan, const ValueRange& range, BackwardExtension extension, SitesThatCannotPayOff unprofitableSites, Vector<OutliningSite>& sites)
{
    const Block& block = scan.function.at(range.block);
    for (uint32_t index = range.end; index-- > range.begin;) {
        ValueID root = block.values[index];
        if (scan.valuesInSites.quickGet(root.index()) || !canBeRoot(scan, root))
            continue;
        std::optional<OutliningSite> run = tryCreateRun(scan, range, index, extension, unprofitableSites);
        if (!run)
            continue;
        markSite(scan, *run);
        index = run->firstIndex;
        if (run->canPayOff || (unprofitableSites == SitesThatCannotPayOff::AreKeptToBeSplit && canBeSplit(scan.program, *run)))
            sites.append(WTF::move(*run));
    }
}

static void findParts(FunctionScan& scan, const OutliningSite& site, Vector<OutliningSite>& parts)
{
    findRuns(scan, { site.entryBlock, site.firstIndex, site.firstIndex + site.numberOfEntryValues }, BackwardExtension::ConstantsOnly, SitesThatCannotPayOff::AreDropped, parts);
    for (BlockID block : site.interiorBlocks)
        findRuns(scan, { block, 0, static_cast<uint32_t>(scan.function.at(block).values.size()) }, BackwardExtension::ConstantsOnly, SitesThatCannotPayOff::AreDropped, parts);
}

// Whether a site is repeated is known when all functions have been looked at. Its parts are found at once, while the analyses of the function are there.
// A part lies within its site and sites do not overlap, so the parts of one site are found without regard to any other site.
static void findPartsOfSplittableSites(FunctionScan& scan, FunctionSites& found)
{
    scan.valuesInSites.clearAll();
    for (uint32_t index = 0; index < found.sites.size(); ++index) {
        const OutliningSite& site = found.sites[index];
        if (!canBeSplit(scan.program, site))
            continue;
        uint32_t firstPart = found.parts.size();
        findParts(scan, site, found.parts);
        found.splittableSites.append(SplittableSite { index, firstPart, static_cast<uint32_t>(found.parts.size() - firstPart), false });
    }
}

static FunctionSites findSitesInFunction(const FinderInputs& finder, FunctionID id)
{
    FunctionSites found;
    const Function& function = finder.program.at(id);
    if (!function.hasBody() || !finder.build.isEnabled(PassID::OutlineColdOperations, id))
        return found;

    FunctionAnalyses analyses(finder.contextOf(id), function);
    BitVector coldBlocks = findColdBlocks(finder, analyses);
    if (coldBlocks.isEmpty())
        return found;

    FunctionScan scan(finder, id, analyses);
    Vector<OutliningSite>& sites = found.sites;
    for (BlockID block : scan.cfg.reversePostOrder()) {
        if (!coldBlocks.quickGet(block.index()))
            continue;
        size_t firstSiteOfBlock = sites.size();
        if (std::optional<OutliningSite> region = tryCreateRegion(scan, coldBlocks, block)) {
            markSite(scan, *region);
            sites.append(WTF::move(*region));
        }
        findRuns(scan, { block, 0, static_cast<uint32_t>(function.at(block).values.size()) }, BackwardExtension::AllValues, SitesThatCannotPayOff::AreKeptToBeSplit, sites);
        std::ranges::reverse(sites.mutableSubspan(firstSiteOfBlock));
    }
    if (!sites.isEmpty())
        findPartsOfSplittableSites(scan, found);
    return found;
}

static void findRepeatedSites(const FinderInputs& finder, ParallelArray<FunctionSites>& functions)
{
    const ParallelLoops& loops = finder.build.parallelLoops();
    ParallelArray<SplittableSiteReference> candidates = collectIntoArrayInParallel<SplittableSiteReference>(loops, functions.size(), { }, [&](unsigned index, Vector<SplittableSiteReference>& found) {
        const FunctionSites& function = functions[index];
        for (uint32_t position = 0; position < function.splittableSites.size(); ++position)
            found.append(SplittableSiteReference { function.sites[function.splittableSites[position].site].hash, index, position });
    });
    parallelSort(loops, candidates.mutableSpan(), [](const SplittableSiteReference& a, const SplittableSiteReference& b) {
        return a.hash < b.hash;
    });
    forEachIndexInParallel(loops, candidates.size(), [&](unsigned index) {
        const SplittableSiteReference& candidate = candidates[index];
        bool equalsPrevious = index && candidates[index - 1].hash == candidate.hash;
        bool equalsNext = index + 1 < candidates.size() && candidates[index + 1].hash == candidate.hash;
        functions[candidate.function].splittableSites[candidate.splittableSite].isRepeated = equalsPrevious || equalsNext;
    });
}

static void appendChosenSites(FunctionSites& function, Vector<OutliningSite*>& result)
{
    size_t firstSiteOfFunction = result.size();
    bool didSplit = false;
    std::span<const SplittableSite> splittableSites = function.splittableSites.span();
    for (uint32_t index = 0; index < function.sites.size(); ++index) {
        bool isSplit = false;
        if (!splittableSites.empty() && splittableSites.front().site == index) {
            const SplittableSite& splittable = splittableSites.front();
            isSplit = !splittable.isRepeated;
            if (isSplit) {
                for (OutliningSite& part : function.parts.mutableSubspan(splittable.firstPart, splittable.numberOfParts))
                    result.append(&part);
            }
            splittableSites = splittableSites.subspan(1);
        }
        didSplit |= isSplit;
        if (!isSplit && function.sites[index].canPayOff)
            result.append(&function.sites[index]);
    }
    if (!didSplit)
        return;
    std::ranges::sort(result.mutableSubspan(firstSiteOfFunction), { }, [](const OutliningSite* site) {
        return std::pair { site->orderOfEntryBlock, site->firstIndex };
    });
}

// Every operation whose result the escape analysis may take for an object of its own.
static OpcodeSet findOpcodesThatMayDefineObjects()
{
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        bool mayDefineObject = resultRuleOf(opcode) == ResultRule::Allocation || resultRuleOf(opcode) == ResultRule::Declared || OperandPosition::parent(opcode);
        switch (effectRuleOf(opcode)) {
        case EffectRule::Builtin:
        case EffectRule::Inline:
            mayDefineObject = true;
            break;
        case EffectRule::Declared:
        case EffectRule::Operator:
        case EffectRule::Access:
        case EffectRule::Call:
            break;
        }
        uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
        for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex)
            mayDefineObject |= operandUsesOf(opcode, operandIndex).contains(OperandUse::Retains) && retainedInOf(opcode, operandIndex) == RetainedIn::Result;
        if (mayDefineObject)
            opcodes.add(opcode);
    }
    return opcodes;
}

static OpcodeSet findOpcodesThatDefineObjectsThatNeedNotBeFresh()
{
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        bool returnsOperand = false;
        uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
        for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex)
            returnsOperand |= operandUsesOf(opcode, operandIndex).contains(OperandUse::Returns);
        if ((returnsOperand && resultRuleOf(opcode) == ResultRule::Allocation) || OperandPosition::parent(opcode))
            opcodes.add(opcode);
    }
    return opcodes;
}

static OpcodeSet findOpcodesThatProve()
{
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (provesOf(opcode) != Proof::None)
            opcodes.add(opcode);
    }
    return opcodes;
}

struct OutliningSiteFinder::State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);

    State(const Program& program, const OptimizationInputs& inputs, BuildContext& build)
        : finder { program, inputs, build, findOpcodesThatMayDefineObjects(), findOpcodesThatDefineObjectsThatNeedNotBeFresh(), findOpcodesThatProve() }
        , functions(build.parallelLoops(), program.functions.size(), [](unsigned) { return FunctionSites(); })
        , hasSites(FillWith { }, program.functions.size(), static_cast<uint8_t>(0))
    {
    }

    FinderInputs finder;
    ParallelArray<FunctionSites> functions;
    Vector<uint8_t> hasSites;
};

OutliningSiteFinder::OutliningSiteFinder(const Program& program, const OptimizationInputs& inputs, BuildContext& build)
    : m_state(makeUnique<State>(program, inputs, build))
{
}

OutliningSiteFinder::~OutliningSiteFinder() = default;

bool OutliningSiteFinder::findIn(FunctionID id)
{
    FunctionSites& found = m_state->functions[id.index()];
    found = findSitesInFunction(m_state->finder, id);
    m_state->hasSites[id.index()] = !found.sites.isEmpty();
    return !found.sites.isEmpty();
}

std::span<const uint8_t> OutliningSiteFinder::hasSites() const
{
    return m_state->hasSites.span();
}

ParallelArray<OutliningSite> OutliningSiteFinder::takeSites()
{
    const FinderInputs& finder = m_state->finder;
    ParallelArray<FunctionSites>& functions = m_state->functions;
    const ParallelLoops& loops = finder.build.parallelLoops();
    findRepeatedSites(finder, functions);
    ParallelArray<OutliningSite*> chosenSites = collectIntoArrayInParallel<OutliningSite*>(loops, functions.size(), { }, [&](unsigned index, Vector<OutliningSite*>& result) {
        appendChosenSites(functions[index], result);
    });
    ParallelArray<OutliningSite> sites(loops, chosenSites.size(), [&](unsigned index) {
        return WTF::move(*chosenSites[index]);
    });
    finder.build.destroyLater(WTF::move(functions));
    return sites;
}

ParallelArray<OutliningSite> findOutliningSites(const Program& program, const OptimizationInputs& inputs, BuildContext& build)
{
    OutliningSiteFinder finder(program, inputs, build);
    const ParallelLoops& loops = build.parallelLoops();
    std::optional<TypeInterningPhase> scratch;
    if (!program.types.hasOpenPhase())
        scratch.emplace(program.types, 0u, TypeInterningPhase::Scratch);
    {
        ProgramCaches caches(program, build);
        Vector<uint32_t> largestFirst = largeFunctions(program, loops);
        forEachIndexInParallel(loops, program.functions.size(), largestFirst.span(), [&](unsigned index) {
            finder.findIn(FunctionID(index));
        });
    }
    if (scratch)
        scratch->discard();
    return finder.takeSites();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
