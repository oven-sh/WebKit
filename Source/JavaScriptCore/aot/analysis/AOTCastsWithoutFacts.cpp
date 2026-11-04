/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCastsWithoutFacts.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTThreshold.h"
#include "AOTTypeTable.h"
#include "AOTValueIdentity.h"

namespace JSC { namespace AOT { namespace Typed {

// By what an operation is declared to do at the most. A check becomes a test that may fail. A loop hint leaves when the program is told to end, whatever its values are.
static constexpr OpcodeSet operationsThatMayLeave = [] {
    OpcodeSet opcodes { Opcode::Check };
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (opcode != Opcode::LoopHint && declaredBoundOf(opcode).mayExitSideways())
            opcodes.add(opcode);
    }
    return opcodes;
}();

static bool isCastWithoutFact(const Value& value)
{
    return value.opcode == Opcode::AssumeType && !value.factOperand();
}

// How far a cast is looked for behind its value. A cast that stands farther away names a fact. A fact between the two does not count: tying one cast parts no other.
AOT_THRESHOLD(uint32_t, mostOperationsBetweenValueAndCast, 16);

static bool standsBehindItsValue(const Function& function, const Block& block, uint32_t indexOfCast)
{
    ValueID narrowed = function.at(block.values[indexOfCast]).valueOperand();
    while (isCastThatProvesNothing(function.at(narrowed).opcode) || isCastWithoutFact(function.at(narrowed)))
        narrowed = function.at(narrowed).operand(0);
    if (isRematerializable(function.at(narrowed)))
        return true;

    uint32_t numberOfOperationsBetween = 0;
    for (uint32_t index = indexOfCast; index--;) {
        if (block.values[index] == narrowed)
            return true;
        Opcode opcode = function.at(block.values[index]).opcode;
        if (makesFact(opcode))
            continue;
        if (operationsThatMayLeave.contains(opcode) || ++numberOfOperationsBetween == mostOperationsBetweenValueAndCast)
            return false;
    }
    return function.at(narrowed).opcode == Opcode::Parameter && block.parameters.contains(narrowed);
}

Vector<CastAwayFromItsValue, 4> findCastsAwayFromTheirValues(const Program& program, const Function& function)
{
    Vector<CastAwayFromItsValue, 4> found;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const Block& block = function.blocks[blockIndex];
        for (uint32_t index = 0; index < block.values.size(); ++index) {
            const Value& value = function.at(block.values[index]);
            if (isCastWithoutFact(value) && !standsBehindItsValue(function, block, index) && !program.typeOf(function, value.valueOperand()).mayBeUnspecified())
                found.append({ BlockID(blockIndex), index, block.values[index] });
        }
    }
    return found;
}

Vector<BlockID, 4> tieCastsWhereTheyStand(const Program& program, Function& function)
{
    Vector<CastAwayFromItsValue, 4> places = findCastsAwayFromTheirValues(program, function);
    Vector<BlockID, 4> changedBlocks;
    for (size_t first = 0; first < places.size();) {
        BlockID block = places[first].block;
        changedBlocks.append(block);
        size_t end = first;
        while (end < places.size() && places[end].block == block)
            ++end;
        const BlockValues& before = function.at(block).values;
        BlockValues values;
        values.reserveInitialCapacity(before.size() + end - first);
        // Casts that stand side by side hold from one place on.
        ValueID fact;
        for (uint32_t index = 0; index < before.size(); ++index) {
            if (first < end && places[first].indexInBlock == index) {
                ValueID cast = places[first++].cast;
                if (!fact) {
                    Value made(Opcode::Specified);
                    made.type = TypeTable::fact;
                    made.setOrigin(function.at(cast).origin());
                    fact = function.add(WTF::move(made));
                    values.append(fact);
                }
                function.edit(cast, ValueEdit::Definition).setFactOperand(fact);
            } else if (!isCastWithoutTest(function.at(before[index]).opcode))
                fact = { };
            values.append(before[index]);
        }
        function.edit(block, BlockEdit::Values).values = WTF::move(values);
        RELEASE_ASSERT(first == end);
    }
    return changedBlocks;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
