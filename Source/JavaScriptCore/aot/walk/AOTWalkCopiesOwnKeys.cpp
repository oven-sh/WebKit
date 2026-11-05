/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations, the walk over own keys: which keys an object has, and in which order it got them, is one number in its cell. Whoever copies or lists an object without the run time asks here which number to expect.
// The state knows the number (the object was made here, or the number was tested): no test. Layouts are proven or expected: an arm a layout and order, behind tests whose failing edges lead to the operation as written.
// At a suspension and at a throw: nothing is kept, the state ends what it knows by the effects of what is built. Withheld: nothing is in sight, and every plan is empty.

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
    // Whether a walk meets the field, or what it would find in the slot, is not known when the program is built.
    Unknown,
    // The property is there, and no walk over enumerable string keys meets it.
    LeftOut,
    Listed,
};

} // anonymous namespace

// An allocation of the layout that lists nothing leaves the word of no keys, and from there on the word of a cell says which keys it has got. An order is numbered within its shape.
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
    // Where only definitions fill the field, the property has the form that they give it, whatever is written.
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
    // The key is listed, and the slot holds the pair of functions, which a walk over the slots would take for the value.
    if (form.contains(FieldForm::IsAccessor) || fieldFacts.facts.contains(FieldFact::IsAccessor) || walker.plan().slot(layout, field).holdsAccessor)
        return FieldInOwnKeyPlan::Unknown;
    return FieldInOwnKeyPlan::Listed;
}

static bool canPlanOwnKeysOfLayoutInCopiesOwnKeys(const Walker& walker, LayoutID layout)
{
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    // The instances of a class below a built-in class are cells of that class. They have no slots and no word for the order of their keys.
    if (!isFoundByShapeTest(program.at(layout)) || !plan.isInUse(layout) || !orderWordTellsOwnKeysOf(walker, layout))
        return false;

    const PlannedLayout& planned = plan.layout(layout);
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        // A slot without an empty state counts as filled from the allocation on, and no order lists it.
        if (planned.slots[field].hasStorage() && !canLeaveSlot(planned.slots[field].representation))
            return false;
        // An index is listed in front of every name, in ascending order, whenever it was created.
        PropertyKeyID key = plan.slots().keyOf(program, { layout, field });
        if (key && program.at(key).kind == PropertyKeyKind::Text && (program.isIndexLike(key) || program.nameClassOf(key) == NameClass::Proto))
            return false;
    }
    return true;
}

// `created`: an object has these fields with storage, got them in this order and has no other.
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
            // The order may list such a field while nothing holds its value, so that the object has it does not say that its slot can be read.
            const PlacedSlot& slot = walker.plan().slot(layout, field);
            if (!slot.hasStorage() || (slot.isOutOfLine() && slot.mayExistWithoutStorage))
                return std::nullopt;
            // A closure that is made on demand lies there as a mark until somebody asks for it, and a GetSlot asks nobody.
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

// ---- Objects in sight

// The state knows an object that was made here under the record that made it, and one that has passed a test under the name that the test gave it.
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
    LayoutID layout = known ? onlyLayoutProvenInCopiesOwnKeys(walker, walker.typeOf(known)) : LayoutID();
    if (!layout || !canPlanOwnKeysOfLayoutInCopiesOwnKeys(walker, layout))
        return std::nullopt;

    // The word is known whole and has no flag: the object lists these keys, one behind the other, each lies in its slot, and it has no property but those.
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

// ---- Plans

static bool isRequiredInCopiesOwnKeys(const Walker& walker, LayoutID layout, FieldIndex field)
{
    const Layout& record = walker.program().at(layout);
    if (field < record.fields.size())
        return record.fields[field].flags.contains(FieldFlags::IsRequired);
    return walker.facts().linked().isAppendedFieldRequiredBySomeWrittenType({ layout, field });
}

// At how many keys of the order, from the first on, the makers of the program are likely to stop, the likeliest first. A guess about an object, which HasKeyOrder tests.
// Outside a loop: at the end of an order that lists every field of a layout whose fields are all required. In a loop: at the end of any order, and behind its last required field.
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
        uint32_t numberOfFieldsWithStorage = 0;
        bool everyFieldIsRequired = true;
        for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
            if (!planned.slots[field].hasStorage())
                continue;
            ++numberOfFieldsWithStorage;
            everyFieldIsRequired &= isRequiredInCopiesOwnKeys(walker, layout, field);
        }
        if (everyFieldIsRequired && whole == numberOfFieldsWithStorage)
            counts.append(whole);
        return counts;
    }
    counts.append(whole);
    if (behindLastRequiredField && behindLastRequiredField != whole)
        counts.append(behindLastRequiredField);
    return counts;
}

// Appends an arm for each list of keys at which the makers of the layout's objects stop. False: some such list has no arm, or there is none.
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
            // Orders that begin alike share the word of the first of them up to where they part.
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

// The layouts in use that the type lists, as many as there may be arms. False: the type admits values that have no layout, and no arm takes them.
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
    // Every arm stands behind a test of ours, whatever is proven of the layout: which keys an object has by now is a guess about its state.
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
        // Undefined and null fail the test of the shape like any other value that nobody expects, and the operation as written says what becomes of them.
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

// ---- The tests of an arm

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

    // An extra key stands among the fields where it was created, and a field that was redefined or that got a value its slot cannot hold lies among the extra properties in whatever form it has now.
    Draft hasNothingElse(Opcode::HasNoExtraProperties, origin);
    hasNothingElse.setObjectOperand(object);
    hasNothingElse.setHasNoFieldOutsideSlot(true);
    if (!goOnWhereTestPassesInCopiesOwnKeys(walker, walker.build(WTF::move(hasNothingElse)), waysOut.whereTestFails))
        return false;
    // A frozen object is read like any other, so nobody has asked whether it is one: the whole word, which WalkState::noteOrderWord() speaks of, is not known.
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
