/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAnalysisTestCommands.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/EnumTraits.h>

namespace JSC { namespace AOT { namespace Typed {

template<typename T> static const T* pointerOrNull(const std::optional<T>& optional) { return optional ? &*optional : nullptr; }

template<typename T>
static void forEachAscendingPair(const Vector<T>& items, NOESCAPE const Invocable<void(T, T)> auto& functor)
{
    for (size_t first = 0; first < items.size(); ++first) {
        for (size_t second = first + 1; second < items.size(); ++second)
            functor(items[first], items[second]);
    }
}

static void dumpItem(PrintStream& out, const EffectsOnEdges& effects) { out.print(effects); }

static void dumpItem(PrintStream& out, const Target& handler)
{
    dumpItems(out, handler.block, "("_s);
    CommaPrinter separator(", "_s);
    for (ValueID argument : handler.arguments) {
        out.print(separator);
        dumpID(out, argument);
    }
    out.print(")");
}

static void dumpItem(PrintStream& out, const BaseObject& baseObject)
{
    out.print(enumName(baseObject.kind));
    if (baseObject.kind == BaseObject::Kind::EntryParameter)
        out.print("(", baseObject.entryParameter, ")");
}

static void dumpItem(PrintStream& out, ObjectSet objects)
{
    Vector<ObjectID> members;
    for (ObjectID object : objects)
        members.append(object);
    dumpItem(out, members);
}

template<typename T>
static void dumpItem(PrintStream& out, const std::optional<T>& item)
{
    if (item)
        dumpItem(out, *item);
    else
        out.print("none");
}

namespace {

struct AnalysisOptions {
    bool hasLivenessOptions() const { return !additionalEdges.isEmpty() || trackedValues || trackedBlocks; }
    bool hasHistoryOptions() const { return historyUses || ignoredBlocks; }
    bool isTracked(BlockID block) const { return !trackedBlocks || trackedBlocks->get(block.index()); }

    Vector<std::pair<BlockID, BlockID>> additionalEdges;
    std::optional<BitVector> trackedValues;
    std::optional<BitVector> trackedBlocks;
    std::optional<OperandUses> historyUses;
    std::optional<BitVector> ignoredBlocks;
    std::optional<BitVector> aliasValues;
    std::optional<BlockID> reachableFrom;
    BitVector barrier;
};

enum class SectionRequirement : uint8_t { None, ReachableFromWord, TrackedSuccessors };

} // anonymous namespace

static void forEachSection(PrintStream& out, Session& session, const AnalysisOptions& options, NOESCAPE const auto& section)
{
    const Program& program = session.analysisContext.program;
    const Function& function = session.function;
    FunctionAnalyses& analyses = session.analyses;
    std::optional<Liveness> livenessWithOptions;
    std::optional<UseEffectHistory> historyWithOptions;

    auto isListed = [&](ValueID value) { return analyses.useLists().locationOf(value).placement != ValuePlacement::Detached; };
    auto listedValues = [&] { return idsMatching<ValueID>(function.values.size(), isListed); };
    auto reachableBlocksMatching = [&](const auto& matches) {
        return idsMatching<BlockID>(function.blocks.size(), [&](BlockID block) { return analyses.cfg().isReachable(block) && matches(block); });
    };
    auto reachableBlocks = [&] { return reachableBlocksMatching([](BlockID) { return true; }); };
    auto checkSolversAgree = [&](const Liveness& solved, LivenessOptions livenessOptions) {
        for (auto solver : { LivenessOptions::Solver::PerValue, LivenessOptions::Solver::SharedSets }) {
            livenessOptions.solver = solver;
            Liveness other(function, analyses.cfg(), analyses.useLists(), livenessOptions);
            for (BlockID block : analyses.cfg().reversePostOrder()) {
                RELEASE_ASSERT(std::ranges::equal(solved.liveAtHead(block), other.liveAtHead(block)));
                RELEASE_ASSERT(solved.isAnyLiveAtHead(block) == other.isAnyLiveAtHead(block));
            }
        }
    };
    auto liveness = [&]() -> const Liveness& {
        if (!options.hasLivenessOptions()) {
            const Liveness& result = analyses.liveness();
            checkSolversAgree(result, { });
            return result;
        }
        if (!livenessWithOptions) {
            LivenessOptions livenessOptions { pointerOrNull(options.trackedValues), pointerOrNull(options.trackedBlocks), options.additionalEdges.span() };
            livenessWithOptions.emplace(function, analyses.cfg(), analyses.useLists(), livenessOptions);
            checkSolversAgree(*livenessWithOptions, livenessOptions);
        }
        return *livenessWithOptions;
    };
    auto history = [&]() -> const UseEffectHistory& {
        if (!options.hasHistoryOptions())
            return analyses.escapeHistory();
        if (!historyWithOptions)
            historyWithOptions.emplace(analyses, UseEffectHistoryOptions { options.historyUses.value_or(OperandUses()), pointerOrNull(options.ignoredBlocks) });
        return *historyWithOptions;
    };
    auto objectsWithOccurrence = [&](Occurrence occurrence, ProgramPoint point) {
        return idsMatching<ObjectID>(analyses.escape().numberOfObjects(), [&](ObjectID object) { return history().occurredBefore(object, point) == occurrence; });
    };

    section("control-flow"_s, [&] {
        const CFG& cfg = analyses.cfg();
        dumpLine(out, "reverse postorder: "_s, cfg.reversePostOrder());
        dumpLine(out, "unreachable: "_s, idsMatching<BlockID>(function.blocks.size(), [&](BlockID block) { return !cfg.isReachable(block); }));
        for (BlockID block : cfg.reversePostOrder()) {
            dumpLine(out, block, " successors: "_s, cfg.successors(block));
            dumpLine(out, block, " predecessors: "_s, cfg.predecessors(block));
            dumpLine(out, block, " incoming edges: "_s, cfg.incomingEdges(block));
            dumpLine(out, block, " single incoming edge: "_s, cfg.singleIncomingEdge(block));
            dumpLine(out, block, " handler: "_s, cfg.hasHandler(block) ? cfg.successors(block).back() : BlockID());
        }
    });
    section("dominators"_s, [&] {
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            dumpLine(out, block, " idom: "_s, analyses.dominators().idom(block));
            dumpLine(out, block, " children: "_s, analyses.dominators().children(block));
        }
    });
    section("natural-loops"_s, [&] {
        const NaturalLoops& naturalLoops = analyses.naturalLoops();
        Vector<BlockID> headers;
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            if (naturalLoops.headerOf(block))
                headers.append(block);
        }
        dumpLine(out, "loop headers: "_s, headers);
        for (BlockID header : headers) {
            const NaturalLoop& loop = *naturalLoops.headerOf(header);
            Vector<BlockID> blocks;
            for (unsigned index = 0; index < loop.size(); ++index)
                blocks.append(loop.at(index));
            AOT::sort(blocks);
            const NaturalLoop* outerLoop = naturalLoops.innerMostOuterLoop(loop);
            dumpLine(out, header, " loop blocks: "_s, blocks);
            dumpLine(out, header, " outer loop header: "_s, outerLoop ? outerLoop->header() : BlockID());
        }
    });
    section("cycles"_s, [&] {
        const BlockCycles& blockCycles = analyses.blockCycles();
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            std::optional<unsigned> outermostCycle = blockCycles.outermostCycleOf(block);
            dumpLine(out, block, " in cycle: "_s, blockCycles.isInCycle(block));
            dumpLine(out, block, " depth: "_s, blockCycles.depthOf(block));
            dumpLine(out, block, " outermost cycle: "_s, outermostCycle ? blockCycles.forest()[*outermostCycle].header() : BlockID());
        }
    });
    section("use-lists"_s, [&] { dumpUseLists(out, function, analyses.useLists()); });
    section("identities"_s, [&] {
        ValueIdentities withoutShortcuts(function, analyses.cfg(), ValueIdentities::RepeatedWalks::StartAtEntry);
        ValueIdentities onDemand(function, analyses.cfg(), ValueIdentities::RepeatedWalks::StartAtFirstChangedBlock, nullptr, ValueIdentities::Evaluation::OnDemand);
        for (uint32_t index = 0; index < function.values.size(); ++index) {
            ValueID identity = withoutShortcuts.identityOf(ValueID(index));
            RELEASE_ASSERT(analyses.identityOf(ValueID(index)) == identity);
            ValueID resolved = onDemand.tryResolve(ValueID(index));
            RELEASE_ASSERT(!resolved || resolved == identity);
        }
        for (ValueID value : listedValues()) {
            if (ValueID identity = analyses.identities().identityOf(value); identity != value)
                dumpLine(out, value, " identity: "_s, identity);
        }
    });
    section("liveness"_s, [&] {
        const Liveness& solved = liveness();
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            if (options.isTracked(block))
                dumpLine(out, block, " live at head: "_s, std::span<const ValueID>(solved.liveAtHead(block)));
        }
    });
    section("effects"_s, [&] {
        for (ValueID value : listedValues())
            dumpLine(out, value, " effects: "_s, analyses.effectsOf(value));
        for (BlockID block : analyses.cfg().reversePostOrder())
            dumpLine(out, block, " terminator effects: "_s, analyses.effectsOfTerminator(block));
    });
    section("escape"_s, [&] {
        const EscapeAnalysis& escape = analyses.escapeWithLiveInstances();
        if (escape.gaveUp())
            dumpLine(out, "gave up: true"_s);
        dumpLine(out, "objects: "_s, escape.numberOfObjects());
        for (unsigned index = 0; index < escape.numberOfObjects(); ++index) {
            ObjectID object(index);
            Vector<Use> objectUses;
            Vector<Use> escapingUses;
            for (const ObjectUse& objectUse : escape.usesOf(object)) {
                objectUses.append(objectUse.use);
                if (objectUse.isEscape)
                    escapingUses.append(objectUse.use);
            }

            const TrackedObject& trackedObject = escape.at(object);
            dumpLine(out, object, " definition: "_s, trackedObject.definition);
            dumpLine(out, object, " kind: "_s, enumName(trackedObject.kind));
            dumpLine(out, object, " state: "_s, enumName(trackedObject.state));
            dumpLine(out, object, " reasons: "_s, trackedObject.reasons);
            dumpLine(out, object, " first escape: "_s, trackedObject.firstEscape);
            dumpLine(out, object, " uses: "_s, trackedObject.uses);
            dumpLine(out, object, " live instances: "_s, enumName(escape.liveInstancesOf(object)));
            dumpLine(out, object, " aliases: "_s, escape.aliasesOf(object));
            dumpLine(out, object, " object uses: "_s, objectUses);
            dumpLine(out, object, " escaping uses: "_s, escapingUses);
            dumpLine(out, object, " contents: "_s, escape.contentsOf(object));
            dumpLine(out, object, " containers: "_s, escape.containersOf(object));
        }
        for (ValueID value : listedValues()) {
            PointsToSet pointsTo = escape.pointsTo(value);
            if (pointsTo.objects.isEmpty())
                continue;
            dumpLine(out, value, " points to: "_s, pointsTo.objects);
            dumpLine(out, value, " may be untracked: "_s, pointsTo.mayBeUntracked);
        }
    });
    section("escape-history"_s, [&] {
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            dumpLine(out, block, " occurred at head: "_s, objectsWithOccurrence(Occurrence::MayHaveOccurred, { block, 0 }));
            dumpLine(out, block, " unknown at head: "_s, objectsWithOccurrence(Occurrence::Unknown, { block, 0 }));
        }
    });
    section("common-dominators"_s, [&] {
        forEachAscendingPair(reachableBlocks(), [&](BlockID first, BlockID second) {
            dumpLine(out, first, " "_s, second, " nearest common dominator: "_s, analyses.dominators().nearestCommonDominator(first, second));
        });
    });
    section("value-dominance"_s, [&] {
        for (ValueID value : listedValues()) {
            auto isAvailableAtHead = [&](BlockID block) { return analyses.dominates(value, ProgramPoint { block, 0 }); };
            dumpLine(out, value, " available at head of: "_s, reachableBlocksMatching(isAvailableAtHead));
        }
    });
    section("edge-dominance"_s, [&] {
        for (BlockID from : reachableBlocks()) {
            forEachOutgoingEdge(function, from, [&](Edge edge, const Target&) {
                auto isDominated = [&](BlockID block) { return analyses.dominates(edge, block); };
                dumpLine(out, edge, " dominates: "_s, reachableBlocksMatching(isDominated));
            });
        }
    });
    section("live-points"_s, [&] {
        const Liveness& solved = liveness();
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            if (!options.isTracked(block))
                continue;
            uint32_t numberOfValues = function.at(block).values.size();
            Vector<Vector<ValueID>> liveValues(numberOfValues + 1);
            Liveness::LocalCalc localCalc(solved, block);
            for (uint32_t index = numberOfValues; ; --index) {
                localCalc.forEachLive([&](ValueID value) { liveValues[index].append(value); });
                if (!index)
                    break;
                localCalc.execute(index - 1);
            }
            for (uint32_t index = 0; index <= numberOfValues; ++index)
                dumpLine(out, ProgramPoint { block, index }, " live: "_s, liveValues[index]);
        }
    }, SectionRequirement::TrackedSuccessors);
    section("value-identity"_s, [&] {
        for (ValueID value : listedValues()) {
            dumpLine(out, value, " same object: "_s, sameObjectThroughCasts(function, value).valueInFrontOfFacts());
            dumpLine(out, value, " through upcasts: "_s, definitionThroughUpcasts(function, value));
            dumpLine(out, value, " identity: "_s, analyses.identityOf(value));
            dumpLine(out, value, " constant: "_s, constantThroughCasts(program, function, value).value_or(ValueID()));
            dumpLine(out, value, " rematerializable: "_s, isRematerializable(function.at(value)));
        }
    });
    section("incoming-values"_s, [&] {
        auto printIncomingValue = [&](Edge edge, const IncomingValue& incoming) {
            switch (incoming.kind) {
            case IncomingValue::Kind::Argument:
                dumpLine(out, incoming.argument);
                return;
            case IncomingValue::Kind::EdgeResult:
                break;
            }
            const EdgeResult& result = edgeResultAt(function, incoming.result);
            switch (result.kind) {
            case EdgeResultKind::NarrowedOperand:
                dumpItems(out, "narrowed "_s, result.subject, " by "_s);
                break;
            case EdgeResultKind::Guard:
                dumpItems(out, "guard by "_s);
                break;
            case EdgeResultKind::DeclaredResult:
                dumpLine(out, "declared result"_s);
                return;
            }
            if (result.test) {
                dumpLine(out, result.test);
                return;
            }
            RELEASE_ASSERT(result.caseIndex);
            dumpLine(out, edge.from, ".case"_s, *result.caseIndex);
        };
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            const CompactList<ValueID>& parameters = function.at(block).parameters;
            for (uint32_t index = 0; index < parameters.size(); ++index) {
                for (Edge edge : analyses.cfg().incomingEdges(block)) {
                    dumpItems(out, parameters[index], " from "_s, edge, ": "_s);
                    printIncomingValue(edge, incomingValueOf(function, edge, index));
                }
            }
        }
    });
    section("object-uses"_s, [&] {
        for (unsigned index = 0; index < analyses.escape().numberOfObjects(); ++index) {
            ObjectID object(index);
            for (const ObjectUse& objectUse : analyses.escape().usesOf(object)) {
                dumpLine(out, object, " "_s, objectUse.use, " uses: "_s, objectUse.uses);
                dumpLine(out, object, " "_s, objectUse.use, " is escape: "_s, objectUse.isEscape);
                dumpLine(out, object, " "_s, objectUse.use, " is contents store: "_s, objectUse.isContentsStore);
            }
        }
    });
    section("history-points"_s, [&] {
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            for (uint32_t index = 0; index <= function.at(block).values.size(); ++index) {
                ProgramPoint point { block, index };
                dumpLine(out, point, " occurred: "_s, objectsWithOccurrence(Occurrence::MayHaveOccurred, point));
                dumpLine(out, point, " unknown: "_s, objectsWithOccurrence(Occurrence::Unknown, point));
            }
        }
    });
    section("aliases"_s, [&] {
        auto mayBeObject = [&](ValueID value) { return isListed(value) && program.typeOf(function, value).hasObjects(); };
        Vector<ValueID> values = options.aliasValues ? idsIn<ValueID>(*options.aliasValues) : idsMatching<ValueID>(function.values.size(), mayBeObject);
        forEachAscendingPair(values, [&](ValueID first, ValueID second) {
            dumpLine(out, first, " "_s, second, " alias: "_s, enumName(analyses.escapeWithLiveInstances().alias(first, second)));
        });
    });
    section("parameter-summaries"_s, [&] {
        const CompactList<ValueID>& parameters = function.at(entryBlock).parameters;
        for (uint32_t index = 0; index < parameters.size(); ++index) {
            OperandSummary summary = summarizeEntryParameter(analyses, index);
            dumpLine(out, parameters[index], " uses: "_s, summary.uses);
            dumpItems(out, parameters[index], " retained in: "_s, nameOf(summary.retainedIn));
            if (summary.retainedIn == RetainedIn::Operand)
                dumpItems(out, " "_s, summary.retainingOperand);
            out.print("\n");
        }
    });
    section("base-objects"_s, [&] {
        for (ValueID value : listedValues()) {
            Vector<BaseObject> baseObjects;
            forEachBaseObject(analyses, value, [&](const BaseObject& baseObject) { baseObjects.append(baseObject); });
            dumpLine(out, value, " base objects: "_s, baseObjects);
        }
    });
    section("handler-edges"_s, [&] {
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            auto isDominated = [&](BlockID other) { return analyses.dominators().terminatorDominates(block, other); };
            dumpLine(out, block, " handler: "_s, std::optional<Target>(function.at(block).handler));
            dumpLine(out, block, " terminator dominates: "_s, reachableBlocksMatching(isDominated));
        }
    });
    section("reachability"_s, [&] {
        BlockID start = *options.reachableFrom;
        dumpLine(out, start, " reaches: "_s, idsIn<BlockID>(analyses.cfg().blocksReachableFrom(start, options.barrier)));
    }, SectionRequirement::ReachableFromWord);
    section("cycles-avoiding-blocks"_s, [&] {
        for (BlockID block : reachableBlocks()) {
            auto isAvoided = [&](BlockID avoidedBlock) {
                uint64_t numberOfBlocksVisited = 0;
                bool isInCycle = analyses.cfg().isInCycleAvoiding(block, avoidedBlock);
                RELEASE_ASSERT(analyses.blockCycles().isInCycleAvoiding(block, avoidedBlock, numberOfBlocksVisited) == isInCycle);
                return isInCycle;
            };
            dumpLine(out, block, " in cycle avoiding: "_s, reachableBlocksMatching(isAvoided));
        }
    });
    section("caught-values"_s, [&] {
        for (BlockID block : reachableBlocks())
            dumpLine(out, block, " index after caught values: "_s, indexAfterCaughtValues(function, function.at(block)));
    });
    section("execution-counts"_s, [&] {
        for (BlockID block : reachableBlocks())
            dumpLine(out, block, " "_s, nameOf(analyses.executionCountOf(block)));
    });
}

static AnalysisOptions parseAnalysisOptions(CommandWords& words, const Function& function)
{
    auto parseBlock = blockParser(function);
    auto parseBlocks = collectionParser<BitVector>(parseBlock);
    auto parseValues = collectionParser<BitVector>(valueParser(function));
    auto parseOperandUse = [](StringView name) { return parseEnum<OperandUse>(name); };

    AnalysisOptions options;
    options.additionalEdges = words.parseAll("additional-edge"_s, pairParser(':', parseBlock, parseBlock));
    options.trackedValues = words.parseOptional("tracked-values"_s, parseValues);
    options.trackedBlocks = words.parseOptional("tracked-blocks"_s, parseBlocks);
    options.historyUses = words.parseOptional("history-uses"_s, collectionParser<OperandUses>(parseOperandUse));
    options.ignoredBlocks = words.parseOptional("ignored-blocks"_s, parseBlocks);
    options.aliasValues = words.parseOptional("alias-values"_s, parseValues);
    options.reachableFrom = words.parseOptional("reachable-from"_s, parseBlock);
    options.barrier = words.parseOptional("barrier"_s, parseBlocks).value_or(BitVector());
    return options;
}

static bool hasOnlyTrackedSuccessors(const CFG& cfg, const AnalysisOptions& options)
{
    Vector<std::pair<BlockID, BlockID>> edges = options.additionalEdges;
    for (BlockID block : cfg.reversePostOrder()) {
        for (BlockID successor : cfg.successors(block))
            edges.append({ block, successor });
    }
    return std::ranges::none_of(edges, [&](const auto& edge) {
        return cfg.isReachable(edge.first) && cfg.isReachable(edge.second) && options.isTracked(edge.first) && !options.isTracked(edge.second);
    });
}

void runAnalyses(TestCommandContext& context)
{
    Program& program = *context.program;
    CommandWords words(context);
    FlowPreparation preparation = prepareFlow(context, words);
    std::optional<FunctionID> onlyFunction = words.parseOptional("function"_s, functionParser(program));
    SavedFrames savedFrames = words.parseFlag("as-the-verifier"_s) ? SavedFrames::AreOutOfReach : SavedFrames::AreUnknownMemory;
    if (words.hasFailed())
        return;

    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        if (!program.at(id).hasBody() || (onlyFunction && *onlyFunction != id))
            continue;

        Session session(context, id, preparation.flowOrNull(), preparation.effectsOrNull(), savedFrames);
        CommandWords optionWords = words;
        AnalysisOptions options = parseAnalysisOptions(optionWords, session.function);
        Vector<bool> isRequested;
        forEachSection(context.out, session, options, [&](ASCIILiteral name, const auto&, SectionRequirement requirement = SectionRequirement::None) {
            isRequested.append(optionWords.parseFlag(name));
            if (!isRequested.last())
                return;
            if (requirement == SectionRequirement::ReachableFromWord && !options.reachableFrom)
                optionWords.failMissingArgument();
            if (requirement == SectionRequirement::TrackedSuccessors && !hasOnlyTrackedSuccessors(session.analyses.cfg(), options))
                optionWords.fail("tracked-blocks"_s);
        });
        if (!optionWords.finish())
            return;

        context.out.println("function f", id.index());
        size_t sectionIndex = 0;
        forEachSection(context.out, session, options, [&](ASCIILiteral name, const auto& print, SectionRequirement = SectionRequirement::None) {
            if (!isRequested[sectionIndex++])
                return;
            context.out.println(name, ":");
            print();
        });
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
