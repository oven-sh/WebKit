/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEnvironmentsHandedBack.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTSlotPlan.h"
#include "AOTThinSummary.h"
#include "AOTThreads.h"
#include "MarkedSpace.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

// Whoever lets a cell of the heap hold an environment sets a byte in it first, and what may be nothing has no byte to set.
// An environment is written as the type of a capture, and of a slot of a scope or of a saved frame: the front end writes it nowhere else. Where one came "or nothing" by another road the walk stops the build.
static BitVector findEnvironmentsThatPositionsTakeOrNothing(const Program& program)
{
    BitVector environments;
    auto note = [&](TypeID written) {
        if (!written || holdsOnlyPrimitives(written))
            return;
        Type type = program.at(written);
        if (type.environment && type.primitives)
            environments.set(type.environment.index());
    };
    for (const Layout& layout : program.layouts) {
        if (layout.kind != LayoutKind::Environment && layout.kind != LayoutKind::CoroutineFrame)
            continue;
        for (const Field& field : layout.fields)
            note(field.declared.written());
    }
    for (const Function& function : program.functions) {
        for (const DeclaredCapture& capture : function.captures)
            note(capture.written());
    }
    return environments;
}

// The walk marks at whatever else names the environment: walk/AOTWalkEnvironmentCells.cpp goes by the rows. This only says where asking the byte pays, so it may be wrong either way.
static bool leavesCellToItsFrames(const BodyValue& user, uint32_t operandIndex)
{
    switch (user.opcode()) {
    case Opcode::GetEnvironmentField:
    case Opcode::PutEnvironmentField:
        return !operandIndex;
    case Opcode::CallDirect:
    case Opcode::ReleaseHint:
        return true;
    default:
        return false;
    }
}

// `function related(a, b) { let depth = 0; return walk(a, b); function walk(x, y) { depth++; .. } }`: some way leads from the allocation to where the body has done with the cell and comes by no operation that keeps it.
// `function counter() { let n = 0; return () => ++n; }`: none does: the byte would be set in every run, and nobody would ever take the cell again.
static bool someWayKeepsNothing(const BodyIndex& index, ValueID environment)
{
    BlockID making = index.blockOf(environment);
    BitVector keeps(index.numberOfBlocks());
    uint32_t mentionsLeft = index.useCount(environment);
    bool countsEveryMention = mentionsLeft < BodyIndex::maxCountedUses;
    // A scope that closures alone take is made for them: on a way that makes none of them it is made for nothing, which is another matter than who gets its cell.
    bool isHandedToFunctionEnteredItself = false;
    // What the allocation stands above has the numbers behind its own.
    std::span<const BlockID> below = index.blocksInDominatorOrder().subspan(index.dominatorNumber(making), index.lastDominatorNumberBelow(making) - index.dominatorNumber(making) + 1);
    for (BlockID id : below) {
        if (countsEveryMention && !mentionsLeft)
            break;
        BodyBlock block = index.block(id);
        for (BodyValue user : block.values()) {
            if (user.id().index() <= environment.index())
                continue;
            user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (operand != environment)
                    return;
                --mentionsLeft;
                isHandedToFunctionEnteredItself |= user.opcode() == Opcode::CallDirect;
                if (!leavesCellToItsFrames(user, operandIndex))
                    keeps.quickSet(id.index());
            });
        }
        // What an edge brings to a parameter goes on under another name: the next turn of a loop can take the cell that this turn has done with.
        block.terminator().forEachOperand([&](ValueID operand, uint32_t) {
            if (operand == environment)
                keeps.quickSet(id.index());
        });
    }
    if (keeps.quickGet(making.index()) || !isHandedToFunctionEnteredItself)
        return false;

    BitVector seen(index.numberOfBlocks());
    Vector<BlockID, 16> worklist { making };
    seen.quickSet(making.index());
    while (!worklist.isEmpty()) {
        BodyBlock block = index.block(worklist.takeLast());
        BodyTerminator end = block.terminator();
        if (end.kind() == TerminatorKind::Return)
            return true;
        for (BodyTarget target : end.targets()) {
            // Out from under the allocation, or round to it: the name is gone.
            if (!index.strictlyDominates(making, target.block()))
                return true;
            if (!keeps.quickGet(target.block().index()) && !seen.quickSet(target.block().index()))
                worklist.append(target.block());
        }
    }
    return false;
}

void planEnvironmentsHandedBack(const Program& program, const Facts& facts, BuildContext& build, SlotPlanParts& parts)
{
    // A debugger stands at any operation and keeps whatever it finds in the scope.
    if (program.admitsInspector || !withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::CellsInFrames))
        return;

    Vector<FunctionID> makers;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        // The frame of a coroutine is a cell of the heap that holds its scope from the first suspension on, and where such a body has done with a cell only its liveness says.
        if (!facts.summaries().at(FunctionID(index)).environmentsHandedToFunctionsEntered.isEmpty() && !program.functions[index].isCoroutine())
            makers.append(FunctionID(index));
    }
    if (makers.isEmpty())
        return;
    Vector<Vector<LayoutID, 2>> found(makers.size());
    forEachIndexInParallel(build.parallelLoops(), makers.size(), [&](unsigned place) {
        const BodyIndex* index = facts.bodies().find(makers[place]);
        if (!index || index->hasIrreducibleFlow())
            return;
        for (ValueID environment : facts.summaries().at(makers[place]).environmentsHandedToFunctionsEntered) {
            LayoutID layout = index->value(environment).as<Opcode::NewObject>().layout();
            // The next run of the place takes the cell that this run has done with.
            if (facts.runsAtMostOnce(makers[place]))
                continue;
            // The scope of a function is done with where the function returns. Where the scope of a turn of a loop is done with, and whether a throw has left the block in front of the allocation, only liveness would say.
            BlockID making = index->blockOf(environment);
            if (index->loopOf(making) || index->block(making).hasHandler())
                continue;
            if (!found[place].contains(layout) && someWayKeepsNothing(*index, environment))
                found[place].append(layout);
        }
    });

    BitVector takenOrNothing = findEnvironmentsThatPositionsTakeOrNothing(program);
    for (const auto& layouts : found) {
        for (LayoutID id : layouts) {
            // A frame of a coroutine has the storage of an environment and is laid out by others.
            if (program.at(id).kind != LayoutKind::Environment || takenOrNothing.get(id.index()))
                continue;
            // Without slots: the layout is never allocated, or an error has been reported of it. Only a cell of the heap is handed back.
            PlannedLayout& planned = parts.layouts[id.index()];
            if (planned.storage != LayoutStorage::Environment || planned.slots.isEmpty() || planned.liesOnlyInFrames || planned.isEscapedOffset)
                continue;
            uint64_t endOfFields = offsetOfFirstEnvironmentVariable;
            for (const PlacedSlot& slot : planned.slots) {
                if (slot.hasStorage())
                    endOfFields = std::max<uint64_t>(endOfFields, slot.end());
            }
            uint64_t size = WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(endOfFields + sizeof(bool));
            if (size > build.limit(LimitID::MaxFieldOffset))
                continue;
            planned.isEscapedOffset = static_cast<uint32_t>(endOfFields);
            build.statistics().count(StatisticID::AllocateInFramesChanges);
            planned.size = static_cast<uint32_t>(size);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
