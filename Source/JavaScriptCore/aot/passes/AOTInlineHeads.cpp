/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInlineHeads.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeShapes.h"
#include "AOTDecidedBranches.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTInlineCopy.h"
#include "AOTKnownValue.h"
#include "AOTOperatorForms.h"
#include "AOTPass.h"
#include "AOTPhaseTimes.h"
#include "AOTProgramCaches.h"
#include "AOTRegionCloner.h"
#include "AOTThreads.h"
#include "AOTTypePropagation.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

// Most calls that run go to a function that leaves early: it reads a field or two of what it was handed, tests them and returns.

namespace {

struct HeadInputs {
    AnalysisContext contextOf(FunctionID id) const { return { build, program, &flow, &effects, id }; }

    BuildContext& build;
    const Program& program;
    const FlowResults& flow;
    const EffectSummaries& effects;
    const Heat& heat;
};

struct Head {
    FunctionID function;
    BitVector blocks;
    // Only edges that are not expected to be taken lead out of it.
    bool holdsEveryLikelyPath { false };
};

struct CallSite {
    BlockID block;
    uint32_t index;
    ValueID call;
};

struct CallerSites {
    FunctionID caller;
    Vector<CallSite> sites;
};

} // anonymous namespace

static bool isCall(Opcode opcode)
{
    switch (effectRuleOf(opcode)) {
    case EffectRule::Call:
    case EffectRule::Builtin:
        return true;
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
    case EffectRule::Inline:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What throws in the copy throws what the function would have thrown at that place.
static bool onlyReads(const EffectsOnEdges& effects)
{
    const Effects& normal = effects.normal;
    return !normal.mayCollect && !normal.mayRunOtherCode() && !normal.mayBeTerminated && normal.writes.containsOnly(bookkeepingHeaps);
}

static bool canEndBlockOfHead(TerminatorKind kind)
{
    switch (kind) {
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::TestAndBranch:
    case TerminatorKind::Return:
        return true;
    case TerminatorKind::ReturnCall:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

class HeadFinder {
public:
    HeadFinder(const HeadInputs& inputs, FunctionID id)
        : m_build(inputs.build)
        , m_id(id)
        , m_function(inputs.program.at(id))
        , m_analyses(inputs.contextOf(id), m_function)
    {
    }

    std::optional<Head> find()
    {
        BitVector blocks;
        BitVector seen;
        uint64_t operations = 0;
        Vector<BlockID, 8> queue { entryBlock };
        for (unsigned next = 0; next < queue.size(); ++next) {
            BlockID id = queue[next];
            if (seen.set(id.index()) || !onlyReadsAndTests(id))
                continue;
            if (m_build.exceeds(LimitID::MaxSelectedHeadBlocks, blocks.bitCount() + 1, m_analyses.subject()) || m_build.exceeds(LimitID::MaxSelectedHeadOperations, operations + sizeOf(id), m_analyses.subject()))
                continue;
            blocks.set(id.index());
            operations += sizeOf(id);
            const auto& targets = m_function.at(id).terminator.targets;
            for (uint32_t index = 0; index < targets.size(); ++index) {
                if (m_analyses.blockFrequencies().classOf(Edge { id, index }) == BlockFrequency::Normal)
                    queue.append(targets[index].block);
            }
        }

        bool changed;
        do {
            changed = false;
            for (BlockID id : queue) {
                const Terminator& terminator = m_function.at(id).terminator;
                if (!blocks.get(id.index()) || terminator.kind == TerminatorKind::Return)
                    continue;
                if (terminator.targets.containsIf([&](const Target& target) { return blocks.get(target.block.index()); }))
                    continue;
                blocks.clear(id.index());
                changed = true;
            }
        } while (changed);

        // Every block that is left leads to a Return in the head.
        if (!blocks.get(entryBlock.index()))
            return std::nullopt;
        bool holdsEveryLikelyPath = true;
        for (size_t index : blocks) {
            const auto& targets = m_function.at(BlockID(index)).terminator.targets;
            for (uint32_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex)
                holdsEveryLikelyPath &= blocks.get(targets[targetIndex].block.index()) || m_analyses.blockFrequencies().classOf(Edge { BlockID(index), targetIndex }) != BlockFrequency::Normal;
        }
        return Head { m_id, WTF::move(blocks), holdsEveryLikelyPath };
    }

private:
    uint64_t sizeOf(BlockID id) const { return m_function.at(id).values.size() + 1; }

    bool onlyReadsAndTests(BlockID id)
    {
        const Block& block = m_function.at(id);
        if (block.handler || !canEndBlockOfHead(block.terminator.kind) || boundToOf(block.terminator.kind) != BoundTo::Nothing)
            return false;
        for (uint32_t index = 0; index < block.values.size(); ++index) {
            const Value& value = m_function.at(block.values[index]);
            if (boundToOf(value.opcode) != BoundTo::Nothing || isBoundToFrame(m_function, block, index) || isCall(value.opcode))
                return false;
            if (hasOperatorForm(value.opcode) && mayCallRuntime(value.opcode, value.form()))
                return false;
        }
        if (!onlyReads(m_analyses.effectsOfTerminator(id)))
            return false;
        for (ValueID value : block.values) {
            if (!onlyReads(m_analyses.effectsOf(value)))
                return false;
        }
        // Most functions are turned down by their entry block, and the graph of none of those is made.
        return !m_analyses.isInCycle(id);
    }

    BuildContext& m_build;
    FunctionID m_id;
    const Function& m_function;
    FunctionAnalyses m_analyses;
};

} // anonymous namespace

static bool mayBeCopiedFrom(const HeadInputs& inputs, FunctionID id)
{
    const Function& function = inputs.program.at(id);
    return function.hasBody() && function.hasOrdinaryFrame() && function.mayBeCloned() && function.results.size() == 1 && inputs.build.isEnabled(PassID::InlineHeads, id);
}

// A copy buys time where time is spent.
static bool isWorthCopy(const Head& head, HeatClass heat, bool isInCycle)
{
    return isHot(heat) || (head.holdsEveryLikelyPath && isInCycle && !isCold(heat));
}

// Without looking at the blocks: none of them is in a cycle or runs more often than the function is entered, and code that runs that often is not hot.
static bool hasNoBlockWorthCopy(const Heat& heat, FunctionAnalyses& analyses)
{
    const AnalysisContext& context = analyses.context();
    return !isHot(heat.classOf(heat.activationsOf(context.program, context.id) * roundingOfFrequencySums)) && runsNoBlockMoreOftenThanEntry(analyses.cfg());
}

static Vector<CallSite> findSites(const HeadInputs& inputs, std::span<const Head* const> headOf, FunctionID id)
{
    const Program& program = inputs.program;
    const Function& caller = program.at(id);
    Vector<CallSite> sites;
    if (!caller.hasBody() || inputs.flow.runsAtMostOnce(program, id) || !inputs.build.isEnabled(PassID::InlineHeads, id))
        return sites;

    std::optional<FunctionAnalyses> analyses;
    for (uint32_t blockIndex = 0; blockIndex < caller.blocks.size(); ++blockIndex) {
        const Block& block = caller.blocks[blockIndex];
        if (block.frequency != BlockFrequency::Normal || block.handler)
            continue;
        ValueID tailCall = block.terminator.kind == TerminatorKind::ReturnCall ? block.terminator.callOperand() : ValueID();
        for (uint32_t valueIndex = 0; valueIndex < block.values.size(); ++valueIndex) {
            ValueID call = block.values[valueIndex];
            const Value& value = caller.at(call);
            if (value.opcode != Opcode::CallDirect || call == tailCall || value.isAfterInlinedHead() || value.function() == id || !headOf[value.function().index()])
                continue;
            const Function& callee = program.at(value.function());
            if (value.numberOfOperands() != callee.numberOfEntryParameters() || !program.at(callee.result().type).isSubsetOf(program.at(value.type), program))
                continue;
            if (!analyses) {
                analyses.emplace(inputs.contextOf(id), caller);
                if (hasNoBlockWorthCopy(inputs.heat, *analyses))
                    return sites;
            }
            if (!isWorthCopy(*headOf[value.function().index()], heatClassOf(inputs.heat, *analyses, BlockID(blockIndex)), analyses->isInCycle(BlockID(blockIndex))))
                continue;
            sites.append({ BlockID(blockIndex), valueIndex, call });
        }
    }
    return sites;
}

static void copyHeadIn(IRBuilder& builder, IRBuilder::DeferredResultReads& resultReads, const CallSite& site, const Function& callee, const Head& head)
{
    Function& caller = builder.function();
    RELEASE_ASSERT(caller.at(site.block).values[site.index] == site.call);
    Value call = caller.at(site.call);
    BlockFrequency frequencyOfCall = caller.at(site.block).frequency;
    uint32_t firstFrame = appendInlineFrames(caller, InlineFrame {
        .callee = head.function,
        .callPosition = call.position,
        .parent = call.inlineFrame,
        .isTailCall = call.isDemotedTailCall(),
        .isConstruction = call.isConstruction(),
        .module = callee.module,
    }, callee);

    builder.setOrigin(call.origin());
    IRBuilder::Expansion expansion = builder.beginExpansion(ProgramPoint { site.block, site.index }, resultReads);
    RegionCloner cloner(builder, callee);
    const auto& parameters = callee.at(entryBlock).parameters;
    for (uint32_t index = 0; index < parameters.size(); ++index)
        cloner.mapValue(parameters[index], upcastToParameterType(builder, callee.at(parameters[index]), call.operand(index)));

    CFG cfg(callee);
    Vector<BlockID, 8> region;
    BlockID fullCall = builder.addBlock(rarerOf(BlockFrequency::Unlikely, frequencyOfCall));
    for (BlockID block : cfg.reversePostOrder()) {
        if (!head.blocks.get(block.index()))
            continue;
        region.append(block);
        for (const Target& target : callee.at(block).terminator.targets) {
            if (!head.blocks.get(target.block.index()) && !cloner.cloneOf(target.block))
                cloner.mapBlock(target.block, fullCall);
        }
    }
    cloneBlocksIntoInlineFrame(builder, cloner, region.span(), firstFrame);
    builder.emitJump(cloner.cloneOf(entryBlock));

    builder.setOrigin(call.origin());
    for (BlockID clone : cloner.clonedBlocks()) {
        Block& block = caller.edit(clone);
        block.frequency = rarerOf(block.frequency, frequencyOfCall);
        for (Target& target : block.terminator.targets) {
            if (target.block != fullCall)
                continue;
            target.arguments.clear();
            target.proofs.clear();
        }
        if (block.terminator.kind != TerminatorKind::Return)
            continue;
        ValueID returned = std::exchange(block.terminator, Terminator { }).valueOperand();
        builder.moveToEndOf(clone);
        builder.endExpansion(expansion, returned ? returned : builder.appendUndefined());
    }

    builder.moveToEndOf(fullCall);
    call.setIsAfterInlinedHead(true);
    builder.endExpansion(expansion, builder.append(WTF::move(call)));
    builder.setHandler(std::nullopt);
}

// A callee that promises its result answers a test that stands right behind the call.
static bool mayPromiseToPass(const Program& program, const Function& function, BlockID block, ValueID result, const Type& tested)
{
    if (function.at(block).handler || function.at(block).values.isEmpty() || function.at(block).values.last() != result)
        return false;
    const Value& call = function.at(result);
    if (call.opcode != Opcode::CallDirect || !call.isAfterInlinedHead() || call.isConstruction())
        return false;
    const Function& callee = program.at(call.function());
    return callee.results.size() == 1 && callee.results[0].flags.contains(PositionFlags::IsExpected) && program.at(callee.results[0].type).isSubsetOf(tested, program);
}

// A copy often returns what it was handed or has read from a slot, and the caller knows the type of that.
static void skipTestsThatArgumentsDecide(IRBuilder& builder, FunctionAnalyses& analyses, const TypeArena& types)
{
    const Program& program = builder.context().program;
    Function& function = builder.function();
    struct SkippingEdge {
        Edge edge;
        Target target;
    };
    Vector<SkippingEdge, 8> skippingEdges;
    Vector<Edge, 8> edgesFromCalls;
    const CFG& cfg = analyses.cfg();
    const UseLists& useLists = analyses.useLists();
    for (BlockID id : cfg.reversePostOrder()) {
        const Block& block = function.at(id);
        if (block.isHandlerBlock() || block.values.size() != 1 || !block.terminator.isConditionalBranch() || cfg.incomingEdges(id).size() < 2)
            continue;
        const Value& test = function.at(block.values[0]);
        if (test.opcode != Opcode::TypeTest || block.terminator.conditionOperand() != block.values[0] || useLists.useCount(block.values[0]) != 1)
            continue;
        size_t testedIndex = block.parameters.find(test.valueOperand());
        if (testedIndex == notFound)
            continue;
        bool parametersDieInBlock = std::ranges::all_of(block.parameters, [&](ValueID parameter) {
            return std::ranges::all_of(useLists.usesOf(parameter), [&](const Use& use) {
                return use.block == id;
            });
        });
        if (!parametersDieInBlock)
            continue;
        for (Edge edge : cfg.incomingEdges(id)) {
            if (edge.isHandlerEdge() || edge.from == id || function.at(edge.from).terminator.kind != TerminatorKind::Jump)
                continue;
            const Arguments& incomingArguments = targetOf(function, edge).arguments;
            KnownBoolean outcome = evaluateTypeTest(program, program.typeOf(function, incomingArguments[testedIndex]), types.at(test.testedType()));
            if (outcome == KnownBoolean::Unknown) {
                if (mayPromiseToPass(program, function, edge.from, incomingArguments[testedIndex], types.at(test.testedType())))
                    edgesFromCalls.append(edge);
                continue;
            }
            Edge chosenEdge { id, outcome == KnownBoolean::True ? 0u : 1u };
            if (targetOf(function, chosenEdge).block == id || !canFollowWithoutTest(function, chosenEdge))
                continue;
            Target target;
            target.block = targetOf(function, chosenEdge).block;
            for (ValueID argument : argumentsWithoutTest(function, chosenEdge)) {
                size_t index = block.parameters.find(argument);
                target.arguments.append(index == notFound ? argument : incomingArguments[index]);
            }
            if (argumentsFitParameters(program, function, target))
                skippingEdges.append({ edge, WTF::move(target) });
        }
    }
    if (skippingEdges.isEmpty() && edgesFromCalls.isEmpty())
        return;
    for (SkippingEdge& skipping : skippingEdges)
        builder.redirectEdge(skipping.edge, WTF::move(skipping.target));
    for (Edge edge : edgesFromCalls) {
        BlockID mergePoint = targetOf(function, edge).block;
        Arguments incomingArguments = targetOf(function, edge).arguments;
        Terminator branch = function.at(mergePoint).terminator;
        const Value& test = function.at(function.at(mergePoint).values[0]);
        size_t testedIndex = function.at(mergePoint).parameters.find(test.valueOperand());
        Value copy = IRBuilder::make(Opcode::TypeTest, { incomingArguments[testedIndex] });
        copy.setTestedType(test.testedType());
        builder.setOrigin(test.origin());
        function.edit(edge.from, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
        builder.moveToEndOf(edge.from);
        branch.setConditionOperand(builder.append(WTF::move(copy)));
        for (Target& target : branch.targets) {
            for (ValueID& argument : target.arguments) {
                size_t index = function.at(mergePoint).parameters.find(argument);
                if (index != notFound)
                    argument = incomingArguments[index];
            }
        }
        builder.emitTerminator(WTF::move(branch));
    }
    analyses.didChangeControlFlow();
}

void inlineHeads(ProgramEditor& editor, const OptimizationInputs& inputs, BuildContext& build, const ScopedLambda<void(FunctionID)>* readEditedFunction, const DirectCalleesOfFunction* directCalleesOf)
{
    PhaseTimes times(build, PassID::InlineHeads);
    const Program& program = editor.program();
    HeadInputs headInputs { build, program, inputs.flow, inputs.effects, inputs.heat };
    unsigned numberOfFunctions = program.functions.size();

    Vector<Head> heads;
    Vector<CallerSites> callerSites;
    Vector<const Head*> headOf;
    {
        ProgramCaches caches(program, build);
        Vector<unsigned> large = largeFunctions(program, build.parallelLoops());
        heads = collectInParallel<Head>(build.parallelLoops(), numberOfFunctions, large.span(), [&](unsigned index, Vector<Head>& found) {
            if (!mayBeCopiedFrom(headInputs, FunctionID(index)))
                return;
            if (auto head = HeadFinder(headInputs, FunctionID(index)).find())
                found.append(WTF::move(*head));
        });
        times.didFinish("find heads"_s);
        if (heads.isEmpty())
            return;
        headOf.fill(nullptr, numberOfFunctions);
        for (const Head& head : heads)
            headOf[head.function.index()] = &head;

        callerSites = collectInParallel<CallerSites>(build.parallelLoops(), numberOfFunctions, large.span(), [&](unsigned index, Vector<CallerSites>& found) {
            const Vector<FunctionID>* directCallees = directCalleesOf ? (*directCalleesOf)(FunctionID(index)) : nullptr;
            if (directCallees && std::ranges::none_of(*directCallees, [&](FunctionID callee) { return !!headOf[callee.index()]; }))
                return;
            Vector<CallSite> sites = findSites(headInputs, headOf.span(), FunctionID(index));
            if (!sites.isEmpty())
                found.append({ FunctionID(index), WTF::move(sites) });
        });
    }
    times.didFinish("find sites"_s);
    build.statistics().count(StatisticID::SelectedHeadsFound, heads.size());
    if (callerSites.isEmpty())
        return;

    // A caller may be a callee too, and the callers are edited all at once: the heads are copied from what the callees were.
    Vector<std::unique_ptr<Function>> bodyOf(numberOfFunctions);
    BitVector isCopiedFrom(numberOfFunctions);
    Vector<FunctionID> callees;
    Vector<FunctionID> callers;
    uint64_t numberOfSites = 0;
    for (const CallerSites& entry : callerSites) {
        for (const CallSite& site : entry.sites) {
            FunctionID callee = program.at(entry.caller).at(site.call).function();
            if (!isCopiedFrom.quickSet(callee.index()))
                callees.append(callee);
        }
        numberOfSites += entry.sites.size();
        callers.append(entry.caller);
    }
    forEachIndexInParallel(build.parallelLoops(), callees.size(), [&](unsigned index) {
        bodyOf[callees[index].index()] = makeUniqueWithoutFastMallocCheck<Function>(program.at(callees[index]));
    });
    build.statistics().count(StatisticID::SelectedHeadsInlined, numberOfSites);
    times.didFinish("copy callees"_s);

    lowerFunctionsInParallel(editor, build, callers.span(), { }, [&](unsigned index, FunctionID id, Function& caller, TaskArenas& arenas) {
        const Vector<CallSite>& sites = callerSites[index].sites;
        uint32_t firstCopiedValue = static_cast<uint32_t>(caller.values.size());
        Vector<ValueID> changedValues;
        for (size_t site = sites.size(); site--;)
            changedValues.append(sites[site].call);

        FunctionAnalyses analyses(headInputs.contextOf(id), caller);
        {
            IRBuilder builder(headInputs.contextOf(id), caller, arenas.types, &analyses);
            // From the last site to the first: what stands in front of a site stays where it is.
            IRBuilder::DeferredResultReads resultReads(builder, changedValues.span());
            for (size_t site = sites.size(); site--;) {
                FunctionID callee = caller.at(sites[site].call).function();
                copyHeadIn(builder, resultReads, sites[site], *bodyOf[callee.index()], *headOf[callee.index()]);
                analyses.didChangeControlFlow();
            }
            resultReads.finish();
        }

        // An argument often says more than the parameter does, and decides tests of the copy.
        for (uint32_t value = static_cast<uint32_t>(caller.values.size()); value-- > firstCopiedValue;)
            changedValues.append(ValueID(value));
        narrowValueTypes(TypePropagationInputs { program, id, caller, analyses, arenas.types, build }, changedValues.span());
        // Typing gives a value what its declaration expects of it. Behind selection nothing tests for that any more.
        for (Value& value : caller.editAllValues()) {
            if (value.expected())
                value.setExpected(TypeID());
        }
        analyses.didChangeControlFlow();
        IRBuilder builder(headInputs.contextOf(id), caller, arenas.types, &analyses);
        skipTestsThatArgumentsDecide(builder, analyses, arenas.types);
    });
    times.didFinish("copy heads in"_s);
    if (readEditedFunction)
        readFunctionsInParallel(program, build, callers.span(), *readEditedFunction);
    times.didFinish("read callers again"_s);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
