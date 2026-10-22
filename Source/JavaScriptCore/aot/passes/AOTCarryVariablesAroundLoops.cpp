/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCarryVariablesAroundLoops.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTDecidedBranches.h"
#include "AOTEffects.h"
#include "AOTEffectsDeclared.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTMemoryLocation.h"
#include "AOTNaturalLoops.h"
#include "AOTRegionCloner.h"
#include "AOTSSARepair.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// A loop that reads a variable of an enclosing function or of a module in every turn reads what it stored or read in the turn before.
// Its reads take that value instead, which goes around the loop as a parameter of the header: the first is read ahead of the loop, and a store defines the next.
// The stores stay at first, so the variable holds its value wherever other code looks at it: behind the loop, in a handler, in a callee.
//
// Nothing else may write the variable on the paths that carry it. SeparateUnlikelyLoopPaths names the paths that are about to leave the loop for its copy:
// they read the variable as before, and where one of them comes back to the others the variable is read again. That pass then tests what goes around the loop
// where it is made. A loop that it leaves alone, because no path has to leave, gets the tests that a turn begins with copied ahead of it: what the loop
// itself passes on has the tested type, so only what enters is tested, and a failing test leaves for the same place as before.
//
// When the loop has its final form and types, and nothing in it can look at the variable or throw, the stores wait for the edges that leave the loop.

namespace {

constexpr AbstractHeapSet carriedHeaps { AbstractHeap::ModuleVariables, AbstractHeap::EnvironmentFields };
constexpr unsigned maxCarriedVariablesPerLoop = 8;
constexpr unsigned maxRoundsPerStep = 4;

struct VariableAccess {
    MemoryLocation location;
    ValueID base;
    ValueID id;
    ValueID stored;
    ProgramPoint point;
};

struct OtherWrite {
    BlockID block;
    LocationMasks writes;
};

struct CarriedVariable {
    MemoryLocation location;
    ValueID base;
    Vector<ValueID, 4> reads;
    Vector<VariableAccess, 2> stores;
    Vector<BlockID, 2> blocksOfOtherWrites;
    Vector<ValueID, 4> definitions;
    bool isReadInEveryTurn { false };
    bool isWrittenOnLeavingPath { false };
};

class VariableCarrier {
    WTF_MAKE_NONCOPYABLE(VariableCarrier);
public:
    explicit VariableCarrier(FunctionPassContext& context)
        : m_context(context)
        , m_program(context.program)
        , m_function(context.function)
        , m_analyses(context.analyses)
    {
        if (context.build.validates())
            m_keptReads = findReadsKeptForValidation(m_function);
    }

    PassOutcome run();
    void setFirstTurnRunsInCopy() { m_firstTurnRunsHere = false; }
    BlocksWritingLoopVariables findBlocksWritingVariables(const NaturalLoop&, const BitVector& leaves);
    bool carryAround(BlockID header, BitVector& leaves);
    bool delayStores(std::span<const VariableOfLoop>);
    Vector<VariableOfLoop> takeVariablesWithStores() { return std::exchange(m_variablesWithStores, { }); }

private:
    struct LeadingTest {
        BlockID block;
        std::optional<size_t> parameterOfHeader;
    };

    const Type& typeOf(ValueID value) const LIFETIME_BOUND { return m_program.typeOf(m_function, value); }
    bool isSubtype(ValueID value, TypeID type) const { return typeOf(value).isSubsetOf(m_program.at(type), m_program); }

    bool retype();
    Vector<BlockID, 8> headersFromInnermost();
    void findVariables(const NaturalLoop&, const BitVector* leaves);
    bool hasTypesOfReads(const CarriedVariable&) const;
    bool holdsOneValueForEveryRead(const CarriedVariable&);
    bool hasCycleWithout(const NaturalLoop&, const BitVector& leaves) const;

    bool carryOnce(const NaturalLoop&, BitVector& leaves);
    bool isReadBy(const CarriedVariable&, Opcode);
    void rewrite(const NaturalLoop&, Edge entry, std::span<const Edge> edgesBack, BitVector& leaves);
    bool hoistCaptures(const NaturalLoop&);

    Vector<LeadingTest, 4> findLeadingTests(const NaturalLoop&);
    bool isPassedOnWithType(const NaturalLoop&, size_t parameterIndex, TypeID tested);
    bool moveLeadingTestsAheadOf(const NaturalLoop&);

    bool delayStoresIn(const NaturalLoop&, std::span<const VariableOfLoop>);

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    BitVector m_stays;
    BitVector m_keptReads;
    Vector<VariableAccess, 16> m_accesses;
    Vector<OtherWrite, 8> m_otherWrites;
    LocationMasks m_writesOnLeavingPaths;
    Vector<BlockID, 8> m_leavingBlocksThatWrite;
    Vector<CarriedVariable, 8> m_variables;
    BitVector m_isHeaderOfChangedLoop;
    Vector<VariableOfLoop> m_variablesWithStores;
    bool m_firstTurnRunsHere { true };
};

// False: blocks and values have other numbers than before.
bool VariableCarrier::retype()
{
    simplifyCasts(m_context);
    if (typeChangedValues(TypePropagationInputs { m_program, m_context.id, m_function, m_analyses, m_context.types, m_context.build, &m_context.typingMemo }).cutBlocks)
        return false;
    simplifyCasts(m_context);
    return true;
}

// What is read ahead of an inner loop is a read of the loop around it.
Vector<BlockID, 8> VariableCarrier::headersFromInnermost()
{
    const NaturalLoops& loops = m_analyses.naturalLoops();
    Vector<const NaturalLoop*, 8> sorted;
    for (unsigned index = 0; index < loops.numLoops(); ++index)
        sorted.append(&loops.loop(index));
    std::ranges::stable_sort(sorted, { }, &NaturalLoop::size);
    Vector<BlockID, 8> headers;
    for (const NaturalLoop* loop : sorted)
        headers.append(loop->header());
    return headers;
}

PassOutcome VariableCarrier::run()
{
    for (BlockID header : headersFromInnermost()) {
        BitVector leaves;
        if (carryAround(header, leaves))
            m_isHeaderOfChangedLoop.set(header.index());
    }
    if (m_isHeaderOfChangedLoop.isEmpty())
        return PassOutcome::Unchanged;

    for (unsigned round = 0; round < maxRoundsPerStep && retype(); ++round) {
        bool moved = false;
        for (BlockID header : headersFromInnermost()) {
            const NaturalLoop* loop = m_analyses.naturalLoops().headerOf(header);
            if (loop && m_isHeaderOfChangedLoop.get(header.index()))
                moved |= moveLeadingTestsAheadOf(*loop);
        }
        if (!moved) {
            if (delayStores(m_variablesWithStores.span()))
                retype();
            break;
        }
    }
    return PassOutcome::Changed;
}

static LocationMasks writesOf(const EffectsOnEdges& effects)
{
    Effects any = effects.onAnyEdge();
    return (any.mayRunOtherCode() ? LocationMasks::top() : any.writes).restrictedTo(carriedHeaps);
}

// Two environments of one layout may be one object: a store through another one counts as a write by something else.
void VariableCarrier::findVariables(const NaturalLoop& loop, const BitVector* leaves)
{
    m_stays = BitVector(m_function.blocks.size());
    m_accesses.shrink(0);
    m_otherWrites.shrink(0);
    m_writesOnLeavingPaths = { };
    m_leavingBlocksThatWrite.shrink(0);
    m_variables.shrink(0);

    for (unsigned position = 0; position < loop.size(); ++position) {
        BlockID block = loop.at(position);
        bool stays = !leaves || !leaves->get(block.index());
        if (stays)
            m_stays.set(block.index());
        LocationMasks otherWrites;
        const BlockValues& values = m_function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            ValueID id = values[index];
            MemoryAccess access = memoryAccessOf(m_function, m_function.at(id));
            if (stays && access && carriedHeaps.contains(access.location.heap()) && !access.location.isEntireHeap()) {
                ValueID base = access.base ? m_analyses.sameObjectThroughCasts(access.base) : ValueID();
                m_accesses.append(VariableAccess { access.location, base, id, access.stored, { block, index } });
                continue;
            }
            otherWrites.merge(writesOf(m_analyses.effectsOf(id)));
        }
        otherWrites.merge(writesOf(m_analyses.effectsOfTerminator(block)));
        if (otherWrites.isEmpty())
            continue;
        if (stays)
            m_otherWrites.append({ block, otherWrites });
        else {
            m_writesOnLeavingPaths.merge(otherWrites);
            m_leavingBlocksThatWrite.append(block);
        }
    }

    const UseLists& useLists = m_analyses.useLists();
    const Dominators& dominators = m_analyses.dominators();
    const NaturalLoops& loops = m_analyses.naturalLoops();
    Vector<BlockID, 4> latches;
    for (Edge edge : m_analyses.cfg().incomingEdges(loop.header())) {
        if (m_stays.get(edge.from.index()))
            latches.append(edge.from);
    }
    if (latches.isEmpty())
        return;
    auto runsInEveryTurn = [&](BlockID block) {
        return std::ranges::all_of(latches, [&](BlockID latch) {
            return dominators.dominates(block, latch);
        });
    };
    // The read ahead of the loop stands for the read of the first turn, so the first turn has to get as far: a loop may well end at its first test.
    BitVector runsInFirstTurn(m_function.blocks.size());
    for (BlockID block = loop.header(); m_firstTurnRunsHere && !runsInFirstTurn.set(block.index());) {
        BlockID next;
        bool mayEndHere = false;
        for (const Target& target : m_function.at(block).terminator.targets) {
            if (m_stays.get(target.block.index())) {
                mayEndHere |= !!next;
                next = target.block;
            } else if (!loops.belongsTo(target.block, loop))
                mayEndHere |= m_function.at(target.block).frequency <= m_function.at(loop.header()).frequency;
        }
        if (mayEndHere || !next)
            break;
        block = next;
    }

    for (const VariableAccess& access : m_accesses) {
        if (access.stored || m_keptReads.get(access.id.index()) || !typeOf(access.id).isJavaScriptValue())
            continue;
        if (access.base && loops.belongsTo(useLists.locationOf(access.base).block, loop))
            continue;
        CarriedVariable* variable = nullptr;
        for (CarriedVariable& candidate : m_variables) {
            if (candidate.location == access.location && candidate.base == access.base)
                variable = &candidate;
        }
        if (!variable) {
            m_variables.append(CarriedVariable { access.location, access.base, { }, { }, { }, { } });
            variable = &m_variables.last();
        }
        variable->reads.append(access.id);
        variable->isReadInEveryTurn |= (!m_firstTurnRunsHere || runsInFirstTurn.get(access.point.block.index())) && runsInEveryTurn(access.point.block);
    }

    for (CarriedVariable& variable : m_variables) {
        if (holdsOneValueForEveryRead(variable))
            continue;
        for (const VariableAccess& access : m_accesses) {
            if (!access.stored || overlapOf(m_program, access.location, variable.location) == LocationOverlap::None)
                continue;
            if (access.location == variable.location && access.base == variable.base)
                variable.stores.append(access);
            else
                variable.blocksOfOtherWrites.append(access.point.block);
        }
        for (const OtherWrite& write : m_otherWrites) {
            if (write.writes.overlaps(m_program, variable.location))
                variable.blocksOfOtherWrites.append(write.block);
        }
        variable.isWrittenOnLeavingPath = m_writesOnLeavingPaths.overlaps(m_program, variable.location);
    }
    // A loop inside this one is where the time goes: without it this loop would turn for nothing.
    auto hasToStay = [&](BlockID block) {
        return runsInEveryTurn(block) || loops.innerMostLoopOf(block) != &loop;
    };
    // HoistLoopInvariantReads moves a read that is all there is to a variable.
    m_variables.removeAllMatching([&](const CarriedVariable& variable) {
        if (!variable.isReadInEveryTurn || !hasTypesOfReads(variable) || std::ranges::any_of(variable.blocksOfOtherWrites, hasToStay))
            return true;
        return variable.stores.isEmpty() && !variable.isWrittenOnLeavingPath && !isReadBy(variable, Opcode::TypeTest) && !isReadBy(variable, Opcode::GetCapture);
    });
}

// The closure of a declared function, for one: nobody stores to it while this function runs, or it is stored to once, ahead of every read.
// In the second case the read ahead of the loop must not come to stand ahead of that store: nothing that may write runs in a turn before the variable is read.
bool VariableCarrier::holdsOneValueForEveryRead(const CarriedVariable& variable)
{
    const Value& read = m_function.at(variable.reads.first());
    switch (stabilityOf(m_program, read, memoryAccessOf(m_function, read))) {
    case LocationStability::Unknown:
        return false;
    case LocationStability::UnchangedDuringActivation:
        return true;
    case LocationStability::ChangedByOwnStores:
        break;
    }
    if (m_program.at(read.type).primitives & Primitive::Absent)
        return false;
    const Dominators& dominators = m_analyses.dominators();
    const UseLists& useLists = m_analyses.useLists();
    return std::ranges::any_of(variable.reads, [&](ValueID candidate) {
        BlockID block = useLists.locationOf(candidate).block;
        auto comesBehindRead = [&](BlockID writer) {
            return writer != block && dominators.dominates(block, writer);
        };
        return std::ranges::all_of(m_otherWrites, comesBehindRead, &OtherWrite::block) && std::ranges::all_of(m_leavingBlocksThatWrite, comesBehindRead);
    });
}

bool VariableCarrier::hasTypesOfReads(const CarriedVariable& variable) const
{
    TypeID typeOfRead = m_function.at(variable.reads.first()).type;
    bool readsAgree = std::ranges::all_of(variable.reads, [&](ValueID read) {
        return m_function.at(read).type == typeOfRead;
    });
    return readsAgree && std::ranges::all_of(variable.stores, [&](const VariableAccess& store) {
        return isSubtype(store.stored, typeOfRead);
    });
}

bool VariableCarrier::hasCycleWithout(const NaturalLoop& loop, const BitVector& leaves) const
{
    BitVector isSeen(m_function.blocks.size());
    Vector<BlockID, 16> worklist { loop.header() };
    isSeen.set(loop.header().index());
    while (!worklist.isEmpty()) {
        BlockID block = worklist.takeLast();
        for (const Target& target : m_function.at(block).terminator.targets) {
            if (target.block == loop.header())
                return true;
            if (m_stays.get(target.block.index()) && !leaves.get(target.block.index()) && !isSeen.set(target.block.index()))
                worklist.append(target.block);
        }
    }
    return false;
}

BlocksWritingLoopVariables VariableCarrier::findBlocksWritingVariables(const NaturalLoop& loop, const BitVector& leaves)
{
    findVariables(loop, &leaves);

    BlocksWritingLoopVariables result;
    result.blocks = BitVector(m_function.blocks.size());
    for (const CarriedVariable& variable : m_variables) {
        for (BlockID block : variable.blocksOfOtherWrites)
            result.blocks.set(block.index());
    }
    result.canCarryVariablesWithoutThem = !m_variables.isEmpty() && hasCycleWithout(loop, result.blocks);
    return result;
}

// The environment of an inlined function comes to stand ahead of the loop once its closure is read there, and with it the variables in it can be carried.
bool VariableCarrier::carryAround(BlockID header, BitVector& leaves)
{
    bool changed = false;
    for (unsigned round = 0; round < maxRoundsPerStep; ++round) {
        const NaturalLoop* loop = m_analyses.naturalLoops().headerOf(header);
        if (!loop || !carryOnce(*loop, leaves))
            break;
        changed = true;
        loop = m_analyses.naturalLoops().headerOf(header);
        if (!loop || !hoistCaptures(*loop))
            break;
    }
    return changed;
}

bool VariableCarrier::carryOnce(const NaturalLoop& loop, BitVector& leaves)
{
    BlockID header = loop.header();
    if (m_function.at(header).isHandlerBlock() || m_function.at(header).frequency != BlockFrequency::Normal || m_analyses.isEnteredFromUnreachableBlock(header))
        return false;
    // The cycle through the copy of a separated loop starts at a block without a hint.
    bool isLoopOfProgram = std::ranges::any_of(m_function.at(header).values, [&](ValueID value) {
        return m_function.at(value).opcode == Opcode::LoopHint;
    });
    std::optional<Edge> entry = m_analyses.naturalLoops().findSingleEntry(loop);
    if (!isLoopOfProgram || !entry || hasEdgeResults(m_function, *entry))
        return false;

    const CFG& cfg = m_analyses.cfg();
    Vector<Edge, 4> edgesBack;
    for (unsigned position = 0; position < loop.size(); ++position) {
        BlockID block = loop.at(position);
        if (leaves.get(block.index()))
            continue;
        for (Edge edge : cfg.incomingEdges(block)) {
            bool comesBack = leaves.get(edge.from.index());
            if ((comesBack || block == header) && (edge.isHandlerEdge() || (block == header && hasEdgeResults(m_function, edge))))
                return false;
            if (comesBack)
                edgesBack.append(edge);
        }
    }

    findVariables(loop, &leaves);
    m_variables.removeAllMatching([](const CarriedVariable& variable) {
        return !variable.blocksOfOtherWrites.isEmpty();
    });
    if (m_variables.isEmpty())
        return false;
    if (m_variables.size() > maxCarriedVariablesPerLoop) {
        std::ranges::stable_sort(m_variables, std::ranges::greater { }, [](const CarriedVariable& variable) {
            return variable.reads.size();
        });
        m_variables.shrink(maxCarriedVariablesPerLoop);
    }
    rewrite(loop, *entry, edgesBack.span(), leaves);
    return true;
}

bool VariableCarrier::isReadBy(const CarriedVariable& variable, Opcode opcode)
{
    const UseLists& uses = m_analyses.useLists();
    return std::ranges::any_of(variable.reads, [&](ValueID read) {
        return std::ranges::any_of(uses.usesOf(read), [&](const Use& use) {
            return use.kind == Use::Kind::Operand && m_function.at(use.user).opcode == opcode;
        });
    });
}

// A variable that the loop only reads, and tests, goes around it as well: so it is tested where it enters.
void VariableCarrier::rewrite(const NaturalLoop& loop, Edge entry, std::span<const Edge> edgesBack, BitVector& leaves)
{
    BlockID header = loop.header();
    Vector<bool, 8> goesAround;
    for (const CarriedVariable& variable : m_variables)
        goesAround.append(!variable.stores.isEmpty() || variable.isWrittenOnLeavingPath || isReadBy(variable, Opcode::TypeTest));
    Vector<Edge, 4> latchEdges;
    for (Edge edge : m_analyses.cfg().incomingEdges(header)) {
        if (edge != entry)
            latchEdges.append(edge);
    }

    IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
    BlockID preheader = entry.from;
    if (m_function.at(preheader).terminator.kind != TerminatorKind::Jump) {
        preheader = builder.splitEdge(entry);
        entry = Edge { preheader, 0 };
    }
    bool readsAgain = std::ranges::any_of(m_variables, &CarriedVariable::isWrittenOnLeavingPath);
    Vector<BlockID, 4> blocksBack;
    if (readsAgain) {
        for (Edge edge : edgesBack) {
            BlockID block = builder.splitEdge(edge);
            blocksBack.append(block);
            leaves.set(block.index());
            for (Edge& latchEdge : latchEdges) {
                if (latchEdge == edge)
                    latchEdge = Edge { block, 0 };
            }
        }
    }

    for (size_t index = 0; index < m_variables.size(); ++index) {
        CarriedVariable& variable = m_variables[index];
        auto insertRead = [&](ProgramPoint point) {
            Value read = m_function.at(variable.reads.first());
            builder.setOrigin(read.origin());
            return builder.insertBefore(point, WTF::move(read));
        };
        ValueID first = insertRead(pointBeforeTerminatorOf(m_function, preheader));
        if (!goesAround[index]) {
            variable.definitions.append(first);
            continue;
        }
        builder.setOrigin(m_function.at(first).origin());
        ValueID parameter = builder.addParameter(header, m_function.at(first).type);
        m_function.edit(parameter).setExpected(m_function.at(first).expected());
        builder.appendArgument(entry, first);
        for (Edge edge : latchEdges)
            builder.appendArgument(edge, parameter);
        variable.definitions.append(parameter);
        if (!variable.stores.isEmpty())
            m_variablesWithStores.append({ header, parameter, variable.location, variable.base });
        for (const VariableAccess& store : variable.stores) {
            Value copy = IRBuilder::make(Opcode::Upcast, { store.stored });
            copy.setCastType(m_function.at(store.stored).type);
            builder.setOrigin(m_function.at(store.id).origin());
            variable.definitions.append(builder.insertBefore({ store.point.block, store.point.index + 1 }, WTF::move(copy)));
        }
        if (!variable.isWrittenOnLeavingPath)
            continue;
        for (BlockID block : blocksBack)
            variable.definitions.append(insertRead({ block, 0 }));
    }
    builder.applyInsertions();

    for (CarriedVariable& variable : m_variables) {
        for (ValueID read : variable.reads)
            becomeUpcast(m_function, read, variable.definitions.first());
    }
    m_analyses.didChangeControlFlow();

    SSARepair repair(builder, m_analyses);
    for (CarriedVariable& variable : m_variables) {
        if (variable.definitions.size() == 1)
            continue;
        SSARepair::Variable repaired = repair.addVariable(variable.definitions.first());
        for (ValueID definition : variable.definitions.span().subspan(1))
            repair.addDefinition(repaired, definition);
    }
    repair.run();
    m_analyses.didChangeControlFlow();
}

// The environment that an inlined function reads its variables from is what its closure has captured. With the closure read ahead of the loop, so is the environment.
bool VariableCarrier::hoistCaptures(const NaturalLoop& loop)
{
    std::optional<Edge> entry = m_analyses.naturalLoops().findSingleEntry(loop);
    if (!entry || m_function.at(entry->from).terminator.kind != TerminatorKind::Jump)
        return false;
    const UseLists& useLists = m_analyses.useLists();
    const NaturalLoops& loops = m_analyses.naturalLoops();

    Vector<ValueID, 4> hoisted;
    for (unsigned position = 0; position < loop.size(); ++position) {
        BlockValues& values = m_function.edit(loop.at(position)).values;
        values.removeAllMatching([&](ValueID id) {
            Value& value = m_function.edit(id);
            if (value.opcode != Opcode::GetCapture)
                return false;
            ValueID closure = m_analyses.definitionThroughUpcasts(value.operand(0));
            if (loops.belongsTo(useLists.locationOf(closure).block, loop) || m_function.at(closure).type != m_function.at(value.operand(0)).type)
                return false;
            value.setOperand(0, closure);
            hoisted.append(id);
            return true;
        });
    }
    if (hoisted.isEmpty())
        return false;
    m_function.edit(entry->from).values.appendVector(hoisted);
    m_analyses.didChangeValues();
    return true;
}

bool VariableCarrier::isPassedOnWithType(const NaturalLoop& loop, size_t parameterIndex, TypeID tested)
{
    ValueID parameter = m_function.at(loop.header()).parameters[parameterIndex];
    return std::ranges::all_of(m_analyses.cfg().incomingEdges(loop.header()), [&](Edge edge) {
        if (!m_analyses.naturalLoops().belongsTo(edge.from, loop))
            return true;
        if (edge.isHandlerEdge() || parameterIndex >= targetOf(m_function, edge).arguments.size())
            return false;
        ValueID argument = targetOf(m_function, edge).arguments[parameterIndex];
        return argument == parameter || isSubtype(argument, tested);
    });
}

// A test of what an earlier one of these tests has proven tests the same value.
auto VariableCarrier::findLeadingTests(const NaturalLoop& loop) -> Vector<LeadingTest, 4>
{
    const CFG& cfg = m_analyses.cfg();
    const NaturalLoops& loops = m_analyses.naturalLoops();
    const UseLists& useLists = m_analyses.useLists();
    const Block& header = m_function.at(loop.header());

    Vector<LeadingTest, 4> tests;
    Vector<ValueID, 4> proven;
    for (BlockID id = loop.header();;) {
        const Block& block = m_function.at(id);
        if (block.handler || block.terminator.kind != TerminatorKind::Branch || block.values.isEmpty() || block.terminator.conditionOperand() != block.values.last())
            break;
        const Value& test = m_function.at(block.values.last());
        bool holdsNothingElse = std::ranges::all_of(block.values.span().first(block.values.size() - 1), [&](ValueID value) {
            return m_function.at(value).opcode == Opcode::LoopHint;
        });
        if (test.opcode != Opcode::TypeTest || !holdsNothingElse || useLists.useCount(block.values.last()) != 1)
            break;
        const Target& taken = block.terminator.targets[0];
        const Target& notTaken = block.terminator.targets[1];
        if (!loops.belongsTo(taken.block, loop) || taken.block == loop.header() || !cfg.singleIncomingEdge(taken.block) || loops.belongsTo(notTaken.block, loop))
            break;

        ValueID tested = test.valueOperand();
        std::optional<size_t> parameterOfHeader;
        if (size_t position = proven.find(tested); position != notFound)
            parameterOfHeader = tests[position].parameterOfHeader;
        else if (size_t index = header.parameters.find(tested); index != notFound)
            parameterOfHeader = index;
        else if (loops.belongsTo(useLists.locationOf(tested).block, loop))
            break;
        if (parameterOfHeader && !isPassedOnWithType(loop, *parameterOfHeader, test.testedType()))
            break;
        tests.append({ id, parameterOfHeader });
        proven.append(taken.proofs.isEmpty() ? ValueID() : m_function.at(taken.block).parameters.last());
        id = taken.block;
    }
    return tests;
}

bool VariableCarrier::moveLeadingTestsAheadOf(const NaturalLoop& loop)
{
    BlockID header = loop.header();
    std::optional<Edge> entry = m_analyses.naturalLoops().findSingleEntry(loop);
    if (!entry || hasEdgeResults(m_function, *entry))
        return false;
    Vector<LeadingTest, 4> tests = findLeadingTests(loop);
    if (tests.isEmpty())
        return false;
    Vector<BlockID, 4> region;
    for (const LeadingTest& test : tests)
        region.append(test.block);
    if (RegionCloner::checkCanDuplicate(m_analyses, region.span()) != CloneCheck::CanClone)
        return false;

    IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
    RegionCloner cloner(builder, m_function);
    cloner.cloneBlocks(region.span(), [](ValueID, Value&) { });
    builder.redirectEdge(*entry, cloner.cloneOf(header), Arguments(targetOf(m_function, *entry).arguments));

    BlockID landing = builder.splitEdge(Edge { cloner.cloneOf(region.last()), 0 });
    Vector<ValueID, 4> provenAheadOfLoop;
    for (size_t position = 0; position < tests.size(); ++position) {
        const Target& taken = m_function.at(tests[position].block).terminator.targets[0];
        if (taken.proofs.isEmpty())
            provenAheadOfLoop.append(ValueID());
        else if (position + 1 == tests.size())
            provenAheadOfLoop.append(m_function.at(landing).parameters.last());
        else
            provenAheadOfLoop.append(cloner.cloneOf(m_function.at(taken.block).parameters.last()));
    }

    Arguments entering;
    for (ValueID parameter : m_function.at(header).parameters)
        entering.append(cloner.cloneOf(parameter));
    for (size_t position = 0; position < tests.size(); ++position) {
        if (tests[position].parameterOfHeader && provenAheadOfLoop[position])
            entering[*tests[position].parameterOfHeader] = provenAheadOfLoop[position];
    }
    builder.setOrigin(m_function.at(landing).terminator.origin());
    m_function.edit(landing).terminator = Terminator { };
    builder.moveToEndOf(landing);
    builder.emitJump(header, WTF::move(entering));

    for (size_t position = 0; position < tests.size(); ++position) {
        BlockID block = tests[position].block;
        if (tests[position].parameterOfHeader || !provenAheadOfLoop[position]) {
            builder.convertBranchToJump(block, 0);
            continue;
        }
        Terminator branch = std::exchange(m_function.edit(block).terminator, Terminator { });
        Arguments arguments = WTF::move(branch.targets[0].arguments);
        for (size_t proof = 0; proof < branch.targets[0].proofs.size(); ++proof)
            arguments.append(provenAheadOfLoop[position]);
        builder.setOrigin(branch.origin());
        builder.moveToEndOf(block);
        builder.emitJump(branch.targets[0].block, WTF::move(arguments));
    }

    SSARepair repair(builder, m_analyses);
    cloner.addVariablesTo(repair, m_analyses);
    builder.emptyUnreachableBlocks();
    m_analyses.didChangeControlFlow();
    repair.run();
    m_analyses.didChangeControlFlow();
    return true;
}

bool VariableCarrier::delayStores(std::span<const VariableOfLoop> variables)
{
    bool changed = false;
    for (size_t first = 0; first < variables.size();) {
        size_t end = first + 1;
        while (end < variables.size() && variables[end].header == variables[first].header)
            ++end;
        if (const NaturalLoop* loop = m_analyses.naturalLoops().headerOf(variables[first].header))
            changed |= delayStoresIn(*loop, variables.subspan(first, end - first));
        first = end;
    }
    return changed;
}

// At the head of a turn the parameter is what the variable should hold, behind a store it is what was stored. An edge that leaves stores whichever of them reaches it.
// A store may wait across an operation that throws for lack of memory alone, as reading a character does while it resolves a rope: by the language it never throws.
bool VariableCarrier::delayStoresIn(const NaturalLoop& loop, std::span<const VariableOfLoop> variables)
{
    struct DelayedVariable {
        VariableOfLoop variable;
        Vector<ProgramPoint, 2> stores;
        bool isSeenByOthers { false };
    };
    const NaturalLoops& loops = m_analyses.naturalLoops();
    BlockID header = loop.header();
    Vector<DelayedVariable, 4> delayed;
    for (const VariableOfLoop& variable : variables) {
        if (m_function.at(header).parameters.contains(variable.parameter))
            delayed.append({ variable, { }, false });
    }

    Vector<Edge, 8> leavingEdges;
    for (unsigned position = 0; position < loop.size(); ++position) {
        BlockID id = loop.at(position);
        const Block& block = m_function.at(id);
        if (block.handler || block.isHandlerBlock())
            return false;
        auto look = [&](Effects effects) {
            if (effects.mayThrow || effects.mayRunOtherCode())
                return false;
            for (DelayedVariable& candidate : delayed)
                candidate.isSeenByOthers |= effects.reads.overlaps(m_program, candidate.variable.location) || effects.writes.overlaps(m_program, candidate.variable.location);
            return true;
        };
        for (uint32_t index = 0; index < block.values.size(); ++index) {
            ValueID value = block.values[index];
            MemoryAccess access = memoryAccessOf(m_function, m_function.at(value));
            DelayedVariable* stored = nullptr;
            if (access.kind == MemoryAccess::Kind::Write) {
                ValueID base = access.base ? m_analyses.sameObjectThroughCasts(access.base) : ValueID();
                for (DelayedVariable& candidate : delayed) {
                    if (candidate.variable.location == access.location && candidate.variable.base == base)
                        stored = &candidate;
                }
            }
            if (stored) {
                stored->stores.append({ id, index });
                continue;
            }
            Effects effects = m_analyses.effectsOf(value).onAnyEdge();
            if (opcodesThrowingOnlyForLackOfMemory.contains(m_function.at(value).opcode) && !effects.mayThrowByRule())
                effects.mayThrow = false;
            if (!look(effects))
                return false;
        }
        if (!look(m_analyses.effectsOfTerminator(id).onAnyEdge()))
            return false;
        for (uint32_t targetIndex = 0; targetIndex < block.terminator.targets.size(); ++targetIndex) {
            if (!loops.belongsTo(block.terminator.targets[targetIndex].block, loop))
                leavingEdges.append(Edge { id, targetIndex });
        }
    }
    delayed.removeAllMatching([](const DelayedVariable& candidate) {
        return candidate.isSeenByOthers || candidate.stores.isEmpty();
    });
    if (delayed.isEmpty() || leavingEdges.isEmpty())
        return false;

    IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
    Vector<BlockID, 8> blocksOnLeavingEdges;
    for (Edge edge : leavingEdges)
        blocksOnLeavingEdges.append(builder.splitEdge(edge));

    Vector<Vector<ValueID, 4>, 4> definitions;
    Vector<ValueID, 4> delayedStores;
    for (const DelayedVariable& candidate : delayed) {
        Vector<ValueID, 4> definitionsOfVariable;
        auto insertCopy = [&](ProgramPoint point, ValueID value) {
            Value copy = IRBuilder::make(Opcode::Upcast, { value });
            copy.setCastType(m_function.at(value).type);
            definitionsOfVariable.append(builder.insertBefore(point, WTF::move(copy)));
        };
        Value store = m_function.at(m_function.at(candidate.stores.first().block).values[candidate.stores.first().index]);
        ValueID storedBySample = memoryAccessOf(m_function, store).stored;
        builder.setOrigin(store.origin());
        insertCopy({ header, 0 }, candidate.variable.parameter);
        for (ProgramPoint point : candidate.stores) {
            ValueID delayedStore = m_function.at(point.block).values[point.index];
            insertCopy({ point.block, point.index + 1 }, memoryAccessOf(m_function, m_function.at(delayedStore)).stored);
            delayedStores.append(delayedStore);
        }
        for (BlockID block : blocksOnLeavingEdges) {
            Value storeOnEdge = store;
            for (uint32_t operand = 0; operand < storeOnEdge.numberOfOperands(); ++operand) {
                if (storeOnEdge.operand(operand) == storedBySample)
                    storeOnEdge.setOperand(operand, definitionsOfVariable.first());
            }
            builder.insertBefore({ block, 0 }, WTF::move(storeOnEdge));
        }
        definitions.append(WTF::move(definitionsOfVariable));
    }
    builder.applyInsertions();
    for (ValueID store : delayedStores)
        becomeUndefined(m_function, store);
    m_analyses.didChangeControlFlow();

    SSARepair repair(builder, m_analyses);
    for (const Vector<ValueID, 4>& definitionsOfVariable : definitions) {
        SSARepair::Variable repaired = repair.addVariable(definitionsOfVariable.first());
        for (ValueID definition : definitionsOfVariable.span().subspan(1))
            repair.addDefinition(repaired, definition);
    }
    repair.run();
    m_analyses.didChangeControlFlow();
    return true;
}

} // anonymous namespace

BlocksWritingLoopVariables findBlocksWritingLoopVariables(FunctionPassContext& context, const NaturalLoop& loop, const BitVector& leaves)
{
    VariableCarrier carrier(context);
    carrier.setFirstTurnRunsInCopy();
    return carrier.findBlocksWritingVariables(loop, leaves);
}

bool carryVariablesAroundSeparatedLoop(FunctionPassContext& context, BlockID header, Vector<VariableOfLoop>& variablesWithStores)
{
    VariableCarrier carrier(context);
    carrier.setFirstTurnRunsInCopy();
    BitVector leaves;
    bool changed = carrier.carryAround(header, leaves);
    variablesWithStores.appendVector(carrier.takeVariablesWithStores());
    return changed;
}

bool delayStoresOfLoopVariables(FunctionPassContext& context, std::span<const VariableOfLoop> variables)
{
    return VariableCarrier(context).delayStores(variables);
}

PassOutcome carryVariablesAroundLoops(FunctionPassContext& context)
{
    return VariableCarrier(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
