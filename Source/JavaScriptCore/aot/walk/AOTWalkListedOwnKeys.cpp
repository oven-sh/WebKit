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
#include "AOTWalkKnown.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t closedOperationsOfOwnKeyList = 2;
static constexpr size_t mostFieldsOfOwnKeyListOutsideLoops = 8;

namespace {

enum class OwnKeyList : uint8_t { Keys, Values, Entries };

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
    if (builtin == table.canonicalID(Builtins::ObjectEntries) && walker.isInLoop())
        return OwnKeyList::Entries;
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
        Reference name = asked.list == OwnKeyList::Values ? Reference() : nameOfOwnKeyInListedOwnKeys(walker, order.layout, field, asked.origin);
        Reference value = asked.list == OwnKeyList::Keys ? Reference() : valueOfOwnKey(walker, object, order.layout, field, asked.origin);
        if ((asked.list != OwnKeyList::Values && !name) || (asked.list != OwnKeyList::Keys && !value))
            return { };
        Reference element = name ? name : value;
        if (asked.list == OwnKeyList::Entries) {
            Draft pair(Opcode::NewArray, asked.origin);
            pair.setContents(walker.types().unionOf(walker.typeOf(name), walker.typeOf(value)));
            pair.setElementsOperands(std::array { name, value });
            walker.chooseConservativeFields(pair);
            element = walker.build(WTF::move(pair));
            if (element)
                noteFreshList(walker, element);
        }
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

static ValueID findReadOfLengthThatAloneReadsListInListedOwnKeys(Walker& walker, ValueID call)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call));
    ValueID list = call;
    for (ValueID reader = index.onlyUsingValue(list); reader && reader.index() > list.index() && reader.index() <= list.index() + 2 && block.holds(reader); reader = index.onlyUsingValue(list)) {
        BodyValue operation = index.value(reader);
        ValueID between = reader.index() == list.index() + 2 ? ValueID(list.index() + 1) : ValueID();
        bool readsUnderWrittenIndex = between && operation.opcode() == Opcode::ElemGet && operation.objectOperand() == list && operation.as<Opcode::ElemGet>().propertyOperand() == between
            && index.opcodeOf(between) == Opcode::Integer && index.onlyUsingValue(between) == reader;
        if (between && !readsUnderWrittenIndex)
            return { };
        if (operation.opcode() == Opcode::PropGet || readsUnderWrittenIndex) {
            std::optional<BodyUser> taker = index.firstUser(reader);
            bool readsLength = !readsUnderWrittenIndex && !operation.isFreeName() && operation.key() == walker.program().keyOf(BuiltinNames::length);
            bool standsInStretch = reader.index() - block.firstValue().index() < walker.endOfStretchBeingWalked();
            return (readsLength || readsUnderWrittenIndex) && standsInStretch && !(taker && taker->kind == BodyUseKind::BlockSubject) ? reader : ValueID();
        }
        if (!opcodesThatStateTypes.contains(operation.opcode()))
            return { };
        list = reader;
    }
    return { };
}

static bool buildListBehindTestsInListedOwnKeys(Walker& walker, const CallOfOwnKeyList& asked, ValueID written, Reference source)
{
    OwnKeyPlan plan = planOwnKeyWalk(walker, written, source);
    if (!plan)
        return false;
    ValueID readOfLength = findReadOfLengthThatAloneReadsListInListedOwnKeys(walker, asked.call.id());
    if (asked.list == OwnKeyList::Keys && !plan.layoutsAreProven && (!readOfLength || !plan.hasArmForEveryExpectedOrder))
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

    walker.recordSpeculation();
    BlockID asWritten = walker.addBlock(BlockFrequency::Unlikely);
    BlockID meeting = walker.addBlock();

    Vector<TestedArmOfOwnKeyList, mostArmsOfOwnKeyWalk> tested;
    for (size_t number = 0; number < plan.orders.size() && !walker.isUnreachable(); ++number) {
        BlockID whereTestFails = number + 1 == plan.orders.size() ? asWritten : walker.addBlock();
        Reference object = source;
        if (continueIfOwnKeys(walker, object, plan.orders[number], plan.layoutsAreProven, asked.origin, whereTestFails))
            tested.append({ walker.currentBlock(), object, number });
        if (walker.hasFailed())
            return true;
        walker.continueIn(whereTestFails);
    }

    uint32_t end = (readOfLength ? readOfLength.index() - block.firstValue().index() : place) + 1;
    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), place, end);
    Walker::ArmsThatMeet ends;
    walker.walkClosedArm(walker.currentBlock(), stretch, ends);
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
        for (uint32_t next = place + 1; next < end; ++next)
            walker.bind(ValueID(block.firstValue().index() + next), Reference());
        if (end > place + 1)
            walker.walkStretch(block.id(), place + 1, end);
        walker.listArm(stretch, ends);
        if (walker.hasFailed())
            return true;
    }

    if (!walker.joinArms(stretch, ends, meeting))
        return true;
    if (Reference met = end == place + 1 ? walker.referenceBoundTo(call) : Reference(); met && walker.program().builtins().mayHavePristineInstances(BuiltinClasses::Array))
        noteListMadeByCaller(walker, met);
    return true;
}

void noteListedOwnKeys(Walker& walker, ValueID id)
{
    BodyValue call = walker.currentBodyIndex().value(id);
    if (call.opcode() != Opcode::Call || listAskedForInListedOwnKeys(walker, call) != OwnKeyList::Keys)
        return;
    Reference list = walker.isUnreachable() ? Reference() : walker.referenceBoundTo(call.id());
    Reference object = walker.referenceOf(call.argumentsOperands()[0]);
    if (!list || !list.isValue() || !object || !object.isValue())
        return;
    static constexpr AbstractHeapSet heapsThatTellOwnKeys { AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::NamedProperties };
    noteKnown(walker, KnownNote::ListedOwnKeys, object, list, 0, LocationMasks(heapsThatTellOwnKeys), 1, true);
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
    if (*list == OwnKeyList::Keys)
        noteListedOwnKeys(walker, value.id());
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
