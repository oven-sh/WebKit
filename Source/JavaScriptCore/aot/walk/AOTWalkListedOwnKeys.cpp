/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkListedOwnKeys.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsDeclared.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include "AOTWalkCopies.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t closedOperationsOfOwnKeyList = 2;
static constexpr size_t mostFieldsOfOwnKeyListOutsideLoops = 8;

namespace {

enum class OwnKeyList : uint8_t { Keys, Values };

struct CallOfOwnKeyList {
    const BodyValue& call;
    OwnKeyList list;
    Origin origin;
    ExpectedType expected;
};

struct TestedArmOfOwnKeyList {
    BlockID block;
    Reference object;
    size_t order { 0 };
};

struct EndOfArmOfOwnKeyList {
    BlockID last;
    Reference list;
};

} // anonymous namespace

static std::optional<OwnKeyList> listAskedForInListedOwnKeys(Walker& walker, const BodyValue& call)
{
    if (call.isDoneInPlace() || call.argumentsOperands().size() != 1)
        return std::nullopt;
    Reference callee = walker.referenceOf(call.calleeOperand());
    TypeID proven = callee ? walker.typeOf(callee) : TypeID();
    if (!proven)
        return std::nullopt;
    TypeArena& types = walker.types();
    Type type = types.at(proven);
    BuiltinID named = type.isObject() ? singleBuiltinOf(types, type) : BuiltinID();
    if (!named)
        return std::nullopt;

    const BuiltinTable& table = walker.program().builtins();
    BuiltinID builtin = table.canonicalID(named);
    if (builtin == table.canonicalID(Builtins::ObjectKeys))
        return OwnKeyList::Keys;
    if (builtin == table.canonicalID(Builtins::ObjectValues))
        return OwnKeyList::Values;
    return std::nullopt;
}

static bool canWriteOutInListedOwnKeys(size_t numberOfFields)
{
    return numberOfFields <= maxComputedElementsOfArrayLiteral;
}

static Reference buildLiteralInListedOwnKeys(Walker& walker, std::span<const Reference> elements, const CallOfOwnKeyList& asked)
{
    TypeArena& types = walker.types();
    TypeID contents = TypeTable::never;
    for (Reference element : elements)
        contents = types.unionOf(contents, walker.typeOf(element));

    Draft literal(Opcode::NewArray, asked.origin);
    literal.expected = asked.expected;
    literal.inputValueGiven = asked.call.id();
    literal.setContents(elements.empty() ? TypeID(TypeTable::anything) : contents);
    literal.setElementsOperands(elements);
    walker.chooseConservativeFields(literal);
    Reference list = walker.build(WTF::move(literal));
    noteFreshList(walker, list);
    return list;
}

static Reference nameOfOwnKeyInListedOwnKeys(Walker& walker, LayoutID layout, FieldIndex field, Origin origin)
{
    const Program& program = walker.program();
    Draft name(Opcode::String, origin);
    name.setString(program.at(walker.plan().slots().keyOf(program, { layout, field })).text);
    return walker.constant(WTF::move(name));
}

static Reference buildListOfOrderInListedOwnKeys(Walker& walker, const CallOfOwnKeyList& asked, Reference object, const OwnKeyOrder& order)
{
    Vector<Reference, 16> elements;
    for (FieldIndex field : order.fields) {
        Reference element = asked.list == OwnKeyList::Keys ? nameOfOwnKeyInListedOwnKeys(walker, order.layout, field, asked.origin) : valueOfOwnKey(walker, object, order.layout, field, asked.origin);
        if (!element)
            return { };
        elements.append(element);
    }
    return buildLiteralInListedOwnKeys(walker, elements.span(), asked);
}

static bool buildListOfObjectInSightInListedOwnKeys(Walker& walker, const CallOfOwnKeyList& asked, Reference source)
{
    std::optional<OwnKeyOrder> order = ownKeysInSight(walker, source);
    if (!order || !canWriteOutInListedOwnKeys(order->fields.size()))
        return false;
    if (Reference list = buildListOfOrderInListedOwnKeys(walker, asked, source, *order))
        walker.bind(asked.call.id(), list);
    return true;
}

static bool isReturnedAtOnceInListedOwnKeys(const BodyIndex& index, ValueID call)
{
    BodyTerminator end = index.terminator(index.blockOf(call));
    return end.kind() == TerminatorKind::ReturnCall && end.operand(0) == call;
}

static bool buildListBehindTestsInListedOwnKeys(Walker& walker, const CallOfOwnKeyList& asked, ValueID written, Reference source)
{
    OwnKeyPlan plan = planOwnKeyWalk(walker, written, source);
    if (!plan)
        return false;
    if (asked.list == OwnKeyList::Keys && !plan.layoutsAreProven)
        return false;
    size_t mostFields = walker.isInLoop() ? mostFieldsOfOwnKeyWalk : mostFieldsOfOwnKeyListOutsideLoops;
    for (const OwnKeyOrder& order : plan.orders) {
        if (order.fields.size() > mostFields || !canWriteOutInListedOwnKeys(order.fields.size()))
            return false;
    }
    if (!shouldOpenGuess(walker, source, walker.types().forLayout(plan.orders[0].layout), plan.isDeclared, closedOperationsOfOwnKeyList))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    ValueID call = asked.call.id();
    BodyBlock block = index.block(index.blockOf(call));
    uint32_t place = call.index() - block.firstValue().index();
    bool isReturnedAtOnce = isReturnedAtOnceInListedOwnKeys(index, call);

    walker.noteGuessOpened();
    BlockID asWritten = walker.addBlock(BlockFrequency::Unlikely);
    BlockID meeting = walker.addBlock();

    Vector<TestedArmOfOwnKeyList, mostArmsOfOwnKeyWalk> tested;
    for (size_t number = 0; number < plan.orders.size() && !walker.isUnreachable(); ++number) {
        BlockID whereTestFails = number + 1 == plan.orders.size() ? asWritten : walker.addBlock();
        Reference object = source;
        if (goOnWhereObjectHasOwnKeys(walker, object, plan.orders[number], plan.layoutsAreProven, asked.origin, whereTestFails))
            tested.append({ walker.currentBlock(), object, number });
        if (walker.hasFailed())
            return true;
        walker.continueIn(whereTestFails);
    }

    Vector<EndOfArmOfOwnKeyList, mostArmsOfOwnKeyWalk + 1> ends;
    auto endArm = [&] {
        if (isReturnedAtOnce && !walker.isUnreachable())
            walker.endArmWithReturnOf(block.id());
        if (walker.isUnreachable())
            return;
        Reference list = walker.referenceOf(call);
        if (!list || !list.isRecord())
            return walker.fail("an arm of a list of own keys has no list"_s);
        ends.append({ walker.currentBlock(), list });
    };

    {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(block.id(), place, place + 1);
        endArm();
    }
    if (walker.hasFailed())
        return true;

    for (const TestedArmOfOwnKeyList& arm : tested) {
        walker.continueIn(arm.block);
        Reference built = buildListOfOrderInListedOwnKeys(walker, asked, arm.object, plan.orders[arm.order]);
        if (walker.hasFailed())
            return true;
        if (!built)
            continue;
        walker.bind(call, built);
        endArm();
        if (walker.hasFailed())
            return true;
    }

    if (ends.isEmpty()) {
        walker.continueIn(meeting);
        return true;
    }

    const Code& code = walker.code();
    TypeID type = code.at(ends[0].list.id()).type;
    Vector<Representation, mostArmsOfOwnKeyWalk + 1> forms;
    for (const EndOfArmOfOwnKeyList& end : ends) {
        type = joinOf(walker.types(), walker.facts().families(), type, code.at(end.list.id()).type);
        forms.append(walker.representationOf(end.list));
    }
    Reference met = walker.addParameter(meeting, type, walker.formOfParameter(type, forms.span()));
    for (const EndOfArmOfOwnKeyList& end : ends) {
        walker.continueIn(end.last);
        walker.jumpTo(meeting, singleElementSpan(end.list));
        if (walker.hasFailed())
            return true;
    }
    walker.continueIn(meeting);
    walker.bind(call, met);
    if (walker.program().builtins().mayHavePristineInstances(BuiltinClasses::Array))
        noteListMadeByCaller(walker, met);
    return true;
}

bool buildOwnKeyListOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::TypedOperations))
        return false;
    std::optional<OwnKeyList> list = listAskedForInListedOwnKeys(walker, value);
    if (!list)
        return false;
    ValueID written = value.argumentsOperands()[0];
    Reference source = walker.referenceOf(written);
    if (!source || !source.isRecord())
        return false;

    Draft draft = walker.draftOf(value);
    Origin origin = draft.origin();
    origin.isTailCall = false;
    CallOfOwnKeyList asked { value, *list, origin, draft.expected };
    if (!buildListOfObjectInSightInListedOwnKeys(walker, asked, source) && !buildListBehindTestsInListedOwnKeys(walker, asked, written, source))
        return false;
    walker.count(StatisticID::OwnKeyWalksExpanded);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
