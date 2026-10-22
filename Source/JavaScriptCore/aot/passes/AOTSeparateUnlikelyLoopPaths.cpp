/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCarryVariablesAroundLoops.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTMethodDispatch.h"
#include "AOTRegionCloner.h"
#include "AOTSSARepair.h"
#include "AOTTypePropagation.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// An edge of a hot loop to an unlikely path that runs other code or writes leaves for the same place in a copy of the loop, so the loop keeps the types
// of its likely paths. The copy runs to the end of the turn, tests what goes around the loop for the types the loop has given it, and comes back.
// An unlikely path that only computes stays: it is one more form of the likely path.
// A likely path leaves as well if it may write a variable that every turn reads, and the loop turns without it: what is left carries the variable as a value.
// Such a loop is entered through its copy: the first turn runs as it was written, and the variables are read and tested on the way from the end of a turn into the loop.
// So a loop that ends at its first test, or in its first turn, does what it did before.

namespace {

struct SeparationPlan {
    BlockID header;
    Vector<BlockID> region;
    Vector<Edge> leavingEdges;
    Vector<BlockID> entriesOfCopy;
    Vector<TypeID> typesOfCarriedValues;
    bool letsVariablesBeCarried { false };
};

class LoopPathSeparator {
    WTF_MAKE_NONCOPYABLE(LoopPathSeparator);
public:
    explicit LoopPathSeparator(FunctionPassContext& context)
        : m_context(context)
        , m_program(context.program)
        , m_function(context.function)
        , m_analyses(context.analyses)
    {
    }

    PassOutcome run();

private:
    const Type& typeOf(ValueID value) const LIFETIME_BOUND { return m_program.typeOf(m_function, value); }
    TypePropagationInputs typingInputs() { return { m_program, m_context.id, m_function, m_analyses, m_context.types, m_context.build, &m_context.typingMemo }; }

    void retypeValues();
    void rememberTypes();
    void restoreRememberedTypes();
    std::optional<SeparationPlan> findPlan();
    enum class LoopVariables : uint8_t { StayInMemory, MayBeCarried };
    std::optional<SeparationPlan> planFor(const NaturalLoop&, LoopVariables);
    std::optional<SeparationPlan> planWithCarriedVariables(const NaturalLoop&);
    bool onlyComputes(BlockID);
    BitVector findLeavingBlocks(const NaturalLoop&, const BitVector& isInLoop);
    bool narrowsParameter(const BitVector& isInLoop, const BitVector& leaves);

    void separate(const SeparationPlan&);
    bool addWayBack(IRBuilder&, const SeparationPlan&, BlockID headerOfCopy, std::span<const BlockID> copy);
    void carryVariables(IRBuilder&, BlockID header, BlockID headerOfCopy, std::span<const BlockID> copy);
    Vector<TypeID> findTestsOfEveryTurn(const NaturalLoop&, const BitVector& leavesLoop);
    Vector<TypeID> findTestsOfVariables(const NaturalLoop&, const BitVector& leavesLoop, size_t firstVariable);
    std::optional<Vector<TypeID>> testedTypesOfWayBack(const SeparationPlan&, std::span<const TypeID> testedEveryTurn);
    bool passesTests(const Arguments&, std::span<const TypeID> tested) const;
    void emitTests(IRBuilder&, const Arguments& carried, std::span<const TypeID> tested, BlockID whenPassed, BlockID whenFailed, BlockFrequency whenPassedFrequency);
    void testOnEdges(IRBuilder&, std::span<const Edge>, std::span<const TypeID> tested, BlockID header, BlockID headerOfCopy);

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    Vector<TypeID> m_rememberedTypes;
    BitVector m_isFinishedHeader;
    BitVector m_isSeparatedHeader;
    Vector<VariableOfLoop> m_variablesWithStores;
    bool m_likelyPathCannotLeave { false };
    unsigned m_numberOfSeparatedLoops { 0 };
};

PassOutcome LoopPathSeparator::run()
{
    while (std::optional<SeparationPlan> plan = findPlan()) {
        if (m_context.exceeds(LimitID::MaxSeparatedLoopsPerFunction, m_numberOfSeparatedLoops + 1))
            break;
        separate(*plan);
        ++m_numberOfSeparatedLoops;
    }
    if (!m_numberOfSeparatedLoops)
        return PassOutcome::Unchanged;
    if (!m_variablesWithStores.isEmpty()) {
        simplifyCasts(m_context);
        if (delayStoresOfLoopVariables(m_context, m_variablesWithStores.span())) {
            m_rememberedTypes.clear();
            retypeValues();
        }
    }
    finishTypedValues(typingInputs());
    m_context.build.statistics().count(StatisticID::LoopsSeparatedFromUnlikelyPaths, m_numberOfSeparatedLoops);
    return PassOutcome::Changed;
}

// Cutting the code behind a value of type never renumbers blocks and values, so it waits until the last loop is done.
// Typing starts from never and widens a range that grows around a cycle no further than the type the value had before. What a value had while the copy was half connected
// is no such bound, so each typing starts from the types that held before the separation: a path less never adds a value.
void LoopPathSeparator::retypeValues()
{
    restoreRememberedTypes();
    typeValuesWithoutCutting(typingInputs());
    m_analyses.didChangeValues();
}

// A value that was made since the last call has the type it was made with: a clone that of its original, a parameter what its maker gave it.
void LoopPathSeparator::rememberTypes()
{
    for (size_t index = m_rememberedTypes.size(); index < m_function.values.size(); ++index)
        m_rememberedTypes.append(m_function.values[index].type);
}

void LoopPathSeparator::restoreRememberedTypes()
{
    rememberTypes();
    for (size_t index = 0; index < m_rememberedTypes.size(); ++index) {
        if (m_function.values[index].type != m_rememberedTypes[index])
            m_function.edit(ValueID(index), ValueEdit::Type).type = m_rememberedTypes[index];
    }
}

// Inner loops come first. A loop around a separated loop stays as it is: its copy would hold both copies of the inner loop again.
std::optional<SeparationPlan> LoopPathSeparator::findPlan()
{
    const NaturalLoops& loops = m_analyses.naturalLoops();
    Vector<const NaturalLoop*, 8> candidates;
    for (unsigned index = 0; index < loops.numLoops(); ++index) {
        if (!m_isFinishedHeader.get(loops.loop(index).header().index()))
            candidates.append(&loops.loop(index));
    }
    std::ranges::stable_sort(candidates, { }, &NaturalLoop::size);

    for (const NaturalLoop* loop : candidates) {
        m_isFinishedHeader.set(loop->header().index());
        if (std::optional<SeparationPlan> plan = planWithCarriedVariables(*loop))
            return plan;
    }
    return std::nullopt;
}

// What may run out of memory calls into the run time, which is no computation, though it goes away with an unused result.
static bool onlyComputes(const Effects& effects)
{
    return effects.isUnobservable();
}

bool LoopPathSeparator::onlyComputes(BlockID block)
{
    const Block& contents = m_function.at(block);
    bool valuesOnlyCompute = std::ranges::all_of(contents.values, [&](ValueID value) {
        return Typed::onlyComputes(m_analyses.effectsOf(value).onAnyEdge());
    });
    return valuesOnlyCompute && Typed::onlyComputes(effectsOf(m_analyses.effectContext(), m_function, contents.terminator).onAnyEdge());
}

// An unlikely block leaves with every unlikely block that leads to it or follows it: a path is in one copy from the edge that enters it to the edge that joins the likely path.
BitVector LoopPathSeparator::findLeavingBlocks(const NaturalLoop& loop, const BitVector& isInLoop)
{
    const CFG& cfg = m_analyses.cfg();
    const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
    BlockFrequency frequencyOfLoop = frequencies.classOf(loop.header());

    BitVector isUnlikely(m_function.blocks.size());
    BitVector leaves(m_function.blocks.size());
    Vector<BlockID, 16> worklist;
    for (unsigned index = 0; index < loop.size(); ++index) {
        BlockID block = loop.at(index);
        if (frequencies.classOf(block) <= frequencyOfLoop)
            continue;
        isUnlikely.set(block.index());
        if (!onlyComputes(block) && !leaves.set(block.index()))
            worklist.append(block);
    }

    auto visit = [&](BlockID neighbor) {
        if (isInLoop.get(neighbor.index()) && isUnlikely.get(neighbor.index()) && !leaves.set(neighbor.index()))
            worklist.append(neighbor);
    };
    while (!worklist.isEmpty()) {
        BlockID block = worklist.takeLast();
        for (Edge edge : cfg.incomingEdges(block))
            visit(edge.from);
        for (const Target& target : m_function.at(block).terminator.targets)
            visit(target.block);
    }
    return leaves;
}

// The copy pays where an operation reads a parameter that is wider only for what a leaving path passes. The types that the parameters have without
// those paths are the least ones that hold what the other edges pass. A parameter that is only passed on says nothing: the block through which a
// separated loop is entered has such parameters.
bool LoopPathSeparator::narrowsParameter(const BitVector& isInLoop, const BitVector& leaves)
{
    const CFG& cfg = m_analyses.cfg();
    const UseLists& uses = m_analyses.useLists();
    TypeArena& types = m_context.types;

    Vector<BlockID, 16> blocksWithParameters;
    Vector<std::optional<Type>> typeWithoutLeavingPaths(m_function.values.size());
    for (size_t index : isInLoop) {
        if (leaves.get(index) || m_function.blocks[index].parameters.isEmpty())
            continue;
        blocksWithParameters.append(BlockID(index));
        for (ValueID parameter : m_function.blocks[index].parameters)
            typeWithoutLeavingPaths[parameter.index()] = Type::never();
    }

    auto typeOnStayingEdge = [&](ValueID value) -> const Type& {
        const std::optional<Type>& narrowed = typeWithoutLeavingPaths[value.index()];
        return narrowed ? *narrowed : typeOf(value);
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (BlockID block : blocksWithParameters) {
            const Block& contents = m_function.at(block);
            for (Edge edge : cfg.incomingEdges(block)) {
                if (leaves.get(edge.from.index()))
                    continue;
                const Arguments& arguments = targetOf(m_function, edge).arguments;
                for (size_t position = 0; position < contents.parameters.size(); ++position) {
                    ValueID parameter = contents.parameters[position];
                    // An edge result has the type of its parameter.
                    const Type& incoming = !edge.isHandlerEdge() && position < arguments.size() ? typeOnStayingEdge(arguments[position]) : typeOf(parameter);
                    Type& known = *typeWithoutLeavingPaths[parameter.index()];
                    if (incoming.isSubsetOf(known, m_program))
                        continue;
                    known = known.unionWith(incoming, types);
                    changed = true;
                }
            }
        }
    }

    for (BlockID block : blocksWithParameters) {
        for (ValueID parameter : m_function.at(block).parameters) {
            if (typeOf(parameter).isSubsetOf(*typeWithoutLeavingPaths[parameter.index()], m_program))
                continue;
            bool isReadByOperation = std::ranges::any_of(uses.usesOf(parameter), [](const Use& use) {
                return use.kind != Use::Kind::EdgeArgument;
            });
            if (isReadByOperation)
                return true;
        }
    }
    return false;
}

std::optional<SeparationPlan> LoopPathSeparator::planWithCarriedVariables(const NaturalLoop& loop)
{
    m_likelyPathCannotLeave = false;
    std::optional<SeparationPlan> plan = planFor(loop, LoopVariables::MayBeCarried);
    if (!plan && m_likelyPathCannotLeave)
        return planFor(loop, LoopVariables::StayInMemory);
    return plan;
}

std::optional<SeparationPlan> LoopPathSeparator::planFor(const NaturalLoop& loop, LoopVariables variables)
{
    const CFG& cfg = m_analyses.cfg();
    BlockID header = loop.header();
    if (m_analyses.blockFrequencies().classOf(header) != BlockFrequency::Normal || heatClassOf(m_context, header) != HeatClass::Hot)
        return std::nullopt;
    // The cycle through the copy of a separated loop starts at a block that this pass made, which has no hint.
    bool isLoopOfProgram = std::ranges::any_of(m_function.at(header).values, [&](ValueID value) {
        return m_function.at(value).opcode == Opcode::LoopHint;
    });
    if (!isLoopOfProgram)
        return std::nullopt;

    SeparationPlan plan;
    plan.header = header;
    for (ValueID parameter : m_function.at(header).parameters)
        plan.typesOfCarriedValues.append(m_function.at(parameter).type);
    BitVector isInLoop(m_function.blocks.size());
    uint64_t bytes = 0;
    for (unsigned index = 0; index < loop.size(); ++index) {
        BlockID block = loop.at(index);
        if (m_isSeparatedHeader.get(block.index()))
            return std::nullopt;
        isInLoop.set(block.index());
        plan.region.append(block);
        bytes += estimatedCodeBytes(m_program, m_function, m_function.at(block));
    }

    BitVector leaves = findLeavingBlocks(loop, isInLoop);
    if (variables == LoopVariables::MayBeCarried) {
        BlocksWritingLoopVariables writers = findBlocksWritingLoopVariables(m_context, loop, leaves);
        if (writers.canCarryVariablesWithoutThem) {
            leaves.merge(writers.blocks);
            plan.letsVariablesBeCarried = true;
        }
    }
    if (leaves.isEmpty() || leaves.get(header.index()))
        return std::nullopt;
    bool movesTest = std::ranges::any_of(findTestsOfEveryTurn(loop, leaves), [](TypeID tested) {
        return !!tested;
    });
    if (!movesTest && !plan.letsVariablesBeCarried && !narrowsParameter(isInLoop, leaves))
        return std::nullopt;

    BitVector isEntryOfCopy(m_function.blocks.size());
    for (size_t index : leaves) {
        BlockID block(index);
        for (Edge edge : cfg.incomingEdges(block)) {
            if (edge.isHandlerEdge())
                return std::nullopt;
            if (leaves.get(edge.from.index()))
                continue;
            // The copy is entered here from the loop and from its own blocks. A guard speaks of the operands of one test, so two of them do not join.
            uint32_t numberOfResults = numberOfEdgeResults(m_function, edge.from, edge.targetIndex);
            bool passesGuard = std::ranges::any_of(m_function.at(block).parameters.span().last(numberOfResults), [&](ValueID result) {
                return !typeOf(result).isJavaScriptValue();
            });
            if (passesGuard) {
                m_likelyPathCannotLeave = plan.letsVariablesBeCarried;
                return std::nullopt;
            }
            plan.leavingEdges.append(edge);
            if (!isEntryOfCopy.set(index))
                plan.entriesOfCopy.append(block);
        }
    }
    if (plan.leavingEdges.isEmpty())
        return std::nullopt;
    // The way back enters the copy at its header where a test fails.
    plan.entriesOfCopy.append(header);

    if (m_context.exceeds(LimitID::MaxBytesOfSeparatedLoop, bytes))
        return std::nullopt;
    if (RegionCloner::checkCanDuplicate(m_analyses, plan.region.span(), plan.entriesOfCopy.span()) != CloneCheck::CanClone)
        return std::nullopt;
    return plan;
}

static BlockFrequency frequencyOfCopy(BlockFrequency original)
{
    return original == BlockFrequency::Normal ? BlockFrequency::Unlikely : BlockFrequency::Rare;
}

void LoopPathSeparator::separate(const SeparationPlan& plan)
{
    m_rememberedTypes.clear();
    IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
    RegionCloner cloner(builder, m_function);
    cloner.cloneBlocks(plan.region.span(), [](ValueID, Value&) { });
    rememberTypes();
    // A copy that the loop is entered through runs as often as the loop did.
    for (BlockID block : plan.region) {
        if (!plan.letsVariablesBeCarried)
            m_function.edit(cloner.cloneOf(block), BlockEdit::Rest).frequency = frequencyOfCopy(m_function.at(block).frequency);
    }
    for (Edge edge : plan.leavingEdges) {
        const Target& target = targetOf(m_function, edge);
        builder.redirectEdge(edge, cloner.cloneOf(target.block), Arguments(target.arguments));
    }
    // A path that has left is unreachable in the loop. What it defines is listed before it is emptied: a block behind the loop may name it, and only the copy reaches that block.
    // The repair runs when every edge into the copy is there: until then whatever the loop defines in front of the leaving edges dominates the copy.
    SSARepair repair(builder, m_analyses);
    cloner.addVariablesTo(repair, m_analyses);
    builder.emptyUnreachableBlocks();
    m_analyses.didChangeControlFlow();
    retypeValues();

    m_isSeparatedHeader.set(plan.header.index());
    Vector<BlockID> copy(cloner.clonedBlocks());
    for (BlockID block : copy)
        m_isFinishedHeader.set(block.index());
    if (!addWayBack(builder, plan, cloner.cloneOf(plan.header), copy.span()))
        m_context.build.statistics().count(StatisticID::LoopsWithoutWayBack);
    else if (plan.letsVariablesBeCarried)
        carryVariables(builder, plan.header, cloner.cloneOf(plan.header), copy.span());

    // A parameter that the repair adds holds what the definitions it joins hold.
    restoreRememberedTypes();
    repair.run();
    m_analyses.didChangeControlFlow();
    retypeValues();
}

// A value that goes around the loop and is tested in every turn, with the copy on the failing edge, is tested where it is made instead: the parameter of the
// header then has the tested type, and an edge that passes a value of that type passes no test.
Vector<TypeID> LoopPathSeparator::findTestsOfEveryTurn(const NaturalLoop& loop, const BitVector& leavesLoop)
{
    const CFG& cfg = m_analyses.cfg();
    const NaturalLoops& loops = m_analyses.naturalLoops();
    const Dominators& dominators = m_analyses.dominators();
    const UseLists& uses = m_analyses.useLists();

    Vector<BlockID, 4> latches;
    for (Edge edge : cfg.incomingEdges(loop.header())) {
        if (loops.belongsTo(edge.from, loop))
            latches.append(edge.from);
    }

    const Block& header = m_function.at(loop.header());
    Vector<TypeID> tested(header.parameters.size());
    for (size_t index = 0; index < header.parameters.size(); ++index) {
        for (const Use& use : uses.usesOf(header.parameters[index])) {
            if (use.kind != Use::Kind::Operand || m_function.at(use.user).opcode != Opcode::TypeTest || !loops.belongsTo(use.block, loop))
                continue;
            const Terminator& terminator = m_function.at(use.block).terminator;
            if (terminator.kind != TerminatorKind::Branch || terminator.conditionOperand() != use.user || !leavesLoop.get(terminator.notTaken().block.index()))
                continue;
            bool runsInEveryTurn = std::ranges::all_of(latches, [&](BlockID latch) {
                return dominators.dominates(use.block, latch);
            });
            TypeID testedType = m_function.at(use.user).testedType();
            if (!runsInEveryTurn || typeOf(header.parameters[index]).isSubsetOf(m_context.types.at(testedType), m_program))
                continue;
            tested[index] = testedType;
            break;
        }
    }
    return tested;
}

// The same for the variables that the loop carries, which are its last parameters. They came out of memory with the type of the variable, so there is more to find.
Vector<TypeID> LoopPathSeparator::findTestsOfVariables(const NaturalLoop& loop, const BitVector& leavesLoop, size_t firstVariable)
{
    const CFG& cfg = m_analyses.cfg();
    const NaturalLoops& loops = m_analyses.naturalLoops();
    const Dominators& dominators = m_analyses.dominators();
    const UseLists& uses = m_analyses.useLists();

    Vector<BlockID, 4> latches;
    for (Edge edge : cfg.incomingEdges(loop.header())) {
        if (loops.belongsTo(edge.from, loop) && !leavesLoop.get(edge.from.index()))
            latches.append(edge.from);
    }

    // What a test has proven, or a cast has passed on, is the same value: the narrowest of the types it is tested for in every turn is what the loop needs.
    // The copy makes the same tests, so it is the place to go on in wherever a failing test went: also where undefined or null would have thrown.
    const Block& header = m_function.at(loop.header());
    PropertyAccessContext accessContext { m_program, &m_context.inputs.flow };
    Vector<TypeID> tested(header.parameters.size());
    for (size_t index = firstVariable; index < header.parameters.size(); ++index) {
        Vector<ValueID, 8> sameValues { header.parameters[index] };
        for (size_t position = 0; position < sameValues.size() && sameValues.size() < 16; ++position) {
            for (const Use& use : uses.usesOf(sameValues[position])) {
                if (use.kind != Use::Kind::Operand || !loops.belongsTo(use.block, loop) || leavesLoop.get(use.block.index()))
                    continue;
                const Value& user = m_function.at(use.user);
                if (user.opcode == Opcode::Upcast || user.opcode == Opcode::AssumeType) {
                    sameValues.append(use.user);
                    continue;
                }
                bool runsInEveryTurn = std::ranges::all_of(latches, [&](BlockID latch) {
                    return dominators.dominates(use.block, latch);
                });
                auto isNarrower = [&](TypeID type) {
                    return !tested[index] || m_context.types.at(type).isSubsetOf(m_context.types.at(tested[index]), m_program);
                };
                if (user.opcode == Opcode::RequireObjectCoercible) {
                    // What comes out of it may have a type that no test can establish: proven contents, a range of integers.
                    const Type& given = typeOf(header.parameters[index]);
                    const Type& wanted = m_context.types.at(user.type);
                    Type testable = testableStoredTypeOf(accessContext, given, wanted, m_context.types);
                    if (!runsInEveryTurn || !given.isJavaScriptValue() || testable.isNever() || !given.castTo(testable, m_context.types).isSubsetOf(wanted, m_program))
                        continue;
                    TypeID testedType = m_context.types.intern(testable);
                    if (!isNarrower(testedType))
                        continue;
                    tested[index] = testedType;
                    sameValues.append(use.user);
                    continue;
                }
                if (user.opcode != Opcode::TypeTest || !runsInEveryTurn || !isNarrower(user.testedType()))
                    continue;
                const Terminator& terminator = m_function.at(use.block).terminator;
                if (terminator.kind != TerminatorKind::Branch || terminator.conditionOperand() != use.user)
                    continue;
                BlockID whenFailed = terminator.notTaken().block;
                if (!leavesLoop.get(whenFailed.index()) && loops.belongsTo(whenFailed, loop))
                    continue;
                tested[index] = user.testedType();
                const Target& taken = terminator.taken();
                if (!taken.proofs.isEmpty() && cfg.singleIncomingEdge(taken.block))
                    sameValues.append(m_function.at(taken.block).parameters.last());
            }
        }
        if (tested[index] && typeOf(header.parameters[index]).isSubsetOf(m_context.types.at(tested[index]), m_program))
            tested[index] = TypeID();
    }
    return tested;
}

// A type that a test cannot establish (proven contents, a range of integers) keeps the copy from coming back.
std::optional<Vector<TypeID>> LoopPathSeparator::testedTypesOfWayBack(const SeparationPlan& plan, std::span<const TypeID> testedEveryTurn)
{
    TypeArena& types = m_context.types;
    const Block& header = m_function.at(plan.header);

    PropertyAccessContext accessContext { m_program, &m_context.inputs.flow };
    Vector<TypeID> testedTypes(header.parameters.size());
    for (size_t index = 0; index < header.parameters.size(); ++index) {
        if (testedEveryTurn[index]) {
            testedTypes[index] = testedEveryTurn[index];
            continue;
        }
        const Type& given = types.at(plan.typesOfCarriedValues[index]);
        const Type& wanted = typeOf(header.parameters[index]);
        if (given.isSubsetOf(wanted, m_program))
            continue;
        if (!given.isJavaScriptValue() || !wanted.isJavaScriptValue())
            return std::nullopt;
        Type testable = testableStoredTypeOf(accessContext, given, wanted, types);
        if (testable.isNever() || !given.castTo(testable, types).isSubsetOf(wanted, m_program))
            return std::nullopt;
        testedTypes[index] = types.intern(testable);
    }
    return testedTypes;
}

bool LoopPathSeparator::passesTests(const Arguments& arguments, std::span<const TypeID> tested) const
{
    for (size_t index = 0; index < tested.size(); ++index) {
        if (tested[index] && !typeOf(arguments[index]).isSubsetOf(m_context.types.at(tested[index]), m_program))
            return false;
    }
    return true;
}

// Emits at the end of the builder's block.
void LoopPathSeparator::emitTests(IRBuilder& builder, const Arguments& carried, std::span<const TypeID> tested, BlockID whenPassed, BlockID whenFailed, BlockFrequency whenPassedFrequency)
{
    Arguments narrowed = carried;
    for (size_t index = 0; index < tested.size(); ++index) {
        if (!tested[index] || typeOf(carried[index]).isSubsetOf(m_context.types.at(tested[index]), m_program))
            continue;
        ValueID test = builder.appendTypeTest(carried[index], tested[index]);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, whenPassedFrequency, m_function.at(whenFailed).frequency);
        narrowed[index] = branch.provenOnTaken;
        builder.moveToEndOf(branch.notTaken);
        Arguments argumentsWhenFailed;
        argumentsWhenFailed.append(carried.span().first(m_function.at(whenFailed).parameters.size()));
        builder.emitJump(whenFailed, WTF::move(argumentsWhenFailed));
        builder.moveToEndOf(branch.taken);
    }
    builder.emitJump(whenPassed, WTF::move(narrowed));
}

// The values of the copy have no types yet: they get them with the repair. What goes around the copy has the types that it had in the loop before.
bool LoopPathSeparator::addWayBack(IRBuilder& builder, const SeparationPlan& plan, BlockID headerOfCopy, std::span<const BlockID> copy)
{
    BlockID header = plan.header;
    size_t numberOfParameters = m_function.at(header).parameters.size();
    const NaturalLoop* loop = m_analyses.naturalLoops().headerOf(header);
    if (!loop || m_function.at(headerOfCopy).parameters.size() != numberOfParameters)
        return false;

    BitVector isInCopy(m_function.blocks.size());
    for (BlockID block : copy)
        isInCopy.set(block.index());
    Vector<Edge, 4> latchEdgesOfCopy;
    Vector<Edge, 4> latchEdges;
    Vector<Edge, 4> enteringEdges;
    const CFG& cfg = m_analyses.cfg();
    for (Edge edge : cfg.incomingEdges(headerOfCopy)) {
        if (edge.isHandlerEdge() || targetOf(m_function, edge).arguments.size() != numberOfParameters)
            return false;
        if (isInCopy.get(edge.from.index()))
            latchEdgesOfCopy.append(edge);
    }
    if (latchEdgesOfCopy.isEmpty())
        return false;
    for (Edge edge : cfg.incomingEdges(header)) {
        if (edge.isHandlerEdge() || targetOf(m_function, edge).arguments.size() != numberOfParameters)
            return false;
        (m_analyses.naturalLoops().belongsTo(edge.from, *loop) ? latchEdges : enteringEdges).append(edge);
    }

    Vector<TypeID> testedEveryTurn = findTestsOfEveryTurn(*loop, isInCopy);
    std::optional<Vector<TypeID>> testedTypes = testedTypesOfWayBack(plan, testedEveryTurn.span());
    if (!testedTypes)
        return false;

    builder.setOrigin(m_function.at(header).terminator.origin());
    BlockFrequency frequencyOfLoop = m_function.at(header).frequency;

    // Everything that enters the loop comes through one block, which is the header of the cycle through the copy. So the loop stays a loop of its own, with one entering edge.
    BlockID entry = builder.addBlock(frequencyOfLoop);
    BlockID tests = builder.addBlock(m_function.at(headerOfCopy).frequency);
    Arguments parametersOfEntry;
    Arguments carried;
    for (size_t index = 0; index < numberOfParameters; ++index) {
        TypeID typeInLoop = m_function.at(m_function.at(header).parameters[index]).type;
        if (TypeID tested = testedEveryTurn[index])
            typeInLoop = m_context.types.intern(m_context.types.at(typeInLoop).castTo(m_context.types.at(tested), m_context.types));
        parametersOfEntry.append(builder.addParameter(entry, typeInLoop));
        carried.append(builder.addParameter(tests, plan.typesOfCarriedValues[index]));
    }
    bool entersThroughTests = false;
    for (Edge edge : enteringEdges) {
        Arguments arguments(targetOf(m_function, edge).arguments);
        if (plan.letsVariablesBeCarried) {
            builder.redirectEdge(edge, headerOfCopy, WTF::move(arguments));
            continue;
        }
        bool passes = passesTests(arguments, testedEveryTurn.span());
        entersThroughTests |= !passes;
        builder.redirectEdge(edge, passes ? entry : tests, WTF::move(arguments));
    }
    if (entersThroughTests)
        m_function.edit(tests, BlockEdit::Rest).frequency = frequencyOfLoop;
    for (Edge edge : latchEdgesOfCopy)
        builder.redirectEdge(edge, tests, Arguments(targetOf(m_function, edge).arguments));
    testOnEdges(builder, latchEdges.span(), testedEveryTurn.span(), header, headerOfCopy);

    builder.moveToEndOf(entry);
    builder.emitJump(header, WTF::move(parametersOfEntry));
    builder.moveToEndOf(tests);
    emitTests(builder, carried, testedTypes->span(), entry, headerOfCopy, m_function.at(tests).frequency);
    m_analyses.didChangeControlFlow();

    for (BlockID block : { entry, tests })
        m_isFinishedHeader.set(block.index());
    return true;
}

void LoopPathSeparator::testOnEdges(IRBuilder& builder, std::span<const Edge> edges, std::span<const TypeID> tested, BlockID header, BlockID headerOfCopy)
{
    for (Edge edge : edges) {
        if (passesTests(targetOf(m_function, edge).arguments, tested))
            continue;
        BlockID tests = builder.splitEdge(edge);
        Arguments arguments(m_function.at(tests).terminator.next().arguments);
        m_function.edit(tests, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
        builder.moveToEndOf(tests);
        emitTests(builder, arguments, tested, header, headerOfCopy, m_function.at(tests).frequency);
    }
}

// The loop has one entering edge, from the way back. What is read ahead of the loop is read there, and tested there for what every turn tested it for.
void LoopPathSeparator::carryVariables(IRBuilder& builder, BlockID header, BlockID headerOfCopy, std::span<const BlockID> copy)
{
    if (!carryVariablesAroundSeparatedLoop(m_context, header, m_variablesWithStores))
        return;
    const NaturalLoop* loop = m_analyses.naturalLoops().headerOf(header);
    if (!loop)
        return;
    BitVector isInCopy(m_function.blocks.size());
    for (BlockID block : copy)
        isInCopy.set(block.index());
    Vector<TypeID> tested = findTestsOfVariables(*loop, isInCopy, m_function.at(headerOfCopy).parameters.size());
    Vector<Edge, 4> edges;
    for (Edge edge : m_analyses.cfg().incomingEdges(header)) {
        if (edge.isHandlerEdge() || targetOf(m_function, edge).arguments.size() != tested.size())
            return;
        edges.append(edge);
    }
    builder.setOrigin(m_function.at(header).terminator.origin());
    testOnEdges(builder, edges.span(), tested.span(), header, headerOfCopy);
    m_analyses.didChangeControlFlow();
}

} // anonymous namespace

PassOutcome separateUnlikelyLoopPaths(FunctionPassContext& context)
{
    if (!context.function.hasBody() || !context.analyses.naturalLoops().numLoops())
        return PassOutcome::Unchanged;
    PassOutcome outcome = LoopPathSeparator(context).run();
    outcome |= carryVariablesAroundLoops(context);
    // SimplifyCasts runs inside this pass, and what it says it has kept holds for the function as it left it.
    context.keptAnalyses = { };
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
