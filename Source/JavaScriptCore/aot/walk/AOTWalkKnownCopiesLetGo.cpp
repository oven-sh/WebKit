/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Inlining, `consume(load())`: what a call alone held is let go of behind the copy of its callee. A stale register or slot of the caller would keep the object alive for as long as the caller runs.
// A hint is no code and proves nothing: the sweep lets go of a value behind its last read in Code, wherever the hint stands.
// At a suspension and at a throw: a way that leaves in front of the hint lets go of nothing. Withheld: there is no copy, and the call itself is the last reader.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTOpcodeSets.h"
#include "AOTRepresentation.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

// What was read out of an object is still named by that object. What a call has handed back may be named by nothing else.
static bool isHandedBackByCallToOneUserInKnownCopies(const BodyIndex& index, ValueID operand)
{
    for (ValueID value = operand; value && index.hasOneUse(value) && !index.isParameter(value); value = index.value(value).operand(0)) {
        Opcode opcode = index.opcodeOf(value);
        if (!opcodesThatStateTypes.contains(opcode))
            return familyOf(opcode) == OperationFamily::Call;
    }
    return false;
}

// False: no register or stack word holds a cell of the heap for the value, so there is nothing to clear.
static bool mayReleaseCellInKnownCopies(Walker& walker, Reference released)
{
    if (!holdsCells(walker.representationOf(released)))
        return false;
    TypeID id = walker.typeOf(released);
    if (!id || isNeverType(id))
        return false;
    Type type = walker.types().at(id);
    if (!type.isJavaScriptValue() || !type.hasCells())
        return false;
    const Operation& definition = walker.code().at(rootOfCellInSweptCode(walker.code(), released.id()));
    return !Operation::opcodesWithStorage.contains(definition.opcode) || definition.storage() == AllocationStorage::Heap;
}

Vector<Reference, 2> collectWhatCopyAloneHolds(Walker& walker, const BodyValue& call)
{
    Vector<Reference, 2> held;
    const BodyIndex& index = walker.currentBodyIndex();
    call.forEachOperand([&](ValueID operand, uint32_t) {
        if (!operand || !isHandedBackByCallToOneUserInKnownCopies(index, operand))
            return;
        Reference handedBack = walker.referenceOf(operand);
        if (handedBack && handedBack.isValue())
            held.append(handedBack);
    });
    return held;
}

// Each name of a value may lie in a place of its own, so each is let go of by itself.
// The program has written nothing behind the body of a callee that is walked at its call, so nothing is seen there, and what holds goes on holding.
void letGoWhatCopyHeld(Walker& walker, std::span<const Reference> held, Origin origin)
{
    const Code& code = walker.code();
    Effects nothing = Effects::none();
    for (Reference value : held) {
        Reference name = value;
        while (Reference checked = walker.state().checkedNameOf(name))
            name = checked;
        while (name.isValue() && !walker.isUnreachable()) {
            if (mayReleaseCellInKnownCopies(walker, name)) {
                Draft hint(Opcode::ReleaseHint, origin);
                walker.chooseConservativeFields(hint);
                hint.setValueOperand(name);
                hint.setIsBehindCopiedBody(true);
                walker.build(WTF::move(hint), &nothing);
            }
            const Operation& named = code.at(name.id());
            if (named.opcode != Opcode::Narrowed || code.isConstant(named.operand(0)))
                break;
            name = Reference::value(named.operand(0));
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
