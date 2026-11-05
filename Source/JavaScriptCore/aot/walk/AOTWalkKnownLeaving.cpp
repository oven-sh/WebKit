/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// EscapeAnalysis. `function log() { if (quiet) return; write.apply(null, arguments); }`: a call names an `arguments` that another block has made, that only this code has, and that the body only reads and hands out last.
// The allocation is walked once more in front of the call, and where no other way needs the first one the sweep drops it.
// At a suspension: nothing is left to undo. At a throw: never under a handler, which may name the object. Withheld: the call takes the object as it is.

#include "config.h"
#include "AOTWalkKnown.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTOperandKinds.h"
#include "AOTPlan.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// BodyLiveness::isLiveBefore() says yes where the readers that are left stand on another way. This looks at the rest of the block and at the heads that its edges lead to.
static bool isNamedBehindInKnownLeaving(const BodyIndex& index, const BodyLiveness& liveness, ValueID value, ValueID point)
{
    BodyBlock block = index.block(index.blockOf(point));
    bool isNamed = block.hasHandler();
    auto look = [&](ValueID operand, uint32_t) {
        isNamed |= operand == value;
    };
    for (uint32_t next = point.index() + 1; !isNamed && next < block.endOfValues().index(); ++next) {
        if (index.opcodeOf(ValueID(next)) != Opcode::ReleaseHint)
            index.value(ValueID(next)).forEachOperand(look);
    }
    block.terminator().forEachOperand(look);
    for (BodyTarget target : block.terminator().targets())
        isNamed |= std::ranges::contains(target.arguments(), value) || liveness.isLiveAtHead(value, target.block());
    return isNamed;
}

// Every mention of the object in the body reads its length, an element or the variable of a parameter, which changes nothing of it, or is a call behind which nothing names it, in no other loop than the one that makes it.
// So each such call may make its own: no way makes two, and nobody can hold one against another.
static bool isOnlyReadAndHandedOutLastInKnownLeaving(const BodyIndex& index, const BodyLiveness& liveness, PropertyKeyID length, ValueID arguments)
{
    uint32_t mentionsLeft = index.useCount(arguments);
    if (mentionsLeft == BodyIndex::maxCountedUses)
        return false;
    LoopID loop = index.loopOf(index.blockOf(arguments));
    for (uint32_t next = arguments.index() + 1; mentionsLeft && next < index.numberOfValues(); ++next) {
        if (index.isParameter(ValueID(next)))
            continue;
        BodyValue user = index.value(ValueID(next));
        uint32_t mentions = 0;
        user.forEachOperand([&](ValueID operand, uint32_t) {
            mentions += operand == arguments;
        });
        if (!mentions)
            continue;
        mentionsLeft -= mentions;
        Opcode opcode = user.opcode();
        if (opcode == Opcode::ReleaseHint || opcode == Opcode::GetMappedArgument || (opcode == Opcode::PropGet && !user.isFreeName() && user.key() == length) || (opcode == Opcode::ElemGet && user.propertyOperand() != arguments))
            continue;
        if (!opcodesThatMayTakeArgumentsLast.contains(opcode) || index.loopOf(index.blockOf(user.id())) != loop || isNamedBehindInKnownLeaving(index, liveness, arguments, user.id()))
            return false;
    }
    // What is left stands at the end of a block, on an edge or at a head.
    return !mentionsLeft;
}

ArgumentsMadeWhereTheyLeave::ArgumentsMadeWhereTheyLeave(Walker& walker, const BodyValue& call)
    : m_walker(walker)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID name;
    call.forEachOperand([&](ValueID operand, uint32_t) {
        if (!name && operand && !index.isParameter(operand) && opcodesThatMakeArguments.contains(index.opcodeOf(operand)))
            name = operand;
    });
    Reference bound = name ? walker.referenceBoundTo(name) : Reference();
    if (!bound || !bound.isValue() || walker.isUnreachable() || walker.standsUnderHandler())
        return;
    const Code& code = walker.code();
    ValueID cell = rootOfCellInSweptCode(code, bound.id());
    if (code.isConstant(cell) || !opcodesThatMakeArguments.contains(code.at(cell).opcode) || code.at(cell).block() == walker.currentBlock() || !walker.state().isFresh(Reference::value(cell)))
        return;
    // A rest parameter is a list of the program's, which may have changed since the entry. The language has no name for the hidden list.
    FunctionID function = walker.currentFunction();
    if (!walker.program().at(function).has(FunctionFlags::HasHiddenRest))
        return;
    // What runs once makes the object once, whichever block makes it.
    if (!walker.isInLoop() && (walker.facts().runsAtMostOnce(function) || walker.program().at(function).isModuleBody()))
        return;
    if (!walker.m_liveness.isComputedFor(index))
        walker.m_liveness.compute(index);
    if (!isOnlyReadAndHandedOutLastInKnownLeaving(index, walker.m_liveness, walker.program().keyOf(BuiltinNames::length), name))
        return;

    m_name = name;
    m_boundBefore = bound;
    buildFreshObject(walker, index.value(name));
}

ArgumentsMadeWhereTheyLeave::~ArgumentsMadeWhereTheyLeave()
{
    if (m_name)
        m_walker.bind(m_name, m_boundBefore);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
