/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
    FunctionID function = walker.currentFunction();
    if (!walker.program().at(function).has(FunctionFlags::HasHiddenRest))
        return;
    if (!walker.isInLoop() && walker.program().at(function).isModuleBody())
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
