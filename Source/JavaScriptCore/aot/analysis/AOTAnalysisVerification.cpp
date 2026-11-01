/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionAnalyses.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsDeclared.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <initializer_list>
#include <type_traits>
#include <wtf/CommaPrinter.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static void printTerm(PrintStream& out, const Function&, FunctionAnalysis analysis) { out.print(sectionNameOf(analysis)); }
static void printTerm(PrintStream& out, const Function&, ObjectID object) { out.print("object ", object.index()); }

template<typename Number> requires std::is_arithmetic_v<Number>
static void printTerm(PrintStream& out, const Function&, Number number) { out.print(number); }

template<typename Enumeration> requires std::is_enum_v<Enumeration>
static void printTerm(PrintStream& out, const Function&, Enumeration member)
{
    if constexpr (requires { nameOf(member); })
        out.print(nameOf(member));
    else
        out.print("member ", static_cast<unsigned>(member), " of its enumeration");
}

template<typename Object> requires requires(const Object& object, PrintStream& out) { object.dump(out); }
static void printTerm(PrintStream& out, const Function&, const Object& object) { object.dump(out); }

static void printTerm(PrintStream& out, const Function&, BlockID block)
{
    if (block)
        out.print("b", block.index());
    else
        out.print("no block");
}

static void printTerm(PrintStream& out, const Function& function, ValueID value)
{
    if (!value) {
        out.print("no value");
        return;
    }
    out.print("v", value.index());
    if (value.index() < function.values.size())
        out.print(" (", nameOf(function.at(value).opcode), ")");
}

static void printTerm(PrintStream& out, const Function&, const Edge& edge)
{
    if (edge.isHandlerEdge())
        out.print("the edge from b", edge.from.index(), " to its handler");
    else
        out.print("edge ", edge.targetIndex, " out of b", edge.from.index());
}

static void printTerm(PrintStream& out, const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        out.print("operand ", use.operandIndex, " of ");
        printTerm(out, function, use.user);
        out.print(" in b", use.block.index());
        return;
    case Use::Kind::TerminatorOperand:
        out.print("operand ", use.operandIndex, " of the end of b", use.block.index());
        return;
    case Use::Kind::EdgeArgument:
        out.print("argument ", use.operandIndex, " on edge ", use.targetIndex, " out of b", use.block.index());
        return;
    case Use::Kind::HandlerArgument:
        out.print("argument ", use.operandIndex, " to the handler of b", use.block.index());
        return;
    case Use::Kind::BlockSubject:
        out.print("the subject of b", use.block.index());
        return;
    }
}

static void printTerm(PrintStream& out, const Function&, const ValueLocation& location)
{
    switch (location.placement) {
    case ValuePlacement::Detached:
        out.print("no block");
        return;
    case ValuePlacement::Parameter:
        out.print("parameter ", location.index, " of b", location.block.index());
        return;
    case ValuePlacement::InBlock:
        out.print("position ", location.index, " in b", location.block.index());
        return;
    }
}

static void printTerm(PrintStream& out, const Function&, const ObjectSet& objects)
{
    CommaPrinter comma;
    out.print("objects {");
    for (ObjectID object : objects)
        out.print(comma, object.index());
    out.print("}");
}

template<typename Object>
static void printTerm(PrintStream& out, const Function& function, const std::optional<Object>& object)
{
    if (object)
        printTerm(out, function, *object);
    else
        out.print("nothing");
}

template<typename Object>
concept HasWords = requires(PrintStream& out, const Function& function, const Object& object) { printTerm(out, function, object); };

namespace {

// What a sentence names. It is put into words only where a check has found something.
class Term {
public:
    Term() = default;

    template<HasWords Object>
    Term(const Object& object)
        : m_object(&object)
        , m_print([](PrintStream& out, const Function& function, const void* object) { printTerm(out, function, *static_cast<const Object*>(object)); })
    {
    }

    explicit operator bool() const { return m_print; }
    void print(PrintStream& out, const Function& function) const { m_print(out, function, m_object); }

private:
    const void* m_object { nullptr };
    void (*m_print)(PrintStream&, const Function&, const void*) { nullptr };
};

// Compares what the analyses of a function hold with the same answers found in another way. The first difference is reported in words.
// Every check answers whether it held, and its caller looks at nothing behind one that did not.
class Comparison {
public:
    Comparison(const Function& function, const EffectsCache& reporter)
        : m_function(function)
        , m_reporter(reporter)
    {
    }

    bool foundDifference() const { return m_reporter.didReportOutdatedAnswer(); }

    void begin(ASCIILiteral section, ASCIILiteral otherWay = "the same analysis made anew"_s)
    {
        m_section = section;
        m_otherWay = otherWay;
    }
    void begin(FunctionAnalysis analysis) { begin(sectionNameOf(analysis)); }
    void begin(FunctionAnalysis analysis, ASCIILiteral otherWay) { begin(sectionNameOf(analysis), otherWay); }

    // In a text, each {} stands for the next term.
    bool holds(bool condition, ASCIILiteral sentence, Term first = { }, Term second = { })
    {
        if (condition) [[likely]]
            return true;
        report({ sentence, first, second }, Finding::TextIsWholeSentence);
        return false;
    }

    template<typename Answer>
    bool isSame(const Answer& held, const std::type_identity_t<Answer>& found, ASCIILiteral what, Term first = { }, Term second = { })
    {
        if (held == found) [[likely]]
            return true;
        reportDifference({ what, first, second }, held, found);
        return false;
    }

    template<typename Entry>
    bool isSameList(std::span<const Entry> held, std::span<const Entry> found, ASCIILiteral what, Term first = { }, Term second = { })
    {
        if (held.size() != found.size()) [[unlikely]] {
            reportDifference({ what, first, second, Part::Length }, held.size(), found.size());
            return false;
        }
        for (size_t index = 0; index < held.size(); ++index) {
            if (held[index] == found[index]) [[likely]]
                continue;
            reportDifference({ what, first, second, Part::Entry, index }, held[index], found[index]);
            return false;
        }
        return true;
    }

private:
    enum class Part : uint8_t { Whole, Length, Entry };
    enum class Finding : uint8_t { TextIsWholeSentence, AnswersDiffer };

    struct Sentence {
        StringView text;
        Term first;
        Term second;
        Part part { Part::Whole };
        size_t entry { 0 };
    };

    template<typename Answer>
    void reportDifference(const Sentence& sentence, const Answer& held, const Answer& found)
    {
        if constexpr (HasWords<Answer>)
            report(sentence, Finding::AnswersDiffer, held, found);
        else
            report(sentence, Finding::AnswersDiffer);
    }

    NEVER_INLINE void report(const Sentence& sentence, Finding finding, Term held = { }, Term found = { })
    {
        StringPrintStream out;
        out.print(m_section, ": ");
        switch (sentence.part) {
        case Part::Whole:
            break;
        case Part::Length:
            out.print("the length of ");
            break;
        case Part::Entry:
            out.print("entry ", sentence.entry, " of ");
            break;
        }
        StringView text = sentence.text;
        for (const Term& term : { sentence.first, sentence.second }) {
            size_t gap = text.find('{');
            if (gap == notFound || !term)
                break;
            out.print(text.left(gap));
            term.print(out, m_function);
            text = text.substring(gap + 2);
        }
        out.print(text);
        if (held) {
            out.print(": the analyses hold ");
            held.print(out, m_function);
            out.print(", and ", m_otherWay, " gives ");
            found.print(out, m_function);
        } else if (finding == Finding::AnswersDiffer)
            out.print(": the analyses hold something else than ", m_otherWay, " gives");
        m_reporter.reportOutdatedAnswer(out.toString());
    }

    const Function& m_function;
    const EffectsCache& m_reporter;
    ASCIILiteral m_section;
    ASCIILiteral m_otherWay;
};

} // anonymous namespace

static bool checkRequiredAnalyses(Comparison& comparison, AnalysisSet cached)
{
    auto check = [&](FunctionAnalysis analysis, AnalysisSet required) {
        if (!cached.contains(analysis))
            return true;
        comparison.begin(analysis);
        for (FunctionAnalysis input : required) {
            if (!comparison.holds(cached.contains(input), "the analyses hold it and not {}, which it is made from"_s, input))
                return false;
        }
        return true;
    };
    return check(FunctionAnalysis::Dominators, { FunctionAnalysis::ControlFlow })
        && check(FunctionAnalysis::NaturalLoops, { FunctionAnalysis::ControlFlow, FunctionAnalysis::Dominators })
        && check(FunctionAnalysis::Cycles, { FunctionAnalysis::ControlFlow })
        && check(FunctionAnalysis::UseLists, { FunctionAnalysis::ControlFlow })
        && check(FunctionAnalysis::Liveness, { FunctionAnalysis::ControlFlow, FunctionAnalysis::UseLists })
        && check(FunctionAnalysis::EscapeHistory, { FunctionAnalysis::ControlFlow, FunctionAnalysis::Escape })
        && check(FunctionAnalysis::Frequencies, { FunctionAnalysis::ControlFlow, FunctionAnalysis::Cycles })
        && check(FunctionAnalysis::Guards, { FunctionAnalysis::ControlFlow, FunctionAnalysis::UseLists, FunctionAnalysis::Identities })
        && check(FunctionAnalysis::FailedTests, { FunctionAnalysis::ControlFlow })
        && check(FunctionAnalysis::Induction, { FunctionAnalysis::ControlFlow, FunctionAnalysis::Dominators, FunctionAnalysis::NaturalLoops });
}

static bool checkControlFlow(Comparison& comparison, const Function& function, const CFG& graph, const CFG& fresh)
{
    comparison.begin(FunctionAnalysis::ControlFlow);
    if (!comparison.isSame(graph.numNodes(), fresh.numNodes(), "the number of blocks"_s)
        || !comparison.isSameList(graph.reversePostOrder(), fresh.reversePostOrder(), "the reverse post order"_s))
        return false;
    for (unsigned blockIndex = 0; blockIndex < fresh.numNodes(); ++blockIndex) {
        BlockID block(blockIndex);
        if (!comparison.isSame(graph.isReachable(block), fresh.isReachable(block), "whether {} is reachable"_s, block)
            || !comparison.isSame(graph.hasHandler(block), fresh.hasHandler(block), "whether {} has a handler"_s, block)
            || !comparison.isSameList(graph.successors(block), fresh.successors(block), "the successors of {}"_s, block)
            || !comparison.isSameList(graph.predecessors(block), fresh.predecessors(block), "the predecessors of {}"_s, block)
            || !comparison.isSameList(graph.incomingEdges(block), fresh.incomingEdges(block), "the incoming edges of {}"_s, block))
            return false;
    }

    comparison.begin(FunctionAnalysis::ControlFlow, "a walk over the blocks of the function"_s);
    unsigned numberOfBlocks = function.blocks.size();
    if (!comparison.isSame(graph.numNodes(), numberOfBlocks, "the number of blocks"_s))
        return false;

    BitVector isReachable(numberOfBlocks);
    Vector<BlockID> order;
    isReachable.set(0);
    Vector<std::pair<unsigned, uint32_t>> stack { { 0u, numberOfOutgoingEdges(function.blocks[0]) } };
    while (!stack.isEmpty()) {
        auto [block, numberOfUnvisitedEdges] = stack.last();
        if (!numberOfUnvisitedEdges) {
            order.append(BlockID(block));
            stack.removeLast();
            continue;
        }
        uint32_t next = --stack.last().second;
        unsigned target = targetOf(function, outgoingEdgeAt(function, BlockID(block), next)).block.index();
        if (!isReachable.set(target))
            stack.append({ target, numberOfOutgoingEdges(function.blocks[target]) });
    }
    order.reverse();
    if (!comparison.isSameList(graph.reversePostOrder(), order.span(), "the reverse post order"_s))
        return false;

    bool hasCycle = false;
    Vector<Vector<Edge>> incomingEdges(numberOfBlocks);
    for (unsigned blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        BlockID block(blockIndex);
        if (!comparison.isSame(graph.isReachable(block), isReachable.get(blockIndex), "whether {} is reachable"_s, block))
            return false;
        Vector<BlockID> successors;
        if (isReachable.get(blockIndex)) {
            forEachOutgoingEdge(function, block, [&](Edge edge, const Target& target) {
                successors.append(target.block);
                incomingEdges[target.block.index()].append(edge);
                hasCycle |= graph.reversePostOrderIndex(target.block) <= graph.reversePostOrderIndex(block);
            });
        }
        if (!comparison.isSameList(graph.successors(block), successors.span(), "the successors of {}"_s, block)
            || !comparison.isSame(graph.hasHandler(block), isReachable.get(blockIndex) && function.blocks[blockIndex].handler, "whether {} has a handler"_s, block))
            return false;
    }
    if (!comparison.isSame(graph.hasCycle(), hasCycle, "whether the graph has a cycle"_s))
        return false;
    for (unsigned blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        BlockID block(blockIndex);
        if (!comparison.isSameList(graph.incomingEdges(block), incomingEdges[blockIndex].span(), "the incoming edges of {}"_s, block))
            return false;
        Vector<BlockID> predecessors;
        for (Edge edge : incomingEdges[blockIndex]) {
            if (predecessors.isEmpty() || predecessors.last() != edge.from)
                predecessors.append(edge.from);
        }
        if (!comparison.isSameList(graph.predecessors(block), predecessors.span(), "the predecessors of {}"_s, block))
            return false;
    }
    for (uint32_t orderIndex = 0; orderIndex < order.size(); ++orderIndex) {
        if (!comparison.isSame(graph.reversePostOrderIndex(order[orderIndex]), orderIndex, "the position of {} in the reverse post order"_s, order[orderIndex]))
            return false;
    }
    return true;
}

static bool checkDominators(Comparison& comparison, const CFG& graph, const Dominators& cached, const Dominators& fresh, bool selfCheck)
{
    comparison.begin(FunctionAnalysis::Dominators);
    for (BlockID block : graph.reversePostOrder()) {
        if (!comparison.isSame(cached.idom(block), fresh.idom(block), "the immediate dominator of {}"_s, block))
            return false;
    }
    comparison.begin(FunctionAnalysis::Dominators, "WTF::Dominators over the graph made anew"_s);
    for (BlockID block : graph.reversePostOrder()) {
        if (!comparison.isSame(cached.idom(block), fresh.treeForChecking().idom(block), "the immediate dominator of {}"_s, block))
            return false;
    }
    for (unsigned blockIndex = 0; blockIndex < graph.numNodes(); ++blockIndex) {
        BlockID block(blockIndex);
        if (!comparison.isSame(cached.dominates(graph.root(), block), fresh.treeForChecking().dominates(graph.root(), block), "whether the entry dominates {}"_s, block))
            return false;
    }
    if (!selfCheck)
        return true;
    comparison.begin(FunctionAnalysis::Dominators, "the graph in which each block with a handler is split"_s);
    for (BlockID from : graph.reversePostOrder()) {
        if (!graph.hasHandler(from))
            continue;
        for (BlockID to : graph.reversePostOrder()) {
            if (!comparison.isSame(cached.terminatorDominates(from, to), fresh.terminatorDominatesInSplitGraph(from, to), "whether the end of {} dominates {}"_s, from, to))
                return false;
        }
    }
    return true;
}

static BlockID innerMostLoopHeaderOf(const NaturalLoops& loops, BlockID block)
{
    const NaturalLoop* loop = loops.innerMostLoopOf(block);
    return loop ? loop->header() : BlockID();
}

static Vector<BlockID> sortedBlocksOf(const NaturalLoop& loop)
{
    Vector<BlockID> blocks;
    blocks.reserveInitialCapacity(loop.size());
    for (unsigned index = 0; index < loop.size(); ++index)
        blocks.append(loop.at(index));
    AOT::sort(blocks);
    return blocks;
}

static bool checkNaturalLoops(Comparison& comparison, const CFG& graph, const NaturalLoops& cached, const NaturalLoops& fresh)
{
    comparison.begin(FunctionAnalysis::NaturalLoops);
    if (!comparison.isSame(cached.numLoops(), fresh.numLoops(), "the number of loops"_s))
        return false;
    for (BlockID block : graph.reversePostOrder()) {
        if (!comparison.isSame(innerMostLoopHeaderOf(cached, block), innerMostLoopHeaderOf(fresh, block), "the header of the innermost loop around {}"_s, block))
            return false;
    }
    for (unsigned loopIndex = 0; loopIndex < fresh.numLoops(); ++loopIndex) {
        const NaturalLoop& freshLoop = fresh.loop(loopIndex);
        const NaturalLoop* cachedLoop = cached.headerOf(freshLoop.header());
        if (!comparison.holds(cachedLoop, "the analyses hold no loop with the header {}, and the same analysis made anew finds one"_s, freshLoop.header())
            || !comparison.isSameList(sortedBlocksOf(*cachedLoop).span(), sortedBlocksOf(freshLoop).span(), "the sorted blocks of the loop with the header {}"_s, freshLoop.header()))
            return false;
    }
    return true;
}

static bool checkBlockCycles(Comparison& comparison, const CFG& graph, const BlockCycles& cached, const BlockCycles& fresh)
{
    comparison.begin(FunctionAnalysis::Cycles);
    BlockID previous;
    for (BlockID block : graph.reversePostOrder()) {
        if (!comparison.isSame(cached.isInCycle(block), fresh.isInCycle(block), "whether {} is in a cycle"_s, block)
            || !comparison.isSame(cached.depthOf(block), fresh.depthOf(block), "the number of cycles around {}"_s, block)
            || !comparison.isSame(cached.outermostCycleOf(block), fresh.outermostCycleOf(block), "the outermost cycle around {}"_s, block)
            || !comparison.isSame(cached.innermostCycleOf(block), fresh.innermostCycleOf(block), "the innermost cycle around {}"_s, block)
            || (previous && !comparison.isSame(cached.areInSameCycle(previous, block), fresh.areInSameCycle(previous, block), "whether {} and {} are in the same cycle"_s, previous, block)))
            return false;
        previous = block;
    }
    return true;
}

static bool checkUseLists(Comparison& comparison, const Function& function, const CFG& graph, const UseLists& useLists, const UseLists& fresh)
{
    comparison.begin(FunctionAnalysis::UseLists);
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        ValueID value(valueIndex);
        if (!comparison.isSame(useLists.locationOf(value), fresh.locationOf(value), "the place of {}"_s, value)
            || !comparison.isSameList(useLists.usesOf(value), fresh.usesOf(value), "the uses of {}"_s, value))
            return false;
    }

    comparison.begin(FunctionAnalysis::UseLists, "a walk over the blocks of the function"_s);
    Vector<uint32_t> numberOfUsesSeen(FillWith { }, function.values.size(), 0u);
    Vector<ValueLocation> locations(function.values.size());
    auto checkNextUse = [&](ValueID used, const Use& use) {
        if (comparison.foundDifference())
            return;
        std::span<const Use> uses = useLists.usesOf(used);
        uint32_t position = numberOfUsesSeen[used.index()]++;
        std::optional<Use> held = position < uses.size() ? std::optional(uses[position]) : std::nullopt;
        if (comparison.isSame(held, use, "use {} of {}"_s, position, used))
            comparison.holds(usedValueOf(function, use) == used, "usedValueOf() does not find {} at {}, where the walk over the blocks of the function reads it"_s, used, use);
    };
    for (BlockID id : graph.reversePostOrder()) {
        const Block& block = function.at(id);
        auto checkArguments = [&](Use::Kind kind, const Target& target, uint32_t targetIndex) {
            for (uint32_t argumentIndex = 0; argumentIndex < target.arguments.size(); ++argumentIndex) {
                if (ValueID argument = target.arguments[argumentIndex])
                    checkNextUse(argument, Use { kind, id, { }, argumentIndex, targetIndex });
            }
        };
        for (uint32_t index = 0; index < block.parameters.size(); ++index)
            locations[block.parameters[index].index()] = { ValuePlacement::Parameter, id, index };
        for (uint32_t index = 0; index < block.values.size(); ++index)
            locations[block.values[index].index()] = { ValuePlacement::InBlock, id, index };
        if (block.subject)
            checkNextUse(block.subject, Use { Use::Kind::BlockSubject, id, { }, 0, 0 });
        if (block.handler)
            checkArguments(Use::Kind::HandlerArgument, *block.handler, 0);
        for (ValueID user : block.values) {
            function.at(user).forEachOperand([&](ValueID used, uint32_t operandIndex) {
                checkNextUse(used, Use { Use::Kind::Operand, id, user, operandIndex, 0 });
            });
        }
        block.terminator.forEachOperand([&](ValueID used, uint32_t operandIndex) {
            checkNextUse(used, Use { Use::Kind::TerminatorOperand, id, { }, operandIndex, 0 });
        });
        for (uint32_t targetIndex = 0; targetIndex < block.terminator.targets.size(); ++targetIndex)
            checkArguments(Use::Kind::EdgeArgument, block.terminator.targets[targetIndex], targetIndex);
        if (comparison.foundDifference())
            return false;
    }
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        ValueID value(valueIndex);
        if (!comparison.isSame(useLists.useCount(value), numberOfUsesSeen[valueIndex], "the number of uses of {}"_s, value)
            || !comparison.isSame(useLists.locationOf(value), locations[valueIndex], "the place of {}"_s, value))
            return false;
    }
    return true;
}

static bool checkIdentities(Comparison& comparison, const Function& function, const ValueIdentities& cached, const ValueIdentities& fresh)
{
    comparison.begin(FunctionAnalysis::Identities, "the same analysis made anew with every walk begun at the entry"_s);
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        ValueID value(valueIndex);
        if (!comparison.isSame(cached.identityOf(value), fresh.identityOf(value), "the identity of {}"_s, value))
            return false;
    }
    return true;
}

static bool checkIdentitiesOnDemand(Comparison& comparison, const Function& function, const CFG& graph, const ValueIdentities& fresh)
{
    comparison.begin(FunctionAnalysis::Identities, "the complete analysis made anew"_s);
    ValueIdentities onDemand(function, graph, ValueIdentities::RepeatedWalks::StartAtFirstChangedBlock, nullptr, ValueIdentities::Evaluation::OnDemand);
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        ValueID value(valueIndex);
        ValueID identity = onDemand.tryResolve(value);
        if (identity && !comparison.isSame(identity, fresh.identityOf(value), "the identity of {} where it alone is asked for"_s, value))
            return false;
    }
    return true;
}

static bool checkLiveness(Comparison& comparison, const Function& function, const CFG& graph, const UseLists& useLists, const Liveness& cached)
{
    comparison.begin(FunctionAnalysis::Liveness, "the solver that takes one value at a time"_s);
    LivenessOptions options;
    options.solver = LivenessOptions::Solver::PerValue;
    Liveness fresh(function, graph, useLists, options);
    for (BlockID block : graph.reversePostOrder()) {
        if (!comparison.isSameList<ValueID>(cached.liveAtHead(block), fresh.liveAtHead(block), "the values live at the head of {}"_s, block))
            return false;
    }
    return true;
}

static bool checkEffectFacts(Comparison& comparison, ValueID value, const OpcodeEffectFacts& facts, Opcode opcode, const Effects& fresh)
{
    return comparison.holds(EffectTraits(fresh).containsAll(facts.traits.atLeast) && facts.traits.atMost.containsAll(EffectTraits(fresh)), "the effects of {}, {}, lie outside the bounds on traits that are kept for its operation"_s, value, fresh)
        && comparison.holds(!facts.staysWithinDeclaredBound || fresh.isSubsetOf(declaredBoundOf(opcode)), "the effects of {}, {}, exceed the bound declared for its operation, which is said to stay within it"_s, value, fresh)
        && comparison.holds(!facts.mayExitSideways || fresh.mayExitSideways(), "the operation of {} is said to leave sideways wherever it stands, and by its effects, {}, this one does not"_s, value, fresh)
        && comparison.holds(fresh.writes.heaps().containsAll(facts.writtenHeaps), "the effects of {}, {}, leave out a heap that its operation is said to write wherever it stands"_s, value, fresh);
}

static bool checkEffects(Comparison& comparison, const EffectContext& context, const Function& function, const EffectsCache& cached)
{
    comparison.begin(FunctionAnalysis::Effects, "effectsOf()"_s);
    auto checkValue = [&](ValueID id) {
        const EffectsOnEdges* kept = cached.findKept(id);
        if (!kept)
            return true;
        const Value& value = function.at(id);
        EffectsOnEdges fresh = effectsOf(context, function, value);
        return comparison.isSame(*kept, fresh, "the effects of {}"_s, id)
            && checkEffectFacts(comparison, id, effectFactsOf(value), value.opcode, fresh.onAnyEdge())
            && checkEffectFacts(comparison, id, effectFactsOf(value), value.opcode, fresh.normal)
            && checkEffectFacts(comparison, id, effectFactsOf(value.opcode), value.opcode, fresh.onAnyEdge());
    };

    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const Block& block = function.blocks[blockIndex];
        for (ValueID parameter : block.parameters) {
            if (!checkValue(parameter))
                return false;
        }
        for (ValueID value : block.values) {
            if (!checkValue(value))
                return false;
        }
        const EffectsOnEdges* kept = cached.findKeptOfTerminator(BlockID(blockIndex));
        if (kept && !comparison.isSame(*kept, effectsOf(context, function, block.terminator), "the effects of the end of {}"_s, BlockID(blockIndex)))
            return false;
    }
    return true;
}

static bool checkEscapeAnalysis(Comparison& comparison, const Function& function, const EscapeAnalysis& cached, const EscapeAnalysis& fresh)
{
    comparison.begin(FunctionAnalysis::Escape);
    if (!comparison.isSame(cached.gaveUp(), fresh.gaveUp(), "whether the analysis gave up"_s)
        || !comparison.isSame(cached.numberOfObjects(), fresh.numberOfObjects(), "the number of objects"_s))
        return false;
    for (unsigned objectIndex = 0; objectIndex < fresh.numberOfObjects(); ++objectIndex) {
        ObjectID object(objectIndex);
        const TrackedObject& cachedObject = cached.at(object);
        const TrackedObject& freshObject = fresh.at(object);
        ValueID definition = freshObject.definition;
        if (!comparison.isSame(cachedObject.definition, definition, "the definition of {}"_s, object)
            || !comparison.isSame(cachedObject.kind, freshObject.kind, "the kind of {}, {}"_s, object, definition)
            || !comparison.isSame(cachedObject.state, freshObject.state, "whether {}, {}, escapes"_s, object, definition)
            || !comparison.isSame(cachedObject.reasons, freshObject.reasons, "the reasons for which {}, {}, escapes"_s, object, definition)
            || !comparison.isSame(cachedObject.firstEscape, freshObject.firstEscape, "the first escape of {}, {}"_s, object, definition)
            || !comparison.isSame(cachedObject.uses, freshObject.uses, "what is done with {}, {}"_s, object, definition))
            return false;
        if (cached.hasLiveInstances()
            && (!comparison.isSame(cached.liveInstancesOf(object), fresh.liveInstancesOf(object), "the number of live instances of {}, {}"_s, object, definition)
                || !comparison.isSame(cached.objectsRenewedAtMakersOf(object), fresh.objectsRenewedAtMakersOf(object), "the objects renewed where {}, {}, is made"_s, object, definition)))
            return false;
        if (cached.hasLiveInstances() && !comparison.isSame(cached.allocationsThatMayGoOutOfSight().get(object.index()), fresh.allocationsThatMayGoOutOfSight().get(object.index()), "whether {}, {}, may go out of sight"_s, object, definition))
            return false;
        auto listOf = [](ObjectUses uses) {
            return Vector<ObjectUse>(uses.size(), [&](size_t index) {
                return uses[index];
            });
        };
        if (!comparison.isSameList(cached.aliasesOf(object), fresh.aliasesOf(object), "the aliases of {}, {}"_s, object, definition)
            || !comparison.isSameList(listOf(cached.usesOf(object)).span(), listOf(fresh.usesOf(object)).span(), "the uses of {}, {}"_s, object, definition)
            || !comparison.isSameList(cached.contentsOf(object), fresh.contentsOf(object), "the contents of {}, {}"_s, object, definition)
            || !comparison.isSameList(cached.containersOf(object), fresh.containersOf(object), "the containers of {}, {}"_s, object, definition))
            return false;
    }
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        ValueID value(valueIndex);
        PointsToSet cachedSet = cached.pointsTo(value);
        PointsToSet freshSet = fresh.pointsTo(value);
        if (!comparison.isSame(cachedSet.objects, freshSet.objects, "the objects that {} may be"_s, value)
            || !comparison.isSame(cachedSet.mayBeUntracked, freshSet.mayBeUntracked, "whether {} may be an object that is not tracked"_s, value))
            return false;
    }
    return true;
}

static bool checkAllocationsInSight(Comparison& comparison, const Function& function, const EscapeAnalysis& cached, const EscapeAnalysis& fresh)
{
    for (unsigned block = 0; cached.hasLiveInstances() && block < function.blocks.size(); ++block) {
        auto held = cached.allocationsInSightAtEndOf(BlockID(block));
        auto found = fresh.allocationsInSightAtEndOf(BlockID(block));
        if (!comparison.isSame(!!held, !!found, "whether {} is a block of a cycle round an allocation that may go out of sight"_s, BlockID(block))
            || (held && !comparison.isSameList(*held, *found, "the allocations in sight at the end of {}"_s, BlockID(block))))
            return false;
    }
    return true;
}

static bool checkUseEffectHistory(Comparison& comparison, const Function& function, const CFG& graph, unsigned numberOfObjects, const UseEffectHistory& cached, const UseEffectHistory& fresh)
{
    comparison.begin(FunctionAnalysis::EscapeHistory);
    if (!comparison.isSame(cached.gaveUp(), fresh.gaveUp(), "whether the analysis gave up"_s))
        return false;
    if (fresh.gaveUp())
        return true;
    for (BlockID block : graph.reversePostOrder()) {
        ProgramPoint head = ProgramPoint::atHeadOf(block);
        ProgramPoint terminator = pointBeforeTerminatorOf(function, block);
        for (unsigned objectIndex = 0; objectIndex < numberOfObjects; ++objectIndex) {
            ObjectID object(objectIndex);
            if (!comparison.isSame(cached.occurredBefore(object, head), fresh.occurredBefore(object, head), "whether the use of {} has occurred at the head of {}"_s, object, block)
                || !comparison.isSame(cached.occurredBefore(object, terminator), fresh.occurredBefore(object, terminator), "whether the use of {} has occurred at the end of {}"_s, object, block))
                return false;
        }
    }
    return true;
}

static bool checkValuesFromOutside(Comparison& comparison, FunctionAnalyses& fresh)
{
    comparison.begin(FunctionAnalysis::Escape);
    const Function& function = fresh.function();
    const EscapeAnalysis& escape = fresh.escape();
    std::optional<ValueID> freshEntryParameter = findFreshEntryParameter(function, fresh.cfg());
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        ValueID value(valueIndex);
        if (!comesFromOutside(fresh, value))
            continue;
        PointsToSet set = escape.pointsTo(value);
        bool mayBeObjectFromOutside = set.mayBeUntracked;
        for (ObjectID object : set.objects)
            mayBeObjectFromOutside |= escape.at(object).kind == ObjectKind::EntryParameter && escape.at(object).definition != freshEntryParameter;
        if (!comparison.holds(mayBeObjectFromOutside, "comesFromOutside() says that {} comes from outside the function, and the analysis made anew says that it is one of {}, none of which does"_s, value, set.objects))
            return false;
    }
    return true;
}

static bool checkFrequencies(Comparison& comparison, const Function& function, const CFG& graph, const BlockFrequencies& cached, const BlockFrequencies& fresh)
{
    comparison.begin(FunctionAnalysis::Frequencies);
    for (BlockID block : graph.reversePostOrder()) {
        if (!comparison.isSame(cached.frequencyOf(block).predicted(), fresh.frequencyOf(block).predicted(), "the frequency of {}"_s, block)
            || !comparison.isSame(cached.classOf(block).predicted(), fresh.classOf(block).predicted(), "the class of {}"_s, block))
            return false;
        forEachOutgoingEdge(function, block, [&](Edge edge, const Target&) {
            if (!comparison.foundDifference())
                comparison.isSame(cached.classOf(edge).predicted(), fresh.classOf(edge).predicted(), "the class of {}"_s, edge);
        });
        if (comparison.foundDifference())
            return false;
    }
    return true;
}

static bool checkGuards(Comparison& comparison, const Function& function, const CFG& graph, const GuardAvailability& cached, const GuardAvailability& fresh)
{
    comparison.begin(FunctionAnalysis::Guards);
    if (!comparison.isSame(cached.gaveUp(), fresh.gaveUp(), "whether the analysis gave up"_s))
        return false;
    Vector<ValueID> guards;
    for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
        ValueID value(valueIndex);
        const GuardFacts& facts = fresh.factsOf(value);
        if (!comparison.isSame(cached.factsOf(value), facts, "the facts that {} proves"_s, value))
            return false;
        if (facts.isKnown())
            guards.append(value);
    }
    if (fresh.gaveUp())
        return true;
    for (BlockID block : graph.reversePostOrder()) {
        ProgramPoint head = ProgramPoint::atHeadOf(block);
        for (ValueID guard : guards) {
            if (!comparison.isSame(cached.stateAt(guard, head), fresh.stateAt(guard, head), "the state of the guard {} at the head of {}"_s, guard, block))
                return false;
        }
    }
    return true;
}

static bool checkFailedTests(Comparison& comparison, const Function& function, const CFG& graph, const FailedTests& cached, const FailedTests& fresh)
{
    comparison.begin(FunctionAnalysis::FailedTests);
    for (BlockID block : graph.reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (!terminator.isConditionalBranch())
            continue;
        const Value& test = function.at(terminator.conditionOperand());
        if (provesOf(test.opcode) == Proof::None)
            continue;
        for (BlockID successor : graph.successors(block)) {
            if (!comparison.isSame(cached.find(test, successor), fresh.find(test, successor), "the earlier test like {} that is known in {}"_s, terminator.conditionOperand(), successor))
                return false;
        }
    }
    return true;
}

void FunctionAnalyses::verifyUseLists() const
{
    if (m_effects.didReportOutdatedAnswer())
        return;
    Comparison comparison(m_function, m_effects);
    const CFG& graph = *m_controlFlow.cfg;
    checkUseLists(comparison, m_function, graph, *m_values.useLists, UseLists(m_function, graph));
}

void FunctionAnalyses::verifyIdentities(const CFG& graph) const
{
    if (m_effects.didReportOutdatedAnswer())
        return;
    Comparison comparison(m_function, m_effects);
    checkIdentities(comparison, m_function, *m_values.identities, ValueIdentities(m_function, graph, ValueIdentities::RepeatedWalks::StartAtEntry));
}

void FunctionAnalyses::verifyLiveness() const
{
    if (m_effects.didReportOutdatedAnswer())
        return;
    Comparison comparison(m_function, m_effects);
    checkLiveness(comparison, m_function, *m_controlFlow.cfg, *m_values.useLists, *m_values.liveness);
}

void FunctionAnalyses::verifyFrequencies()
{
    if (m_effects.didReportOutdatedAnswer())
        return;
    Comparison comparison(m_function, m_effects);
    CFG& graph = *m_controlFlow.cfg;
    checkFrequencies(comparison, m_function, graph, *m_values.blockFrequencies, BlockFrequencies(m_context, m_function, graph, *m_controlFlow.blockCycles));
}

bool FunctionAnalyses::verifyCached()
{
    // The build ends with that error. What the analyses hold behind it need not be right.
    if (m_effects.didReportOutdatedAnswer())
        return false;

    if (m_values.useLists && !m_values.useLists->isUpToDate() && !m_values.useLists->mustBeMadeAnew())
        useLists();

    AnalysisSet cached = computed();
    if (m_controlFlow.mayDescribeFormerGraph && m_controlFlow.cfg->describes(m_function))
        cached.add(heldControlFlowAnalyses());
    bool wouldKeepFrequencies = m_values.frequenciesMayDescribeFormerFunction && cached.contains(FunctionAnalysis::ControlFlow) && m_values.blockFrequencies->describes(m_context, m_function);
    Comparison comparison(m_function, m_effects);
    comparison.begin(FunctionAnalysis::ControlFlow);
    if (comparison.foundDifference()
        || !comparison.holds(cached.contains(FunctionAnalysis::ControlFlow) || !m_controlFlow.edgesFromUnreachableBlocks, "the analyses hold the edges from unreachable blocks and not the graph, which they are read from"_s)
        || !checkEffects(comparison, m_effectContext, m_function, m_effects))
        return false;
    if (cached.isEmpty())
        return true;
    if (!checkRequiredAnalyses(comparison, cached))
        return false;
    if (cached.contains(FunctionAnalysis::ControlFlow))
        checkBlockCount();
    if (cached.containsAny(valueAnalyses))
        checkValueCount();

    FunctionAnalyses fresh(m_context, m_function);
    CFG& graph = fresh.cfg();

    if (cached.contains(FunctionAnalysis::ControlFlow) && !checkControlFlow(comparison, m_function, *m_controlFlow.cfg, graph))
        return false;
    comparison.begin(FunctionAnalysis::ControlFlow);
    if (m_controlFlow.edgesFromUnreachableBlocks && !comparison.isSameList(m_controlFlow.edgesFromUnreachableBlocks->span(), fresh.edgesFromUnreachableBlocks(), "the edges from unreachable blocks"_s))
        return false;
    if (cached.contains(FunctionAnalysis::Dominators)) {
        bool selfCheck = m_function.blocks.size() <= m_context.build.limit(LimitID::MaxBlocksForDominatorSelfCheck);
        bool comparesWithNaiveSolution = selfCheck && !m_controlFlow.dominators->wasComparedWithNaiveSolution();
        Dominators freshDominators(graph, comparesWithNaiveSolution);
        if (!checkDominators(comparison, graph, *m_controlFlow.dominators, freshDominators, comparesWithNaiveSolution))
            return false;
        if (comparesWithNaiveSolution)
            m_controlFlow.dominators->didCompareWithNaiveSolution();
        if (cached.contains(FunctionAnalysis::NaturalLoops)) {
            NaturalLoops freshNaturalLoops(graph, freshDominators, selfCheck);
            if (!checkNaturalLoops(comparison, graph, *m_controlFlow.naturalLoops, freshNaturalLoops))
                return false;
        }
    }
    if (cached.contains(FunctionAnalysis::Cycles) && !checkBlockCycles(comparison, graph, *m_controlFlow.blockCycles, fresh.blockCycles()))
        return false;

    if (cached.contains(FunctionAnalysis::UseLists) && !checkUseLists(comparison, m_function, graph, *m_values.useLists, fresh.useLists()))
        return false;
    if (cached.contains(FunctionAnalysis::Identities)) {
        ValueIdentities freshIdentities(m_function, graph, ValueIdentities::RepeatedWalks::StartAtEntry);
        m_values.identities->setGraph(&graph);
        bool holdsSameIdentities = checkIdentities(comparison, m_function, *m_values.identities, freshIdentities);
        m_values.identities->setGraph(nullptr);
        if (!holdsSameIdentities || !checkIdentitiesOnDemand(comparison, m_function, graph, freshIdentities))
            return false;
    }
    if (cached.contains(FunctionAnalysis::Liveness) && !checkLiveness(comparison, m_function, graph, fresh.useLists(), *m_values.liveness))
        return false;
    if (cached.contains(FunctionAnalysis::Escape) && (!checkEscapeAnalysis(comparison, m_function, *m_values.escape, fresh.escapeWithLiveInstances()) || !checkAllocationsInSight(comparison, m_function, *m_values.escape, fresh.escapeWithLiveInstances())))
        return false;
    if (cached.contains(FunctionAnalysis::EscapeHistory) && !checkUseEffectHistory(comparison, m_function, graph, fresh.escape().numberOfObjects(), *m_values.escapeHistory, fresh.escapeHistory()))
        return false;
    if (cached.containsAny({ FunctionAnalysis::Escape, FunctionAnalysis::Guards }) && !checkValuesFromOutside(comparison, fresh))
        return false;
    if ((cached.contains(FunctionAnalysis::Frequencies) || wouldKeepFrequencies) && !checkFrequencies(comparison, m_function, graph, *m_values.blockFrequencies, fresh.blockFrequencies()))
        return false;
    if (cached.contains(FunctionAnalysis::Guards) && !checkGuards(comparison, m_function, graph, *m_values.guards, fresh.guards()))
        return false;
    comparison.begin("operations"_s);
    if (m_values.opcodes && m_values.numberOfValuesWithCollectedOpcodes == m_function.values.size() && !comparison.isSame(*m_values.opcodes, fresh.opcodesOfValues(), "the set of the operations of all values"_s))
        return false;
    if (cached.contains(FunctionAnalysis::FailedTests) && !checkFailedTests(comparison, m_function, graph, *m_values.failedTests, fresh.failedTests()))
        return false;
    comparison.begin(FunctionAnalysis::Induction);
    if (cached.contains(FunctionAnalysis::Induction) && !comparison.isSameList(m_values.inductionVariables->loops(), fresh.inductionVariables().loops(), "the loops with their induction variables"_s))
        return false;
    return !fresh.m_effects.didReportOutdatedAnswer();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
