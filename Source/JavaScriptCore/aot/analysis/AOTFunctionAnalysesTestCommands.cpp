/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAnalysisTestCommands.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTEffectSummaries.h"
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

static void dumpItem(PrintStream& out, const std::optional<Target>& handler)
{
    if (!handler) {
        dumpID(out, BlockID());
        return;
    }
    dumpID(out, handler->block);
    out.print("(");
    CommaPrinter separator(", "_s);
    for (ValueID argument : handler->arguments) {
        out.print(separator);
        dumpID(out, argument);
    }
    out.print(")");
}

static void dumpItem(PrintStream& out, const Vector<BaseObject>& baseObjects)
{
    dumpList(out, baseObjects.span(), [](PrintStream& out, const BaseObject& baseObject) {
        out.print(enumName(baseObject.kind));
        if (baseObject.kind == BaseObject::Kind::EntryParameter)
            out.print("(", baseObject.entryParameter, ")");
    });
}

namespace {

struct TestReference {
    BlockID block;
    ValueID test;
    std::optional<uint32_t> caseIndex;
};

void dumpItem(PrintStream& out, const TestReference& reference)
{
    if (reference.test) {
        dumpID(out, reference.test);
        return;
    }
    RELEASE_ASSERT(reference.caseIndex);
    dumpID(out, reference.block);
    out.print(".case", *reference.caseIndex);
}

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
    std::optional<BitVector> baseObjectValues;
    std::optional<BlockID> reachableFrom;
    BitVector barrier;
};

struct SectionPrinter {
    void checkIdentityShortcuts()
    {
        ValueIdentities withoutShortcuts(function, analyses.cfg(), ValueIdentities::RepeatedWalks::StartAtEntry);
        ValueIdentities onDemand(function, analyses.cfg(), ValueIdentities::RepeatedWalks::StartAtFirstChangedBlock, nullptr, ValueIdentities::Evaluation::OnDemand);
        for (uint32_t index = 0; index < function.values.size(); ++index) {
            ValueID identity = withoutShortcuts.identityOf(ValueID(index));
            RELEASE_ASSERT(analyses.identityOf(ValueID(index)) == identity);
            ValueID resolved = onDemand.tryResolve(ValueID(index));
            RELEASE_ASSERT(!resolved || resolved == identity);
        }
    }

    void printCachedSection(FunctionAnalysis analysis)
    {
        if (analysis == FunctionAnalysis::Identities)
            checkIdentityShortcuts();
        if (analysis == FunctionAnalysis::Liveness)
            dumpLiveness(out, analyses.cfg(), liveness(), pointerOrNull(options.trackedBlocks));
        else if (analysis == FunctionAnalysis::EscapeHistory && options.hasHistoryOptions())
            dumpUseEffectHistory(out, analyses.cfg(), analyses.escape(), history());
        else
            dumpAnalyses(out, analyses, analysis);
    }

    void printCommonDominators()
    {
        forEachAscendingPair(reachableBlocks(), [&](BlockID first, BlockID second) {
            dumpLine(out, first, " "_s, second, " nearest common dominator: "_s, analyses.dominators().nearestCommonDominator(first, second));
        });
    }

    void printValueDominance()
    {
        for (ValueID value : listedValues()) {
            auto isAvailableAtHead = [&](BlockID block) { return analyses.dominates(value, ProgramPoint { block, 0 }); };
            dumpLine(out, value, " available at head of: "_s, reachableBlocksMatching(isAvailableAtHead));
        }
    }

    void printEdgeDominance()
    {
        for (Edge edge : reachableEdges()) {
            auto isDominated = [&](BlockID block) { return analyses.dominates(edge, block); };
            dumpLine(out, edge, " dominates: "_s, reachableBlocksMatching(isDominated));
        }
    }

    void printLivePoints()
    {
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            if (!options.isTracked(block))
                continue;
            uint32_t numberOfValues = function.at(block).values.size();
            Vector<Vector<ValueID>> liveValues(numberOfValues + 1);
            Liveness::LocalCalc localCalc(liveness(), block);
            for (uint32_t index = numberOfValues; ; --index) {
                localCalc.forEachLive([&](ValueID value) { liveValues[index].append(value); });
                if (!index)
                    break;
                localCalc.execute(index - 1);
            }
            for (uint32_t index = 0; index <= numberOfValues; ++index)
                dumpLine(out, ProgramPoint { block, index }, " live: "_s, liveValues[index]);
        }
    }

    void printValueIdentity()
    {
        for (ValueID value : listedValues()) {
            dumpLine(out, value, " same object: "_s, sameObjectThroughCasts(function, value));
            dumpLine(out, value, " through upcasts: "_s, definitionThroughUpcasts(function, value));
            dumpLine(out, value, " identity: "_s, analyses.identityOf(value));
            dumpLine(out, value, " constant: "_s, constantThroughCasts(context.program, function, value).value_or(ValueID()));
            dumpLine(out, value, " rematerializable: "_s, isRematerializable(function.at(value)));
        }
    }

    void printIncomingValues()
    {
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            const CompactList<ValueID>& parameters = function.at(block).parameters;
            for (uint32_t index = 0; index < parameters.size(); ++index) {
                for (Edge edge : analyses.cfg().incomingEdges(block))
                    printIncomingValue(parameters[index], edge, incomingValueOf(function, edge, index));
            }
        }
    }

    void printObjectUses()
    {
        for (unsigned index = 0; index < analyses.escape().numberOfObjects(); ++index) {
            ObjectID object(index);
            for (const ObjectUse& objectUse : analyses.escape().usesOf(object)) {
                dumpLine(out, object, " "_s, objectUse.use, " uses: "_s, objectUse.uses);
                dumpLine(out, object, " "_s, objectUse.use, " is escape: "_s, objectUse.isEscape);
                dumpLine(out, object, " "_s, objectUse.use, " is contents store: "_s, objectUse.isContentsStore);
            }
        }
    }

    void printHistoryPoints()
    {
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            for (uint32_t index = 0; index <= function.at(block).values.size(); ++index) {
                ProgramPoint point { block, index };
                dumpLine(out, point, " occurred: "_s, objectsWithOccurrence(Occurrence::MayHaveOccurred, point));
                dumpLine(out, point, " unknown: "_s, objectsWithOccurrence(Occurrence::Unknown, point));
            }
        }
    }

    void printAliases()
    {
        auto mayBeObject = [&](ValueID value) { return isListed(value) && context.program.typeOf(function, value).hasObjects(); };
        Vector<ValueID> values = options.aliasValues ? idsIn<ValueID>(*options.aliasValues) : idsMatching<ValueID>(function.values.size(), mayBeObject);
        forEachAscendingPair(values, [&](ValueID first, ValueID second) {
            dumpLine(out, first, " "_s, second, " alias: "_s, enumName(analyses.escapeWithLiveInstances().alias(first, second)));
        });
    }

    void printParameterSummaries()
    {
        const CompactList<ValueID>& parameters = function.at(entryBlock).parameters;
        for (uint32_t index = 0; index < parameters.size(); ++index)
            printSummary(parameters[index], summarizeEntryParameter(analyses, index));
    }

    void printBaseObjects()
    {
        Vector<ValueID> values = options.baseObjectValues ? idsIn<ValueID>(*options.baseObjectValues) : listedValues();
        for (ValueID value : values) {
            Vector<BaseObject> baseObjects;
            forEachBaseObject(analyses, value, [&](const BaseObject& baseObject) { baseObjects.append(baseObject); });
            dumpLine(out, value, " base objects: "_s, baseObjects);
        }
    }

    void printHandlerEdges()
    {
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            auto isDominated = [&](BlockID other) { return analyses.dominators().terminatorDominates(block, other); };
            dumpLine(out, block, " handler: "_s, function.at(block).handler);
            dumpLine(out, block, " terminator dominates: "_s, reachableBlocksMatching(isDominated));
        }
    }

    void printReachability()
    {
        BlockID start = *options.reachableFrom;
        dumpLine(out, start, " reaches: "_s, idsIn<BlockID>(analyses.cfg().blocksReachableFrom(start, options.barrier)));
    }

    void printCyclesAvoidingBlocks()
    {
        for (BlockID block : reachableBlocks()) {
            auto isAvoided = [&](BlockID avoidedBlock) { return analyses.cfg().isInCycleAvoiding(block, avoidedBlock); };
            dumpLine(out, block, " in cycle avoiding: "_s, reachableBlocksMatching(isAvoided));
        }
    }

    void printCaughtValues()
    {
        for (BlockID block : reachableBlocks())
            dumpLine(out, block, " index after caught values: "_s, indexAfterCaughtValues(function, function.at(block)));
    }

    void printExecutionCounts()
    {
        for (BlockID block : reachableBlocks())
            dumpLine(out, block, " "_s, nameOf(analyses.executionCountOf(block)));
    }

    void printIncomingValue(ValueID parameter, Edge edge, const IncomingValue& incoming)
    {
        switch (incoming.kind) {
        case IncomingValue::Kind::Argument:
            dumpLine(out, parameter, " from "_s, edge, ": "_s, incoming.argument);
            return;
        case IncomingValue::Kind::EdgeResult:
            printEdgeResult(parameter, edge, edgeResultAt(function, incoming.result));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void printEdgeResult(ValueID parameter, Edge edge, const EdgeResult& result)
    {
        TestReference test { edge.from, result.test, result.caseIndex };
        switch (result.kind) {
        case EdgeResultKind::NarrowedOperand:
            dumpLine(out, parameter, " from "_s, edge, ": narrowed "_s, result.subject, " by "_s, test);
            return;
        case EdgeResultKind::Guard:
            dumpLine(out, parameter, " from "_s, edge, ": guard by "_s, test);
            return;
        case EdgeResultKind::DeclaredResult:
            dumpLine(out, parameter, " from "_s, edge, ": declared result"_s);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void printSummary(ValueID parameter, const OperandSummary& summary)
    {
        dumpLine(out, parameter, " uses: "_s, summary.uses);
        if (summary.retainedIn == RetainedIn::Operand)
            dumpLine(out, parameter, " retained in: "_s, nameOf(summary.retainedIn), " "_s, summary.retainingOperand);
        else
            dumpLine(out, parameter, " retained in: "_s, nameOf(summary.retainedIn));
    }

    void checkSolversAgree(const Liveness& liveness, LivenessOptions livenessOptions)
    {
        livenessOptions.solver = LivenessOptions::Solver::PerValue;
        Liveness perValue(function, analyses.cfg(), analyses.useLists(), livenessOptions);
        for (BlockID block : analyses.cfg().reversePostOrder())
            RELEASE_ASSERT(std::ranges::equal(liveness.liveAtHead(block), perValue.liveAtHead(block)));
    }

    const Liveness& liveness()
    {
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
    }

    const UseEffectHistory& history()
    {
        if (!options.hasHistoryOptions())
            return analyses.escapeHistory();
        if (!historyWithOptions) {
            UseEffectHistoryOptions historyOptions { options.historyUses.value_or(OperandUses()), pointerOrNull(options.ignoredBlocks) };
            historyWithOptions.emplace(analyses, historyOptions);
        }
        return *historyWithOptions;
    }

    Vector<ObjectID> objectsWithOccurrence(Occurrence occurrence, ProgramPoint point)
    {
        return idsMatching<ObjectID>(analyses.escape().numberOfObjects(), [&](ObjectID object) { return history().occurredBefore(object, point) == occurrence; });
    }

    Vector<BlockID> reachableBlocksMatching(NOESCAPE const Invocable<bool(BlockID)> auto& matches)
    {
        return idsMatching<BlockID>(function.blocks.size(), [&](BlockID block) { return analyses.cfg().isReachable(block) && matches(block); });
    }

    Vector<Edge> reachableEdges()
    {
        Vector<Edge> edges;
        for (BlockID from : reachableBlocks())
            forEachOutgoingEdge(function, from, [&](Edge edge, const Target&) { edges.append(edge); });
        return edges;
    }

    Vector<BlockID> reachableBlocks() { return reachableBlocksMatching([](BlockID) { return true; }); }
    bool isListed(ValueID value) { return analyses.useLists().locationOf(value).placement != ValuePlacement::Detached; }
    Vector<ValueID> listedValues() { return idsMatching<ValueID>(function.values.size(), [&](ValueID value) { return isListed(value); }); }

    PrintStream& out;
    const Function& function;
    const AnalysisContext& context;
    FunctionAnalyses& analyses;
    const AnalysisOptions& options;
    std::optional<Liveness> livenessWithOptions { };
    std::optional<UseEffectHistory> historyWithOptions { };
};

enum class SectionRequirement : uint8_t { None, ReachableFromWord, TrackedSuccessors };

struct QuerySection {
    ASCIILiteral name;
    void (SectionPrinter::*print)();
    SectionRequirement requirement { SectionRequirement::None };
};

constexpr QuerySection querySections[] {
    { "common-dominators"_s, &SectionPrinter::printCommonDominators },
    { "value-dominance"_s, &SectionPrinter::printValueDominance },
    { "edge-dominance"_s, &SectionPrinter::printEdgeDominance },
    { "live-points"_s, &SectionPrinter::printLivePoints, SectionRequirement::TrackedSuccessors },
    { "value-identity"_s, &SectionPrinter::printValueIdentity },
    { "incoming-values"_s, &SectionPrinter::printIncomingValues },
    { "object-uses"_s, &SectionPrinter::printObjectUses },
    { "history-points"_s, &SectionPrinter::printHistoryPoints },
    { "aliases"_s, &SectionPrinter::printAliases },
    { "parameter-summaries"_s, &SectionPrinter::printParameterSummaries },
    { "base-objects"_s, &SectionPrinter::printBaseObjects },
    { "handler-edges"_s, &SectionPrinter::printHandlerEdges },
    { "reachability"_s, &SectionPrinter::printReachability, SectionRequirement::ReachableFromWord },
    { "cycles-avoiding-blocks"_s, &SectionPrinter::printCyclesAvoidingBlocks },
    { "caught-values"_s, &SectionPrinter::printCaughtValues },
    { "execution-counts"_s, &SectionPrinter::printExecutionCounts },
};

} // anonymous namespace

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
    options.baseObjectValues = words.parseOptional("values"_s, parseValues);
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
    AnalysisSet cachedSections;
    for (FunctionAnalysis analysis : allAnalyses)
        cachedSections.set(analysis, words.parseFlag(sectionNameOf(analysis)));
    Vector<const QuerySection*> requestedQuerySections;
    for (const QuerySection& section : querySections) {
        if (words.parseFlag(section.name))
            requestedQuerySections.append(&section);
    }
    std::optional<FunctionID> onlyFunction = words.parseOptional("function"_s, functionParser(program));
    if (words.hasFailed())
        return;

    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        if (!program.at(id).hasBody() || (onlyFunction && *onlyFunction != id))
            continue;

        Session session(context, id, preparation.flowOrNull(), preparation.effectsOrNull());
        CommandWords optionWords = words;
        AnalysisOptions options = parseAnalysisOptions(optionWords, session.function);
        for (const QuerySection* section : requestedQuerySections) {
            if (section->requirement == SectionRequirement::ReachableFromWord && !options.reachableFrom)
                optionWords.failMissingArgument();
            if (section->requirement == SectionRequirement::TrackedSuccessors && !hasOnlyTrackedSuccessors(session.analyses.cfg(), options))
                optionWords.fail("tracked-blocks"_s);
        }
        if (!optionWords.finish())
            return;

        context.out.println("function f", id.index());
        SectionPrinter printer { context.out, session.function, session.analysisContext, session.analyses, options };
        for (FunctionAnalysis analysis : cachedSections)
            printer.printCachedSection(analysis);
        for (const QuerySection* section : requestedQuerySections) {
            context.out.println(section->name, ":");
            (printer.*section->print)();
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
