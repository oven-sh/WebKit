/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Guesses: `Counter.prototype.n = 0; c.n++`. The store of an update creates the field in its slot, so from the second update on the read finds the slot filled. So the slot of such a read is worth a look,
// though objects leave the property out. It says where a look PAYS and claims nothing: behind the look the slot is read, and where the slot is empty the read goes by name as written.
// It reads the body alone and builds nothing, so a suspension and a throw mean nothing to it. Withheld: nobody asks.

#include "config.h"
#include "AOTWalkNumberGuesses.h"

#if ENABLE(AOT)

#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet constantsBehindReadOfUpdate { Opcode::Integer, Opcode::Double, Opcode::Boolean };

// `emitter.count++`, `o.n += step`: as written, the first operation behind the read that is no arithmetic on numbers is a store under the same name into the same object.
// Arithmetic on numbers and booleans runs no code and throws nothing. So where the read finds a number, the store is sure to follow, and nothing in front of it can ask whether the object has the property.
bool isReadOfUpdateOfField(const Walker& walker, const BodyValue& read)
{
    if (read.opcode() != Opcode::PropGet || read.isFreeName())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(read.id()));
    if (block.hasHandler())
        return false;

    ValueID object = index.valueBehindCasts(read.objectOperand());
    uint32_t numberOfRead = read.id().index();
    // What stands behind the read has been looked at, and the read itself counts where it finds a number.
    auto isNumberOrBoolean = [&](ValueID operand) {
        if (operand.index() >= numberOfRead)
            return true;
        Reference walked = walker.referenceOf(operand);
        return walked && walked.isRecord() && walker.types().at(walker.typeOf(walked)).hasOnlyPrimitives(Primitive::Number | Primitive::Boolean);
    };

    uint32_t end = block.endOfValues().index();
    for (uint32_t number = numberOfRead + 1; number < end; ++number) {
        BodyValue next = index.value(ValueID(number));
        Opcode opcode = next.opcode();
        if (opcode == Opcode::PropSet)
            return !next.isFreeName() && next.key() == read.key() && index.valueBehindCasts(next.objectOperand()) == object;
        if (constantsBehindReadOfUpdate.contains(opcode))
            continue;
        if (!allNumericOperators.contains(opcode) && !opcodesThatStateTypes.contains(opcode))
            return false;
        bool takesOnlyNumbers = true;
        next.forEachOperand([&](ValueID operand, uint32_t) {
            takesOnlyNumbers &= isNumberOrBoolean(operand);
        });
        if (!takesOnlyNumbers)
            return false;
    }
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
