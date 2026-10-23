/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAnalysisDump.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/EnumTraits.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ASCIILiteral absentText = "none"_s;
static constexpr ASCIILiteral emptyText = "-"_s;

ASCIILiteral sectionNameOf(FunctionAnalysis analysis)
{
    switch (analysis) {
    case FunctionAnalysis::ControlFlow:
        return "control-flow"_s;
    case FunctionAnalysis::Dominators:
        return "dominators"_s;
    case FunctionAnalysis::NaturalLoops:
        return "natural-loops"_s;
    case FunctionAnalysis::Cycles:
        return "cycles"_s;
    case FunctionAnalysis::UseLists:
        return "use-lists"_s;
    case FunctionAnalysis::Identities:
        return "identities"_s;
    case FunctionAnalysis::Liveness:
        return "liveness"_s;
    case FunctionAnalysis::Effects:
        return "effects"_s;
    case FunctionAnalysis::Escape:
        return "escape"_s;
    case FunctionAnalysis::EscapeHistory:
        return "escape-history"_s;
    case FunctionAnalysis::Frequencies:
        return "frequencies"_s;
    case FunctionAnalysis::Guards:
        return "guards"_s;
    case FunctionAnalysis::FailedTests:
        return "failed-tests"_s;
    case FunctionAnalysis::Induction:
        return "induction"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<FunctionAnalysis> parseSectionName(StringView name)
{
    for (FunctionAnalysis analysis : allAnalyses) {
        if (name == sectionNameOf(analysis))
            return analysis;
    }
    return std::nullopt;
}

template<typename Tag>
static void dumpIDWithPrefix(PrintStream& out, ASCIILiteral prefix, ID<Tag> id)
{
    if (!id) {
        out.print(absentText);
        return;
    }
    out.print(prefix, id.index());
}

void dumpID(PrintStream& out, ValueID id)
{
    dumpIDWithPrefix(out, "v"_s, id);
}

void dumpID(PrintStream& out, BlockID id)
{
    dumpIDWithPrefix(out, "b"_s, id);
}

void dumpID(PrintStream& out, ObjectID id)
{
    dumpIDWithPrefix(out, "o"_s, id);
}

void dumpPoint(PrintStream& out, ProgramPoint point)
{
    dumpID(out, point.block);
    out.print(":", point.index);
}

void dumpEdge(PrintStream& out, Edge edge)
{
    dumpID(out, edge.from);
    switch (edge.kind) {
    case EdgeKind::Normal:
        out.print(".", edge.targetIndex);
        return;
    case EdgeKind::Handler:
        out.print(".handler");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void dumpUse(PrintStream& out, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        dumpID(out, use.user);
        break;
    case Use::Kind::TerminatorOperand:
        dumpID(out, use.block);
        out.print(".terminator");
        break;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        dumpEdge(out, use.edge());
        break;
    case Use::Kind::BlockSubject:
        dumpID(out, use.block);
        out.print(".subject");
        return;
    }
    out.print("(", use.operandIndex, ")");
}

void dumpBoolean(PrintStream& out, bool value)
{
    out.print(value ? "yes" : "no");
}

void dumpValues(PrintStream& out, std::span<const ValueID> values)
{
    dumpList(out, values, [](PrintStream& out, ValueID value) { dumpID(out, value); });
}

void dumpBlocks(PrintStream& out, std::span<const BlockID> blocks)
{
    dumpList(out, blocks, [](PrintStream& out, BlockID block) { dumpID(out, block); });
}

void dumpObjects(PrintStream& out, std::span<const ObjectID> objects)
{
    dumpList(out, objects, [](PrintStream& out, ObjectID object) { dumpID(out, object); });
}

void dumpObjects(PrintStream& out, ObjectSet objects)
{
    Vector<ObjectID> members;
    for (ObjectID object : objects)
        members.append(object);
    dumpObjects(out, members.span());
}

template<typename Flag>
static void dumpFlags(PrintStream& out, OptionSet<Flag> flags)
{
    Vector<Flag, 16> members;
    for (Flag flag : allMembersOf<Flag>) {
        if (flags.contains(flag))
            members.append(flag);
    }
    dumpList(out, members.span(), [](PrintStream& out, Flag flag) { out.print(nameOf(flag)); });
}

void dumpOperandUses(PrintStream& out, OperandUses uses)
{
    if (uses == OperandUses::all()) {
        out.print("all");
        return;
    }
    dumpFlags(out, uses);
}

void dumpEscapeReasons(PrintStream& out, EscapeReasons reasons)
{
    dumpFlags(out, reasons);
}

static void dumpProofNames(PrintStream& out, ProofSet proofs)
{
    CommaPrinter separator(" "_s);
    for (Proof proof : allMembersOf<Proof>) {
        if (proofs.contains(proof))
            out.print(separator, nameOf(proof));
    }
}

void dumpProofs(PrintStream& out, ProofSet proofs)
{
    out.print("[");
    dumpProofNames(out, proofs);
    out.print("]");
}

static void dumpGuardOrigin(PrintStream& out, const GuardOrigin& origin)
{
    switch (origin.source) {
    case GuardSource::Test:
        dumpID(out, origin.test);
        return;
    case GuardSource::SwitchCase:
        dumpID(out, origin.switchBlock);
        out.print(".case", origin.caseIndex);
        return;
    case GuardSource::Allocation:
        out.print("allocation ");
        dumpID(out, origin.test);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpGuardFact(PrintStream& out, const Program& program, const GuardFact& fact)
{
    dumpProofNames(out, fact.proofs);
    out.print(" of ");
    if (fact.subject)
        dumpID(out, fact.subject);
    else
        out.print(emptyText);
    out.print(" by ");
    CommaPrinter alternative(" or "_s);
    for (const GuardOrigin& origin : fact.origins) {
        out.print(alternative);
        dumpGuardOrigin(out, origin);
    }
    out.print(" reads ");
    if (fact.dependsOn.isEmpty())
        out.print(emptyText);
    CommaPrinter separator(" "_s);
    for (const MemoryLocation& location : fact.dependsOn)
        out.print(separator, MemoryLocationDump { program, location });
}

void dumpGuardFacts(PrintStream& out, const Program& program, const GuardFacts& facts)
{
    if (!facts.isKnown()) {
        out.print("unknown");
        return;
    }
    out.print("[");
    CommaPrinter separator;
    for (const GuardFact& fact : facts.facts) {
        out.print(separator);
        dumpGuardFact(out, program, fact);
    }
    out.print("]");
}

template<typename T>
static void dumpOptional(PrintStream& out, const std::optional<T>& value, NOESCAPE const Invocable<void(PrintStream&, const T&)> auto& dumpValue)
{
    if (!value) {
        out.print(absentText);
        return;
    }
    dumpValue(out, *value);
}

static ASCIILiteral placementTextOf(ValuePlacement placement)
{
    switch (placement) {
    case ValuePlacement::Parameter:
        return "parameter"_s;
    case ValuePlacement::InBlock:
        return "value"_s;
    case ValuePlacement::Detached:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpLocation(PrintStream& out, ValueLocation location)
{
    dumpID(out, location.block);
    out.print(" ", placementTextOf(location.placement), " ", location.index);
}

static void dumpAnswer(PrintStream& out, bool answer) { dumpBoolean(out, answer); }
static void dumpAnswer(PrintStream& out, uint32_t answer) { out.print(answer); }
static void dumpAnswer(PrintStream& out, ASCIILiteral answer) { out.print(answer); }
static void dumpAnswer(PrintStream& out, ValueID answer) { dumpID(out, answer); }
static void dumpAnswer(PrintStream& out, BlockID answer) { dumpID(out, answer); }
static void dumpAnswer(PrintStream& out, ValueLocation answer) { dumpLocation(out, answer); }
static void dumpAnswer(PrintStream& out, const std::optional<Edge>& answer) { dumpOptional(out, answer, dumpEdge); }
static void dumpAnswer(PrintStream& out, const std::optional<Use>& answer) { dumpOptional(out, answer, dumpUse); }
static void dumpAnswer(PrintStream& out, std::span<const ValueID> answer) { dumpValues(out, answer); }
static void dumpAnswer(PrintStream& out, std::span<const BlockID> answer) { dumpBlocks(out, answer); }
static void dumpAnswer(PrintStream& out, std::span<const ObjectID> answer) { dumpObjects(out, answer); }
static void dumpAnswer(PrintStream& out, ObjectSet answer) { dumpObjects(out, answer); }
static void dumpAnswer(PrintStream& out, std::span<const Edge> answer) { dumpList(out, answer, dumpEdge); }
static void dumpAnswer(PrintStream& out, std::span<const Use> answer) { dumpList(out, answer, dumpUse); }
static void dumpAnswer(PrintStream& out, std::span<const ProgramPoint> answer) { dumpList(out, answer, dumpPoint); }
static void dumpAnswer(PrintStream& out, OperandUses answer) { dumpOperandUses(out, answer); }
static void dumpAnswer(PrintStream& out, EscapeReasons answer) { dumpEscapeReasons(out, answer); }
static void dumpAnswer(PrintStream& out, const EffectsOnEdges& answer) { out.print(answer); }

template<typename Answer>
static void dumpProperty(PrintStream& out, ASCIILiteral property, const Answer& answer)
{
    out.print(property, ": ");
    dumpAnswer(out, answer);
    out.print("\n");
}

template<typename Tag, typename Answer>
static void dumpProperty(PrintStream& out, ID<Tag> subject, ASCIILiteral property, const Answer& answer)
{
    dumpID(out, subject);
    out.print(" ");
    dumpProperty(out, property, answer);
}

static void dumpSectionName(PrintStream& out, FunctionAnalysis analysis)
{
    out.print(sectionNameOf(analysis), ":\n");
}

static void forEachListedValue(const UseLists& useLists, NOESCAPE const Invocable<void(ValueID)> auto& functor)
{
    for (unsigned index = 0; index < useLists.numberOfValues(); ++index) {
        ValueID value(index);
        if (useLists.locationOf(value).placement != ValuePlacement::Detached)
            functor(value);
    }
}

static BlockID handlerBlockOf(const CFG& cfg, BlockID block)
{
    return cfg.hasHandler(block) ? cfg.successors(block).back() : BlockID();
}

static void dumpControlFlow(PrintStream& out, const Function& function, const CFG& cfg)
{
    Vector<BlockID> unreachableBlocks;
    for (size_t index = 0; index < function.blocks.size(); ++index) {
        BlockID block(index);
        if (!cfg.isReachable(block))
            unreachableBlocks.append(block);
    }

    dumpSectionName(out, FunctionAnalysis::ControlFlow);
    dumpProperty(out, "reverse postorder"_s, cfg.reversePostOrder());
    dumpProperty(out, "unreachable"_s, unreachableBlocks.span());
    for (BlockID block : cfg.reversePostOrder()) {
        dumpProperty(out, block, "successors"_s, cfg.successors(block));
        dumpProperty(out, block, "predecessors"_s, cfg.predecessors(block));
        dumpProperty(out, block, "incoming edges"_s, cfg.incomingEdges(block));
        dumpProperty(out, block, "single incoming edge"_s, cfg.singleIncomingEdge(block));
        dumpProperty(out, block, "handler"_s, handlerBlockOf(cfg, block));
    }
}

static void dumpDominators(PrintStream& out, const CFG& cfg, const Dominators& dominators)
{
    dumpSectionName(out, FunctionAnalysis::Dominators);
    for (BlockID block : cfg.reversePostOrder()) {
        dumpProperty(out, block, "idom"_s, dominators.idom(block));
        dumpProperty(out, block, "children"_s, dominators.children(block));
    }
}

static void dumpNaturalLoops(PrintStream& out, const CFG& cfg, const NaturalLoops& naturalLoops)
{
    Vector<const NaturalLoop*> loops;
    Vector<BlockID> headers;
    for (BlockID block : cfg.reversePostOrder()) {
        if (const NaturalLoop* loop = naturalLoops.headerOf(block)) {
            loops.append(loop);
            headers.append(block);
        }
    }

    dumpSectionName(out, FunctionAnalysis::NaturalLoops);
    dumpProperty(out, "loop headers"_s, headers.span());
    for (const NaturalLoop* loop : loops) {
        Vector<BlockID> blocks;
        for (unsigned index = 0; index < loop->size(); ++index)
            blocks.append(loop->at(index));
        AOT::sort(blocks);
        const NaturalLoop* outerLoop = naturalLoops.innerMostOuterLoop(*loop);

        dumpProperty(out, loop->header(), "loop blocks"_s, blocks.span());
        dumpProperty(out, loop->header(), "outer loop header"_s, outerLoop ? outerLoop->header() : BlockID());
    }
}

static BlockID outermostCycleHeaderOf(const BlockCycles& blockCycles, BlockID block)
{
    std::optional<unsigned> cycle = blockCycles.outermostCycleOf(block);
    return cycle ? blockCycles.forest()[*cycle].header() : BlockID();
}

static void dumpBlockCycles(PrintStream& out, const CFG& cfg, const BlockCycles& blockCycles)
{
    dumpSectionName(out, FunctionAnalysis::Cycles);
    for (BlockID block : cfg.reversePostOrder()) {
        dumpProperty(out, block, "in cycle"_s, blockCycles.isInCycle(block));
        dumpProperty(out, block, "depth"_s, blockCycles.depthOf(block));
        dumpProperty(out, block, "outermost cycle"_s, outermostCycleHeaderOf(blockCycles, block));
    }
}

static void dumpUseLists(PrintStream& out, const Function& function, const UseLists& useLists)
{
    dumpSectionName(out, FunctionAnalysis::UseLists);
    Vector<ValueID> detachedValues;
    for (size_t index = 0; index < function.values.size(); ++index) {
        ValueID value(index);
        ValueLocation location = useLists.locationOf(value);
        if (location.placement == ValuePlacement::Detached) {
            detachedValues.append(value);
            continue;
        }
        auto uses = useLists.usesOf(value);
        Vector<ProgramPoint> usePoints;
        for (const Use& use : uses)
            usePoints.append(useLists.pointOf(use));

        dumpProperty(out, value, "location"_s, location);
        dumpProperty(out, value, "uses"_s, uses);
        dumpProperty(out, value, "use points"_s, usePoints.span());
    }
    dumpProperty(out, "detached"_s, detachedValues.span());
}

static void dumpIdentities(PrintStream& out, const UseLists& useLists, const ValueIdentities& identities)
{
    dumpSectionName(out, FunctionAnalysis::Identities);
    forEachListedValue(useLists, [&](ValueID value) {
        ValueID identity = identities.identityOf(value);
        if (identity != value)
            dumpProperty(out, value, "identity"_s, identity);
    });
}

void dumpLiveness(PrintStream& out, const CFG& cfg, const Liveness& liveness, const BitVector* trackedBlocks)
{
    dumpSectionName(out, FunctionAnalysis::Liveness);
    for (BlockID block : cfg.reversePostOrder()) {
        if (trackedBlocks && !trackedBlocks->get(block.index()))
            continue;
        dumpProperty(out, block, "live at head"_s, liveness.liveAtHead(block));
    }
}

static void dumpEffects(PrintStream& out, FunctionAnalyses& analyses)
{
    dumpSectionName(out, FunctionAnalysis::Effects);
    forEachListedValue(analyses.useLists(), [&](ValueID value) {
        dumpProperty(out, value, "effects"_s, analyses.effectsOf(value));
    });
    for (BlockID block : analyses.cfg().reversePostOrder())
        dumpProperty(out, block, "terminator effects"_s, analyses.effectsOfTerminator(block));
}

static void dumpTrackedObject(PrintStream& out, const EscapeAnalysis& escape, ObjectID object)
{
    Vector<Use> objectUses;
    Vector<Use> escapingUses;
    for (const ObjectUse& objectUse : escape.usesOf(object)) {
        objectUses.append(objectUse.use);
        if (objectUse.isEscape)
            escapingUses.append(objectUse.use);
    }

    const TrackedObject& trackedObject = escape.at(object);
    dumpProperty(out, object, "definition"_s, trackedObject.definition);
    dumpProperty(out, object, "kind"_s, enumName(trackedObject.kind));
    dumpProperty(out, object, "state"_s, enumName(trackedObject.state));
    dumpProperty(out, object, "reasons"_s, trackedObject.reasons);
    dumpProperty(out, object, "first escape"_s, trackedObject.firstEscape);
    dumpProperty(out, object, "uses"_s, trackedObject.uses);
    dumpProperty(out, object, "live instances"_s, enumName(escape.liveInstancesOf(object)));
    dumpProperty(out, object, "aliases"_s, escape.aliasesOf(object));
    dumpProperty(out, object, "object uses"_s, objectUses.span());
    dumpProperty(out, object, "escaping uses"_s, escapingUses.span());
    dumpProperty(out, object, "contents"_s, escape.contentsOf(object));
    dumpProperty(out, object, "containers"_s, escape.containersOf(object));
}

static void dumpEscape(PrintStream& out, const UseLists& useLists, const EscapeAnalysis& escape)
{
    dumpSectionName(out, FunctionAnalysis::Escape);
    if (escape.gaveUp())
        dumpProperty(out, "gave up"_s, "true"_s);
    dumpProperty(out, "objects"_s, escape.numberOfObjects());
    for (unsigned index = 0; index < escape.numberOfObjects(); ++index)
        dumpTrackedObject(out, escape, ObjectID(index));
    forEachListedValue(useLists, [&](ValueID value) {
        PointsToSet pointsTo = escape.pointsTo(value);
        if (pointsTo.objects.isEmpty())
            return;
        dumpProperty(out, value, "points to"_s, pointsTo.objects);
        dumpProperty(out, value, "may be untracked"_s, pointsTo.mayBeUntracked);
    });
}

void dumpUseEffectHistory(PrintStream& out, const CFG& cfg, const EscapeAnalysis& escape, const UseEffectHistory& history)
{
    dumpSectionName(out, FunctionAnalysis::EscapeHistory);
    for (BlockID block : cfg.reversePostOrder()) {
        Vector<ObjectID> occurredObjects;
        Vector<ObjectID> unknownObjects;
        for (unsigned index = 0; index < escape.numberOfObjects(); ++index) {
            ObjectID object(index);
            switch (history.occurredBefore(object, ProgramPoint { block, 0 })) {
            case Occurrence::MayHaveOccurred:
                occurredObjects.append(object);
                break;
            case Occurrence::Unknown:
                unknownObjects.append(object);
                break;
            case Occurrence::HasNotOccurred:
                break;
            }
        }
        dumpProperty(out, block, "occurred at head"_s, occurredObjects.span());
        dumpProperty(out, block, "unknown at head"_s, unknownObjects.span());
    }
}

static void dumpSection(PrintStream& out, FunctionAnalyses& analyses, FunctionAnalysis analysis)
{
    switch (analysis) {
    case FunctionAnalysis::ControlFlow:
        dumpControlFlow(out, analyses.function(), analyses.cfg());
        return;
    case FunctionAnalysis::Dominators:
        dumpDominators(out, analyses.cfg(), analyses.dominators());
        return;
    case FunctionAnalysis::NaturalLoops:
        dumpNaturalLoops(out, analyses.cfg(), analyses.naturalLoops());
        return;
    case FunctionAnalysis::Cycles:
        dumpBlockCycles(out, analyses.cfg(), analyses.blockCycles());
        return;
    case FunctionAnalysis::UseLists:
        dumpUseLists(out, analyses.function(), analyses.useLists());
        return;
    case FunctionAnalysis::Identities:
        dumpIdentities(out, analyses.useLists(), analyses.identities());
        return;
    case FunctionAnalysis::Liveness:
        dumpLiveness(out, analyses.cfg(), analyses.liveness(), nullptr);
        return;
    case FunctionAnalysis::Effects:
        dumpEffects(out, analyses);
        return;
    case FunctionAnalysis::Escape:
        dumpEscape(out, analyses.useLists(), analyses.escapeWithLiveInstances());
        return;
    case FunctionAnalysis::EscapeHistory:
        dumpUseEffectHistory(out, analyses.cfg(), analyses.escape(), analyses.escapeHistory());
        return;
    case FunctionAnalysis::Frequencies:
        dumpSectionName(out, analysis);
        analyses.blockFrequencies().dump(out, analyses.function());
        return;
    case FunctionAnalysis::Guards:
        dumpSectionName(out, analysis);
        analyses.guards().dump(out);
        return;
    case FunctionAnalysis::FailedTests:
        dumpSectionName(out, analysis);
        return;
    case FunctionAnalysis::Induction:
        dumpSectionName(out, analysis);
        analyses.inductionVariables().dump(out);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void dumpAnalyses(PrintStream& out, FunctionAnalyses& analyses, AnalysisSet set)
{
    for (FunctionAnalysis analysis : set)
        dumpSection(out, analyses, analysis);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
