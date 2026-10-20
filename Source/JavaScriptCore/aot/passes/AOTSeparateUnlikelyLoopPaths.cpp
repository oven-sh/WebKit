/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
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

namespace {

struct SeparationPlan {
    BlockID header;
    Vector<BlockID> region;
    Vector<Edge> leavingEdges;
    Vector<BlockID> entriesOfCopy;
    Vector<TypeID> typesOfCarriedValues;
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
    TypePropagationInputs typingInputs() { return { m_program, m_context.id, m_function, m_analyses, m_context.types, m_context.build }; }

    void retypeValues();
    void rememberTypes();
    void restoreRememberedTypes();
    std::optional<SeparationPlan> findPlan();
    std::optional<SeparationPlan> planFor(const NaturalLoop&);
    bool onlyComputes(BlockID);
    BitVector findLeavingBlocks(const NaturalLoop&, const BitVector& isInLoop);
    bool narrowsParameter(const BitVector& isInLoop, const BitVector& leaves);

    void separate(const SeparationPlan&);
    bool addWayBack(IRBuilder&, const SeparationPlan&, BlockID headerOfCopy, std::span<const BlockID> copy);
    Vector<TypeID> findTestsOfEveryTurn(const NaturalLoop&, const BitVector& leavesLoop);
    std::optional<Vector<TypeID>> testedTypesOfWayBack(const SeparationPlan&, std::span<const TypeID> testedEveryTurn);
    bool passesTests(const Arguments&, std::span<const TypeID> tested) const;
    void emitTests(IRBuilder&, const Arguments& carried, std::span<const TypeID> tested, BlockID whenPassed, BlockID whenFailed, BlockFrequency whenPassedFrequency);

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    Vector<TypeID> m_rememberedTypes;
    BitVector m_isFinishedHeader;
    BitVector m_isSeparatedHeader;
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
    for (size_t index = 0; index < m_rememberedTypes.size(); ++index)
        m_function.values[index].type = m_rememberedTypes[index];
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
        if (std::optional<SeparationPlan> plan = planFor(*loop))
            return plan;
    }
    return std::nullopt;
}

bool LoopPathSeparator::onlyComputes(BlockID block)
{
    const Block& contents = m_function.at(block);
    bool valuesOnlyCompute = std::ranges::all_of(contents.values, [&](ValueID value) {
        return m_analyses.effectsOf(value).onAnyEdge().isRemovableWhenUnused();
    });
    return valuesOnlyCompute && effectsOf(m_analyses.effectContext(), m_function, contents.terminator).onAnyEdge().isRemovableWhenUnused();
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

std::optional<SeparationPlan> LoopPathSeparator::planFor(const NaturalLoop& loop)
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
    if (leaves.isEmpty() || leaves.get(header.index()))
        return std::nullopt;
    bool movesTest = std::ranges::any_of(findTestsOfEveryTurn(loop, leaves), [](TypeID tested) {
        return !!tested;
    });
    if (!movesTest && !narrowsParameter(isInLoop, leaves))
        return std::nullopt;

    BitVector isEntryOfCopy(m_function.blocks.size());
    for (size_t index : leaves) {
        BlockID block(index);
        for (Edge edge : cfg.incomingEdges(block)) {
            if (edge.isHandlerEdge())
                return std::nullopt;
            if (leaves.get(edge.from.index()))
                continue;
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
    for (BlockID block : plan.region)
        m_function.at(cloner.cloneOf(block)).frequency = frequencyOfCopy(m_function.at(block).frequency);
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
        builder.emitJump(whenFailed, Arguments(carried));
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
        bool passes = passesTests(arguments, testedEveryTurn.span());
        entersThroughTests |= !passes;
        builder.redirectEdge(edge, passes ? entry : tests, WTF::move(arguments));
    }
    if (entersThroughTests)
        m_function.at(tests).frequency = frequencyOfLoop;
    for (Edge edge : latchEdgesOfCopy)
        builder.redirectEdge(edge, tests, Arguments(targetOf(m_function, edge).arguments));
    for (Edge edge : latchEdges) {
        if (passesTests(targetOf(m_function, edge).arguments, testedEveryTurn.span()))
            continue;
        BlockID latchTests = builder.splitEdge(edge);
        Arguments arguments(m_function.at(latchTests).terminator.next().arguments);
        m_function.at(latchTests).terminator = Terminator { };
        builder.moveToEndOf(latchTests);
        emitTests(builder, arguments, testedEveryTurn.span(), header, headerOfCopy, m_function.at(latchTests).frequency);
    }

    builder.moveToEndOf(entry);
    builder.emitJump(header, WTF::move(parametersOfEntry));
    builder.moveToEndOf(tests);
    emitTests(builder, carried, testedTypes->span(), entry, headerOfCopy, m_function.at(tests).frequency);
    m_analyses.didChangeControlFlow();

    for (BlockID block : { entry, tests })
        m_isFinishedHeader.set(block.index());
    return true;
}

} // anonymous namespace

PassOutcome separateUnlikelyLoopPaths(FunctionPassContext& context)
{
    if (!context.function.hasBody() || !context.analyses.naturalLoops().numLoops())
        return PassOutcome::Unchanged;
    return LoopPathSeparator(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
