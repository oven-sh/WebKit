/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTNameClass.h"
#include "AOTSlotRepresentation.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class FieldInOwnKeyPlan : uint8_t {
    Unknown,
    LeftOut,
    Listed,
};

} // anonymous namespace

bool orderWordTellsOwnKeysOf(const Walker& walker, LayoutID layout)
{
    const Plan& plan = walker.plan();
    return plan.layout(layout).holdsCompiledObjects() && plan.shapes().shapesOf(layout).count == 1;
}

static FieldInOwnKeyPlan classifyFieldInCopiesOwnKeys(const Walker& walker, LayoutID layout, FieldIndex field)
{
    const Program& program = walker.program();
    const Layout& record = program.at(layout);
    FieldFacts fieldFacts = walker.facts().field({ layout, field });
    PropertyKeyID key;
    OptionSet<FieldForm> form = fieldFacts.formOfValue;
    if (field < record.fields.size()) {
        const Field& declared = record.fields[field];
        if (declared.computedKey)
            return FieldInOwnKeyPlan::Unknown;
        if (declared.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsBrandOfMethods, FieldFlags::IsHidden }))
            return FieldInOwnKeyPlan::LeftOut;
        key = declared.key;
        form.add(declared.form);
    } else
        key = walker.plan().slots().keyOf(program, { layout, field });
    if (!key)
        return FieldInOwnKeyPlan::Unknown;

    switch (program.at(key).kind) {
    case PropertyKeyKind::Text:
        break;
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return FieldInOwnKeyPlan::LeftOut;
    }
    if (form.contains(FieldForm::IsNotEnumerable))
        return FieldInOwnKeyPlan::LeftOut;
    if (form.contains(FieldForm::IsAccessor) || fieldFacts.facts.contains(FieldFact::IsAccessor) || walker.plan().slot(layout, field).holdsAccessor)
        return FieldInOwnKeyPlan::Unknown;
    return FieldInOwnKeyPlan::Listed;
}

static bool canPlanOwnKeysOfLayoutInCopiesOwnKeys(const Walker& walker, LayoutID layout)
{
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    if (!isFoundByShapeTest(program.at(layout)) || !plan.isInUse(layout) || !orderWordTellsOwnKeysOf(walker, layout))
        return false;

    const PlannedLayout& planned = plan.layout(layout);
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        if (planned.slots[field].hasStorage() && !canLeaveSlot(planned.slots[field].representation))
            return false;
        PropertyKeyID key = plan.slots().keyOf(program, { layout, field });
        if (key && program.at(key).kind == PropertyKeyKind::Text && (program.isIndexLike(key) || program.nameClassOf(key) == NameClass::Proto))
            return false;
    }
    return true;
}

static std::optional<OwnKeyOrder> ownKeysAmongCreatedFieldsInCopiesOwnKeys(const Walker& walker, LayoutID layout, std::span<const FieldIndex> created)
{
    OwnKeyOrder order { layout, { }, { } };
    order.created.append(created);
    for (FieldIndex field : created) {
        switch (classifyFieldInCopiesOwnKeys(walker, layout, field)) {
        case FieldInOwnKeyPlan::Unknown:
            return std::nullopt;
        case FieldInOwnKeyPlan::LeftOut:
            break;
        case FieldInOwnKeyPlan::Listed: {
            const PlacedSlot& slot = walker.plan().slot(layout, field);
            if (!slot.hasStorage() || (slot.isOutOfLine() && slot.mayExistWithoutStorage))
                return std::nullopt;
            if (slot.representation == SlotRepresentation::DeferredClosure)
                return std::nullopt;
            order.fields.append(field);
            break;
        }
        }
    }
    return order;
}

static LayoutID onlyLayoutProvenInCopiesOwnKeys(Walker& walker, TypeID proven)
{
    if (!proven)
        return { };
    Type type = walker.types().at(proven);
    if (!type.isJavaScriptValue() || type.primitives || !type.hasOnlyLayouts() || type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow) || !holdsOneMember(type.layouts))
        return { };
    return singleLayoutOf(walker.types(), type);
}

static Reference nameWhoseOrderWordIsKnownInCopiesOwnKeys(Walker& walker, Reference object)
{
    if (walker.state().orderWordOf(object))
        return object;
    ValueID cell = rootOfCellInSweptCode(walker.code(), object.id());
    if (walker.code().isConstant(cell))
        return { };
    Reference root = Reference::value(cell);
    return walker.state().orderWordOf(root) ? root : Reference();
}

std::optional<OwnKeyOrder> ownKeysInSight(Walker& walker, Reference object)
{
    if (!walker.has(Mechanism::TypedOperations) || !object || !object.isValue())
        return std::nullopt;
    Reference known = nameWhoseOrderWordIsKnownInCopiesOwnKeys(walker, object);
    LayoutID layout = known ? onlyLayoutProvenInCopiesOwnKeys(walker, walker.typeOf(object)) : LayoutID();
    if (!layout || !canPlanOwnKeysOfLayoutInCopiesOwnKeys(walker, layout))
        return std::nullopt;

    uint32_t orderWord = *walker.state().orderWordOf(known);
    if (orderWord & ~orderAndCountMask || isOrderStoredOutOfLine(orderWord))
        return std::nullopt;
    const KeyOrders& orders = walker.plan().keyOrders();
    std::span<const FieldIndex> created;
    if (uint32_t count = countOf(orderWord)) {
        if (orderOf(orderWord) >= orders.numberOfOrdersOf(layout) || count > orders.order(layout, orderOf(orderWord)).size())
            return std::nullopt;
        created = orders.order(layout, orderOf(orderWord)).first(count);
    }
    return ownKeysAmongCreatedFieldsInCopiesOwnKeys(walker, layout, created);
}

static bool isRequiredInCopiesOwnKeys(const Walker& walker, LayoutID layout, FieldIndex field)
{
    const Layout& record = walker.program().at(layout);
    if (field < record.fields.size())
        return record.fields[field].flags.contains(FieldFlags::IsRequired);
    return walker.facts().linked().isAppendedFieldRequiredBySomeWrittenType({ layout, field });
}

static Vector<uint32_t, 2> countsAtWhichMakersStopInCopiesOwnKeys(const Walker& walker, LayoutID layout, std::span<const FieldIndex> order)
{
    uint32_t behindLastRequiredField = 0;
    for (uint32_t place = 0; place < order.size(); ++place) {
        if (isRequiredInCopiesOwnKeys(walker, layout, order[place]))
            behindLastRequiredField = place + 1;
    }

    Vector<uint32_t, 2> counts;
    uint32_t whole = static_cast<uint32_t>(order.size());
    if (!walker.isInLoop()) {
        const PlannedLayout& planned = walker.plan().layout(layout);
        uint32_t numberOfFieldsThatMakersCreate = 0;
        bool everyFieldIsRequired = true;
        uint32_t numberOfDeclaredFields = static_cast<uint32_t>(walker.program().at(layout).fields.size());
        for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
            if (!planned.slots[field].hasStorage())
                continue;
            bool isRequired = isRequiredInCopiesOwnKeys(walker, layout, field);
            if (!isRequired && field >= numberOfDeclaredFields)
                continue;
            ++numberOfFieldsThatMakersCreate;
            everyFieldIsRequired &= isRequired;
        }
        if (everyFieldIsRequired && behindLastRequiredField == numberOfFieldsThatMakersCreate)
            counts.append(behindLastRequiredField);
        return counts;
    }
    counts.append(whole);
    if (behindLastRequiredField && behindLastRequiredField != whole)
        counts.append(behindLastRequiredField);
    return counts;
}

static bool appendOrdersOfLayoutInCopiesOwnKeys(const Walker& walker, LayoutID layout, unsigned mostArms, OwnKeyPlan& plan)
{
    if (!canPlanOwnKeysOfLayoutInCopiesOwnKeys(walker, layout))
        return false;

    const KeyOrders& keyOrders = walker.plan().keyOrders();
    size_t firstArmOfLayout = plan.orders.size();
    bool hasList = false;
    bool everyListHasArm = true;
    for (uint32_t number = 0; number < keyOrders.numberOfOrdersOf(layout); ++number) {
        std::span<const FieldIndex> order = keyOrders.order(layout, number);
        for (uint32_t count : countsAtWhichMakersStopInCopiesOwnKeys(walker, layout, order)) {
            std::span<const FieldIndex> created = order.first(count);
            bool hasArmAlready = std::ranges::any_of(plan.orders.span().subspan(firstArmOfLayout), [&](const OwnKeyOrder& earlier) {
                return std::ranges::equal(earlier.created, created);
            });
            if (hasArmAlready)
                continue;
            if (plan.orders.size() == mostArms)
                return false;
            hasList = true;
            std::optional<OwnKeyOrder> arm = keyOrders.findOrderWord(layout, created) ? ownKeysAmongCreatedFieldsInCopiesOwnKeys(walker, layout, created) : std::nullopt;
            if (arm && arm->fields.size() <= mostFieldsOfOwnKeyWalk)
                plan.orders.append(WTF::move(*arm));
            else
                everyListHasArm = false;
        }
    }
    return hasList && everyListHasArm;
}

static bool listLayoutsInCopiesOwnKeys(Walker& walker, TypeID listed, unsigned mostArms, Vector<LayoutID, mostArmsOfOwnKeyWalk>& layouts, bool& hasMoreLayouts)
{
    if (!listed)
        return false;
    Type type = walker.types().at(listed);
    if (!type.isJavaScriptValue() || type.primitives || !type.hasOnlyLayouts() || type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return false;
    for (LayoutID member : membersOf(walker.types(), type.layouts)) {
        if (!walker.plan().isInUse(member))
            continue;
        if (layouts.size() == mostArms) {
            hasMoreLayouts = true;
            break;
        }
        layouts.append(member);
    }
    return !layouts.isEmpty();
}

OwnKeyPlan planOwnKeyWalk(Walker& walker, ValueID source, Reference built)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !built || !built.isValue())
        return { };

    TypeArena& types = walker.types();
    unsigned mostArms = walker.isInLoop() ? mostArmsOfOwnKeyWalk : 1;
    TypeID proven = walker.typeOf(built);
    Vector<LayoutID, mostArmsOfOwnKeyWalk> layouts;
    bool hasMoreLayouts = false;

    OwnKeyPlan plan;
    plan.isDeclared = true;
    bool layoutsAreListedByProof = listLayoutsInCopiesOwnKeys(walker, proven, mostArms, layouts, hasMoreLayouts);
    if (!layoutsAreListedByProof) {
        ExpectationOfValue expectation = findExpectation(walker, source);
        TypeID expected = expectation.type ? types.withoutNullish(expectation.type) : TypeID();
        if (!expected || !proven || !types.overlap(proven, expected) || !listLayoutsInCopiesOwnKeys(walker, expected, mostArms, layouts, hasMoreLayouts))
            return { };
        plan.isDeclared = expectation.isDeclared;
    }
    plan.layoutsAreProven = layoutsAreListedByProof && layouts.size() == 1 && !hasMoreLayouts;

    plan.hasArmForEveryExpectedOrder = !hasMoreLayouts;
    for (LayoutID layout : layouts)
        plan.hasArmForEveryExpectedOrder &= appendOrdersOfLayoutInCopiesOwnKeys(walker, layout, mostArms, plan);
    if (plan.orders.isEmpty())
        return { };
    return plan;
}

static bool goOnWhereTestPassesInCopiesOwnKeys(Walker& walker, Reference test, BlockID whereTestFails)
{
    if (!test)
        return false;
    BlockID passed = walker.addBlock();
    walker.branch(test, passed, whereTestFails);
    walker.continueIn(passed);
    return !walker.isUnreachable() && !walker.hasFailed();
}

bool goOnWhereObjectHasOwnKeys(Walker& walker, Reference& object, const OwnKeyOrder& order, bool layoutIsProven, Origin origin, const WaysOutOfOwnKeyTests& waysOut)
{
    origin.isTailCall = false;
    if (!layoutIsProven) {
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(object);
        test.setLayouts(walker.types().at(walker.types().forLayout(order.layout)).layouts);
        Reference hasShape = walker.build(WTF::move(test));
        if (!goOnWhereTestPassesInCopiesOwnKeys(walker, hasShape, waysOut.whereLayoutDiffers))
            return false;
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(object);
        narrowed.setTestOperand(hasShape);
        narrowed.setOperandIndex(OperandIndex::ShapeTest::object);
        narrowed.setIsOnTakenEdge(true);
        object = walker.build(WTF::move(narrowed));
        if (!object)
            return false;
    }

    Draft hasOrder(Opcode::HasKeyOrder, origin);
    hasOrder.setObjectOperand(object);
    hasOrder.setLayout(order.layout);
    hasOrder.setFields(order.created.span());
    if (!goOnWhereTestPassesInCopiesOwnKeys(walker, walker.build(WTF::move(hasOrder)), waysOut.whereOrderDiffers))
        return false;

    BlockID whereGuessFails = waysOut.whereTestFails;
    walker.letGuessFailUnasked(whereGuessFails, origin);
    Draft hasNothingElse(Opcode::HasNoExtraProperties, origin);
    hasNothingElse.setObjectOperand(object);
    hasNothingElse.setHasNoFieldOutsideSlot(true);
    if (!goOnWhereTestPassesInCopiesOwnKeys(walker, walker.build(WTF::move(hasNothingElse)), waysOut.whereTestFails))
        return false;
    walker.state().noteFactsOfCell(object, FactOfCell::NoFieldOutsideSlot);
    return true;
}

bool goOnWhereObjectHasOwnKeys(Walker& walker, Reference& object, const OwnKeyOrder& order, bool layoutIsProven, Origin origin, BlockID whereTestFails)
{
    return goOnWhereObjectHasOwnKeys(walker, object, order, layoutIsProven, origin, WaysOutOfOwnKeyTests { whereTestFails, whereTestFails, whereTestFails });
}

Reference valueOfOwnKey(Walker& walker, Reference object, LayoutID layout, FieldIndex field, Origin origin)
{
    const Program& program = walker.program();
    PropertyKeyID key = walker.plan().slots().keyOf(program, { layout, field });
    FieldReference place = findField(program, layout, key).value_or(FieldReference { layout, field });

    if (walker.has(Mechanism::Folding)) {
        Reference names[] { object, object.isValue() ? Reference::value(rootOfCellInSweptCode(walker.code(), object.id())) : Reference() };
        for (Reference name : names) {
            for (FieldReference known : { place, FieldReference { layout, field } }) {
                Reference held = name ? walker.state().slot(name, known) : Reference();
                TypeID type = held ? walker.typeOf(held) : TypeID();
                if (type && !(walker.types().at(type).primitives & Primitive::Absent))
                    return held;
            }
        }
    }

    origin.isTailCall = false;
    Draft read(Opcode::GetSlot, origin);
    read.setObjectOperand(object);
    read.setKey(key);
    read.setReceivers(walker.types().at(walker.types().forLayout(layout)).layouts);
    read.setLevel(KnowledgeLevel::Proven);
    Reference value = walker.build(WTF::move(read));
    if (value)
        walker.state().noteSlot(object, place, value);
    return value;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
