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
    for (uint32_t next = point.index() + 1; !isNamed && next < block.endOfValues().index(); ++next)
        index.value(ValueID(next)).forEachOperand(look);
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
        if (opcode == Opcode::GetMappedArgument || (opcode == Opcode::PropGet && !user.isFreeName() && user.key() == length) || (opcode == Opcode::ElemGet && user.propertyOperand() != arguments))
            continue;
        if (!opcodesThatMayTakeArgumentsLast.contains(opcode) || index.loopOf(index.blockOf(user.id())) != loop || isNamedBehindInKnownLeaving(index, liveness, arguments, user.id()))
            return false;
    }
    return !mentionsLeft;
}

static bool collectNamesOfListOnlyReadInKnownLeaving(const BodyIndex& index, PropertyKeyID length, ValueID name, Vector<ValueID, 8>& names, Vector<ValueID, 4>& takers)
{
    uint32_t mentionsLeft = index.useCount(name);
    if (mentionsLeft == BodyIndex::maxCountedUses)
        return false;
    bool isListItself = names.isEmpty();
    names.append(name);
    for (uint32_t next = 0; mentionsLeft && next < index.numberOfValues(); ++next) {
        if (index.isParameter(ValueID(next)))
            continue;
        BodyValue user = index.value(ValueID(next));
        uint32_t mentions = 0;
        user.forEachOperand([&](ValueID operand, uint32_t) {
            mentions += operand == name;
        });
        if (!mentions)
            continue;
        mentionsLeft -= mentions;
        Opcode opcode = user.opcode();
        if ((opcode == Opcode::PropGet && !user.isFreeName() && user.key() == length) || (opcode == Opcode::ElemGet && user.propertyOperand() != name))
            continue;
        if (inputPassThroughOpcodes.contains(opcode) && mentions == 1 && user.operand(0) == name) {
            if (!collectNamesOfListOnlyReadInKnownLeaving(index, length, user.id(), names, takers))
                return false;
            continue;
        }
        if (!isListItself || !opcodesThatMayTakeArgumentsLast.contains(opcode) || index.loopOf(index.blockOf(user.id())))
            return false;
        takers.append(user.id());
    }
    return !mentionsLeft;
}

static bool restListIsOnlyReadAndHandedOutLastInKnownLeaving(const BodyIndex& index, const BodyLiveness& liveness, PropertyKeyID length, ValueID list)
{
    Vector<ValueID, 8> names;
    Vector<ValueID, 4> takers;
    if (!collectNamesOfListOnlyReadInKnownLeaving(index, length, list, names, takers))
        return false;
    return std::ranges::all_of(takers, [&](ValueID taker) {
        return std::ranges::none_of(names, [&](ValueID name) {
            return isNamedBehindInKnownLeaving(index, liveness, name, taker);
        });
    });
}

bool restListMayFallAwayInCopy(const Walker& walker, FunctionID callee)
{
    const Function& function = walker.program().at(callee);
    const BodyIndex* body = function.has(FunctionFlags::HasRestParameter) ? walker.facts().bodies().find(callee) : nullptr;
    if (!body)
        return false;
    BodyLiveness liveness;
    liveness.compute(*body);
    ValueID list = body->block(entryBlock).parameter(function.firstParameterIndex() + function.numberOfPositionalParameters());
    return restListIsOnlyReadAndHandedOutLastInKnownLeaving(*body, liveness, walker.program().keyOf(BuiltinNames::length), list);
}

void ArgumentsMadeWhereTheyLeave::makeRestListOfCopyWhereItLeaves(const BodyValue& call)
{
    Walker& walker = m_walker;
    const BodyIndex& index = walker.currentBodyIndex();
    const Function& function = walker.program().at(walker.currentFunction());
    if (!walker.depthOfCopies() || !function.has(FunctionFlags::HasRestParameter) || walker.isUnreachable() || walker.standsUnderHandler())
        return;
    ValueID name = index.block(entryBlock).parameter(function.firstParameterIndex() + function.numberOfPositionalParameters());
    uint32_t mentions = 0;
    call.forEachOperand([&](ValueID operand, uint32_t) {
        mentions += operand == name;
    });
    Reference bound = mentions == 1 ? walker.referenceBoundTo(name) : Reference();
    if (!bound || !bound.isValue())
        return;
    const Code& code = walker.code();
    ValueID cell = underlyingCellOf(code, bound.id());
    if (code.isConstant(cell) || code.at(cell).opcode != Opcode::NewArray || code.at(cell).block() == walker.currentBlock() || !walker.state().isFresh(Reference::value(cell)))
        return;
    std::optional<Vector<Reference, 8>> elements = elementsOfFreshList(walker, bound);
    if (!elements)
        return;
    if (!walker.m_liveness.isComputedFor(index))
        walker.m_liveness.compute(index);
    if (!restListIsOnlyReadAndHandedOutLastInKnownLeaving(index, walker.m_liveness, walker.program().keyOf(BuiltinNames::length), name))
        return;

    Origin origin = walker.originOf(call);
    origin.isTailCall = false;
    walker.checkWhatIsInSightOfList(bound, origin);
    Draft again(Opcode::NewArray, origin);
    walker.chooseConservativeFields(again);
    again.standsForNoValueOfInput = true;
    again.setContents(TypeTable::anything);
    again.setVariadicOperands(elements->span());
    Reference made = walker.build(WTF::move(again));
    if (!made)
        return;
    noteFreshList(walker, made);
    walker.count(StatisticID::RestArrayAllocationsSunk);
    m_name = name;
    m_boundBefore = bound;
    walker.bind(name, made);
}

ArgumentsMadeWhereTheyLeave::ArgumentsMadeWhereTheyLeave(Walker& walker, const BodyValue& call)
    : m_walker(walker)
{
    makeRestListOfCopyWhereItLeaves(call);
    if (m_name)
        return;
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
    ValueID cell = underlyingCellOf(code, bound.id());
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
    ValueID list = walker.walksBodyBehindEntry() && index.opcodeOf(name) == Opcode::CreateUnmappedArguments ? index.value(name).as<Opcode::CreateUnmappedArguments>().restOperand() : ValueID();
    Reference listBefore = list ? walker.referenceBoundTo(list) : Reference();
    if (Reference again = listBefore ? walker.rematerializeHiddenRestArray() : Reference())
        walker.bind(list, again);
    buildFreshObject(walker, index.value(name));
    if (listBefore)
        walker.bind(list, listBefore);
}

ArgumentsMadeWhereTheyLeave::~ArgumentsMadeWhereTheyLeave()
{
    if (m_name)
        m_walker.bind(m_name, m_boundBefore);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
