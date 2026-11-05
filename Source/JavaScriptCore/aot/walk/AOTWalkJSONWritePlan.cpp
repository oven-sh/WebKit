/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkJSONWritePlan.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinPristine.h"
#include "AOTIRProgram.h"
#include "AOTWalk.h"
#include <algorithm>
#include <tuple>
#include <wtf/BitVector.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr size_t mostLayoutsTestedForOneJSONValue = 4;
static constexpr size_t mostKeyOrdersWrittenOutForOneJSONObject = 4;

static PropertyKeyID keyOfToJSONInWritePlan(const AccessContext& context)
{
    return context.program.keyOf(BuiltinNames::toJSON);
}

static bool isConstantLocationForJSONWritePlan(const AccessContext& context, BuiltinID member)
{
    const Program& program = context.program;
    const BuiltinTable& table = program.builtins();
    if (table.has(BuiltinMark::MayBeAbsent, member) || program.isUnlocked(member) || program.isUnlocked(table.canonicalID(member)))
        return false;
    if (table.at(member).mutability == BuiltinMutability::AlwaysLocked || table.isLocationOfLockedObject(member))
        return true;
    return (*context.facts).builtinLocation(member) == BuiltinLocationState::Constant;
}

static std::optional<BuiltinID> findMemberAboveForJSONWritePlan(const AccessContext& context, BuiltinID start, BuiltinName name)
{
    const Program& program = context.program;
    const BuiltinTable& table = program.builtins();
    PropertyKeyID key = program.keyOf(name);
    if (!start || !key)
        return std::nullopt;
    MembersUnderKey members(program, key);
    BuiltinID holder = table.canonicalID(start);
    while (true) {
        if (BuiltinID member = members.findOwnMember(holder)) {
            bool liesOnHolder = !table.at(member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances);
            return liesOnHolder && isConstantLocationForJSONWritePlan(context, member) ? std::optional(member) : std::nullopt;
        }
        if (!listsEveryMemberUnder(program, holder, key) || !members.isClosedKey(holder) || !table.hasLockedPrototypeLink(holder))
            return std::nullopt;
        switch (table.at(holder).prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return std::nullopt;
        case BuiltinPrototypeLink::Null:
            return BuiltinID();
        case BuiltinPrototypeLink::Builtin:
            holder = table.canonicalID(table.at(holder).inheritsFrom);
            break;
        }
    }
}

static bool findsNoToJSONFrom(const AccessContext& context, BuiltinID start)
{
    std::optional<BuiltinID> found = findMemberAboveForJSONWritePlan(context, start, BuiltinNames::toJSON);
    return found && !*found;
}

bool hasNoJSONText(const AccessContext& context, TypeID id)
{
    if (!id)
        return false;
    Type type = context.types.at(id);
    if (type.isNever() || !type.isJavaScriptValue() || (type.primitives & ~primitivesWithoutJSONText))
        return false;
    return !type.hasObjects();
}

TypeID provenTypeOfJSONMember(const AccessContext& context, LayoutID layout, uint32_t field)
{
    TypeArena& types = context.types;
    TypeID inSlot = context.plan.slot(layout, field).type;
    TypeID atDoors = context.plan.slot(layout, field).provenType;
    TypeID proven = atDoors ? atDoors : inSlot;
    return proven ? types.withoutPrimitives(proven, Primitive::Absent) : TypeID(TypeTable::anything);
}

TypeID writtenTypeOfJSONMember(const AccessContext& context, LayoutID layout, uint32_t field)
{
    const Layout& record = context.program.at(layout);
    return field < record.fields.size() ? record.fields[field].declared.expected().predicted() : TypeID();
}

JSONFieldListing listingOfJSONField(const AccessContext& context, LayoutID layout, uint32_t field)
{
    const Program& program = context.program;
    const Layout& record = program.at(layout);
    FieldFacts facts = (*context.facts).field({ layout, field });
    OptionSet<FieldForm> form = facts.formOfValue;
    PropertyKeyID key;
    if (field < record.fields.size()) {
        const Field& declared = record.fields[field];
        if (declared.computedKey)
            return JSONFieldListing::Unknown;
        if (declared.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsBrandOfMethods, FieldFlags::IsHidden }))
            return JSONFieldListing::LeftOut;
        key = declared.key;
        form.add(declared.form);
    } else
        key = context.plan.slots().keyOf(program, { layout, field });
    if (!key)
        return JSONFieldListing::Unknown;

    switch (program.at(key).kind) {
    case PropertyKeyKind::Text:
        break;
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
        return JSONFieldListing::LeftOut;
    }
    NameClass nameClass = program.nameClassOf(key);
    if (mayBeElementKey(nameClass) || nameClass == NameClass::Proto)
        return JSONFieldListing::Unknown;
    if (form.contains(FieldForm::IsAccessor) || facts.facts.contains(FieldFact::IsAccessor) || context.plan.slot(layout, field).holdsAccessor)
        return JSONFieldListing::Unknown;
    return form.contains(FieldForm::IsNotEnumerable) ? JSONFieldListing::LeftOut : JSONFieldListing::Listed;
}

static bool findParentNamedByClassRecordForJSONWritePlan(const AccessContext& context, LayoutID classLayout, LayoutID& parent)
{
    const Layout& layout = context.program.at(classLayout);
    ClassFacts facts = (*context.facts).classOf(classLayout);
    parent = { };
    if (layout.isDerived())
        parent = writtenBaseOf(layout);
    else {
        switch (facts.baseState) {
        case BaseState::Layout:
            parent = facts.base;
            break;
        case BaseState::Unknown:
            parent = writtenBaseOf(layout);
            break;
        case BaseState::None:
            parent = facts.prototypeBase;
            break;
        case BaseState::BuiltinClass:
            return false;
        }
    }
    return parent || !layout.has(LayoutFlags::HasHeritage);
}

static bool guardOfClassAnswersForToJSON(const AccessContext& context, LayoutID classLayout, PropertyKeyID key)
{
    const Program& program = context.program;
    const Facts& facts = *context.facts;
    const LinkTables& link = facts.linked();
    if (!facts.isEvaluatedAtMostOnce(classLayout))
        return false;
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID current = classLayout; current;) {
        const Layout& layout = program.at(current);
        if (!numberOfClassesLeft-- || layout.kind != LayoutKind::Instance || !layout.constructor || !context.plan.hasClassRecord(current))
            return false;
        if (layout.has(LayoutFlags::IsConstructorFunction) && program.at(layout.constructor).captures.size() != context.plan.promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor))
            return false;
        for (const ClassMember& declared : facts.membersOf(current)) {
            if (declared.side == ClassSide::Prototype && (!declared.key || declared.key == key))
                return false;
        }
        for (const LinkedMember& declared : std::ranges::equal_range(link.members, current, { }, &LinkedMember::classLayout)) {
            if (declared.side == ClassSide::Prototype && (!declared.key || declared.key == key))
                return false;
        }
        LayoutID parent;
        if (!findParentNamedByClassRecordForJSONWritePlan(context, current, parent))
            return false;
        current = parent;
    }
    return true;
}

static bool planLookupOfToJSON(const AccessContext& context, LayoutID layout, OptionSet<LayoutChange> possibleChanges, JSONObjectPlan& plan)
{
    const Program& program = context.program;
    const Layout& record = program.at(layout);
    PropertyKeyID key = keyOfToJSONInWritePlan(context);
    if (!key || record.has(LayoutFlags::IsOfCreatedObject))
        return false;
    if (context.plan.slots().findField(program, layout, key) || findField(program, layout, key))
        return false;
    if (!findsNoToJSONFrom(context, Builtins::ObjectPrototype))
        return false;

    bool staysWhereItWasMade = !possibleChanges.contains(LayoutChange::ChangesPrototype);
    if (record.kind == LayoutKind::Record) {
        if (record.constructor || record.has(LayoutFlags::IsConstructorFunction))
            return false;
        if (!staysWhereItWasMade)
            plan.looksAtCell.add(FactOfCell::HasLayoutPrototype);
        return true;
    }
    if (staysWhereItWasMade && (*context.facts).linked().findMember(program, layout, ClassSide::Prototype, key).kind == MemberFound::Kind::Nothing)
        return true;

    LayoutID classLayout = declaredLayoutOf(program, layout);
    if (!guardOfClassAnswersForToJSON(context, classLayout, key))
        return false;
    plan.looksAtCell.add(FactOfCell::HasLayoutPrototype);
    plan.classesGuardedAgainstToJSON.append(classLayout);
    return true;
}

static bool everyFieldOfJSONObjectStaysInSlot(const AccessContext& context, LayoutID layout)
{
    TypeArena& types = context.types;
    const Layout& record = context.program.at(layout);
    unsigned numberOfFields = context.plan.layout(layout).numberOfFields();
    for (uint32_t field = 0; field < numberOfFields; ++field) {
        const PlacedSlot& slot = context.plan.slot(layout, field);
        if (!slot.hasStorage() || slot.holdsAccessor || (field < record.fields.size() && !record.fields[field].form.isEmpty()))
            return false;
        if (slot.representation == SlotRepresentation::JSValue && (!slot.type || types.at(slot.type).isAnything()))
            continue;
        if (!slot.type || !slot.everyStoreFits)
            return false;
    }
    return true;
}

bool planLooksAtJSONObject(const AccessContext& context, LayoutID layout, JSONObjectPlan& plan)
{
    const Layout& record = context.program.at(layout);
    if (record.kind != LayoutKind::Record && record.kind != LayoutKind::Instance)
        return false;
    if (!isFoundByShapeTest(record) || !context.plan.slots().holdsCompiledObjects(layout))
        return false;

    const LinkTables& link = (*context.facts).linked();
    OptionSet<LayoutChange> possibleChanges = layout.index() < link.layouts.size() ? link.layout(layout).possibleChanges : LinkedLayout { }.possibleChanges;
    if (!planLookupOfToJSON(context, layout, possibleChanges, plan))
        return false;
    if (possibleChanges.containsAny({ LayoutChange::GainsProperty, LayoutChange::GetsAccessor }) || !everyFieldOfJSONObjectStaysInSlot(context, layout))
        plan.looksAtCell.add({ FactOfCell::NoFieldOutsideSlot, FactOfCell::OrderWordIsKnown });
    return true;
}

static std::optional<JSONKeyOrderPlan> planKeyOrderOfJSONObject(const AccessContext& context, LayoutID layout, std::span<const FieldIndex> created)
{
    if (!context.plan.keyOrders().findOrderWord(layout, created))
        return std::nullopt;
    JSONKeyOrderPlan order;
    for (FieldIndex field : created) {
        order.created.append(field);
        switch (listingOfJSONField(context, layout, field)) {
        case JSONFieldListing::Unknown:
            return std::nullopt;
        case JSONFieldListing::LeftOut:
            continue;
        case JSONFieldListing::Listed:
            break;
        }
        const PlacedSlot& slot = context.plan.slot(layout, field);
        if (!slot.hasStorage() || (slot.isOutOfLine() && slot.mayExistWithoutStorage))
            return std::nullopt;
        if (slot.deferredFunction || slot.representation == SlotRepresentation::DeferredClosure) {
            if (!hasNoJSONText(context, provenTypeOfJSONMember(context, layout, field)))
                return std::nullopt;
            continue;
        }
        PropertyKeyID key = context.plan.slots().keyOf(context.program, { layout, field });
        if (!key || !context.types.at(key).text)
            return std::nullopt;
        order.members.append(JSONMemberPlan { key, field });
    }
    return order;
}

namespace {

struct StopsInKeyOrderOfJSONObject {
    uint32_t least { 0 };
    uint32_t most { 0 };
};

} // anonymous namespace

static std::optional<StopsInKeyOrderOfJSONObject> findStopsInKeyOrderOfJSONObject(const AccessContext& context, LayoutID layout, std::span<const FieldIndex> order)
{
    const Layout& record = context.program.at(layout);
    const LinkTables& link = (*context.facts).linked();
    StopsInKeyOrderOfJSONObject stops { 0, static_cast<uint32_t>(order.size()) };
    bool hasPassedFieldThatNobodyCreates = false;
    for (uint32_t place = 0; place < order.size(); ++place) {
        FieldIndex field = order[place];
        bool isRequired = false;
        if (field < record.fields.size()) {
            const Field& declared = record.fields[field];
            bool liesAboveInstances = record.kind == LayoutKind::Instance && (declared.flags.contains(FieldFlags::IsMethod) || declared.form.contains(FieldForm::IsAccessor));
            if (liesAboveInstances || declared.flags.contains(FieldFlags::IsNeverWritten)) {
                if (!hasPassedFieldThatNobodyCreates)
                    stops.most = place;
                hasPassedFieldThatNobodyCreates = true;
                continue;
            }
            isRequired = declared.flags.contains(FieldFlags::IsRequired);
        } else
            isRequired = link.isAppendedFieldRequiredBySomeWrittenType({ layout, field });
        if (!isRequired)
            continue;
        if (hasPassedFieldThatNobodyCreates)
            return std::nullopt;
        stops.least = place + 1;
    }
    return stops;
}

static bool planKeyOrdersOfJSONObject(const AccessContext& context, LayoutID layout, Vector<JSONKeyOrderPlan, 2>& planned)
{
    const KeyOrders& keyOrders = context.plan.keyOrders();
    uint32_t numberOfOrders = keyOrders.numberOfOrdersOf(layout);
    auto isPlanned = [&](std::span<const FieldIndex> created) {
        return planned.containsIf([&](const JSONKeyOrderPlan& other) {
            return std::ranges::equal(other.created, created);
        });
    };

    for (uint32_t number = 0; number < numberOfOrders; ++number) {
        std::span<const FieldIndex> order = keyOrders.order(layout, number);
        std::optional<StopsInKeyOrderOfJSONObject> stops = findStopsInKeyOrderOfJSONObject(context, layout, order);
        if (!stops || isPlanned(order.first(stops->most)))
            continue;
        if (planned.size() == mostKeyOrdersWrittenOutForOneJSONObject)
            return false;
        std::optional<JSONKeyOrderPlan> longest = planKeyOrderOfJSONObject(context, layout, order.first(stops->most));
        if (!longest)
            return false;
        planned.append(WTF::move(*longest));
    }
    for (uint32_t number = 0; number < numberOfOrders; ++number) {
        std::span<const FieldIndex> order = keyOrders.order(layout, number);
        std::optional<StopsInKeyOrderOfJSONObject> stops = findStopsInKeyOrderOfJSONObject(context, layout, order);
        if (!stops)
            continue;
        for (uint32_t count = stops->least; count < stops->most && planned.size() < mostKeyOrdersWrittenOutForOneJSONObject; ++count) {
            if (isPlanned(order.first(count)))
                continue;
            if (std::optional<JSONKeyOrderPlan> shorter = planKeyOrderOfJSONObject(context, layout, order.first(count)))
                planned.append(WTF::move(*shorter));
        }
    }
    return !planned.isEmpty();
}

std::optional<JSONObjectPlan> planJSONObject(const AccessContext& context, LayoutID layout)
{
    JSONObjectPlan plan;
    plan.layout = layout;
    if (!planLooksAtJSONObject(context, layout, plan))
        return std::nullopt;

    const PlannedLayout& planned = context.plan.layout(layout);
    if (context.plan.shapes().shapesOf(layout).count != 1)
        return std::nullopt;
    for (uint32_t field = 0; field < planned.numberOfFields(); ++field) {
        PropertyKeyID key = context.plan.slots().keyOf(context.program, { layout, field });
        if (key && context.program.at(key).kind == PropertyKeyKind::Text && context.program.isIndexLike(key))
            return std::nullopt;
    }
    if (!planKeyOrdersOfJSONObject(context, layout, plan.orders))
        return std::nullopt;
    return plan;
}

static auto rankOfLayoutWrittenAsJSON(const AccessContext& context, const JSONObjectPlan& plan)
{
    const Layout& record = context.program.at(plan.layout);
    uint32_t members = 0;
    uint32_t membersWithoutText = 0;
    for (const JSONKeyOrderPlan& order : plan.orders) {
        if (order.members.size() < members)
            continue;
        members = order.members.size();
        membersWithoutText = 0;
        for (const JSONMemberPlan& member : order.members) {
            if (hasNoJSONText(context, provenTypeOfJSONMember(context, plan.layout, member.field)))
                ++membersWithoutText;
        }
    }
    bool isOfLibrary = record.module && context.program.at(record.module).flags.contains(ModuleFlags::IsLibrary);
    uint32_t module = record.module ? record.module.index() + 1 : 0;
    return std::tuple { isOfLibrary, record.kind == LayoutKind::Instance, membersWithoutText, members, module, record.position, plan.layout.index() };
}

static JSONValuePlan planLayoutsWrittenAsJSON(const AccessContext& context, TypeID objects)
{
    using Rank = decltype(rankOfLayoutWrittenAsJSON(context, std::declval<const JSONObjectPlan&>()));
    Vector<Rank, 8> ranked;
    Vector<LayoutID, 8> layouts;
    layouts.append(membersOf(context.types, context.types.at(objects)));
    for (LayoutID layout : layouts) {
        if (!context.plan.isInUse(layout))
            continue;
        if (std::optional<JSONObjectPlan> plan = planJSONObject(context, layout))
            ranked.append(rankOfLayoutWrittenAsJSON(context, *plan));
    }
    if (ranked.isEmpty())
        return { };
    std::ranges::sort(ranked);
    if (ranked.size() > mostLayoutsTestedForOneJSONValue)
        ranked.shrink(mostLayoutsTestedForOneJSONValue);

    JSONValuePlan plan;
    plan.kind = JSONValueKind::Objects;
    for (const Rank& rank : ranked)
        plan.layouts.append(LayoutID(std::get<6>(rank)));
    return plan;
}

static bool pristineInstanceFindsForJSON(const AccessContext& context, BuiltinClassID builtinClass, BuiltinName name, bool findsNothing, PristineAspectSet& aspects)
{
    const BuiltinTable& builtins = context.program.builtins();
    if (builtins.isRedefinedBelow(builtinClass, name))
        return false;
    std::optional<BuiltinID> found = findMemberAboveForJSONWritePlan(context, builtins.at(builtinClass).prototype, name);
    if (!found)
        return false;
    if (findsNothing)
        return !*found;
    if (!*found)
        return false;
    aspects.add(aspectThatFindsMember(builtins, builtinClass, *found));
    return true;
}

static JSONValuePlan planBuiltinClassWrittenAsJSON(const AccessContext& context, const ClassPart& part)
{
    TypeArena& types = context.types;
    const BuiltinTable& builtins = context.program.builtins();
    BuiltinClassID builtinClass = part.builtinClass;
    if (!builtins.mayHavePristineInstances(builtinClass))
        return { };
    switch (pristineTestOf(builtins, builtinClass)) {
    case PristineTest::NeverPasses:
        return { };
    case PristineTest::InstanceStructure:
        if (builtinClass == BuiltinClasses::Array)
            return { };
        break;
    case PristineTest::MayPass:
    case PristineTest::ClosureAsCreated:
        break;
    }

    JSONValuePlan plan;
    plan.tested = types.testablePartOf(types.builtinClass(builtinClass));
    if (!types.isSubset(plan.tested, types.builtinClass(builtinClass)))
        return { };
    PristineAspectSet none;
    if (builtinClass == BuiltinClasses::Array && pristineInstanceFindsForJSON(context, builtinClass, BuiltinNames::toJSON, true, none)) {
        TypeID writtenElement = part.expectedContentsAt(builtins, ContentsSlot::Element).predicted();
        plan.kind = JSONValueKind::Array;
        plan.element = writtenElement ? writtenElement : TypeID(TypeTable::anything);
        return plan;
    }
    return { };
}

static JSONValuePlan scalarPlanOfJSON(Opcode row, TypeID tested)
{
    JSONValuePlan plan;
    plan.kind = JSONValueKind::Scalar;
    plan.row = row;
    plan.tested = tested;
    return plan;
}

static JSONValuePlan planDefinedJSONType(const AccessContext& context, TypeID id)
{
    TypeArena& types = context.types;
    Type defined = types.at(id);
    if (defined.isBoolean())
        return scalarPlanOfJSON(Opcode::JSONAppendBoolean, TypeTable::boolean);
    if (defined.isInteger() && defined.integers.isSubsetOf(Range::exact()))
        return scalarPlanOfJSON(Opcode::JSONAppendInteger, defined.isInt32() ? TypeID(TypeTable::int32) : types.integer(Range::exact()));
    if (defined.isNumber())
        return scalarPlanOfJSON(Opcode::JSONAppendNumber, TypeTable::number);
    if (defined.isAnyString())
        return scalarPlanOfJSON(Opcode::JSONAppendString, TypeTable::string);

    JSONValuePlan plan;
    plan.kind = JSONValueKind::ObjectParts;
    Vector<ClassPart, 4> classes;
    classes.append(classPartsOf(types, defined));
    for (const ClassPart& part : classes) {
        if (JSONValuePlan instances = planBuiltinClassWrittenAsJSON(context, part); instances.kind != JSONValueKind::Generic)
            plan.parts.append(WTF::move(instances));
    }
    if (defined.layouts) {
        if (JSONValuePlan layouts = planLayoutsWrittenAsJSON(context, id); layouts.kind != JSONValueKind::Generic)
            plan.parts.append(WTF::move(layouts));
    }
    if (plan.parts.size() == 1)
        return plan.parts.takeLast();
    return plan.parts.isEmpty() ? JSONValuePlan { } : WTF::move(plan);
}

static bool removePartsOfJSONPlanThatCannotPass(const AccessContext& context, JSONValuePlan& expected, TypeID defined)
{
    TypeArena& types = context.types;
    auto canNarrowTo = [&](TypeID tested) {
        TypeID narrowed = types.intersectionOf(defined, tested);
        return !types.at(narrowed).isNever() && types.isSubset(narrowed, tested);
    };
    switch (expected.kind) {
    case JSONValueKind::Generic:
    case JSONValueKind::Nullish:
        return false;
    case JSONValueKind::Objects:
        expected.layouts.removeAllMatching([&](LayoutID layout) {
            return !canNarrowTo(types.forLayout(layout));
        });
        return !expected.layouts.isEmpty();
    case JSONValueKind::ObjectParts:
        expected.parts.removeAllMatching([&](JSONValuePlan& part) {
            return !removePartsOfJSONPlanThatCannotPass(context, part, defined);
        });
        return !expected.parts.isEmpty();
    case JSONValueKind::Scalar:
    case JSONValueKind::Array:
        return expected.tested && canNarrowTo(expected.tested);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSONValuePlan planJSONValue(const AccessContext& context, TypeID proven, TypeID expected)
{
    TypeArena& types = context.types;
    JSONValuePlan plan;
    if (!proven || !types.at(proven).isJavaScriptValue() || types.at(proven).mayBeUnspecified())
        return plan;

    TypeID defined = types.withoutNullish(proven);
    bool mayBeNullish = defined != proven;
    if (types.at(defined).isNever()) {
        plan.kind = mayBeNullish ? JSONValueKind::Nullish : JSONValueKind::Generic;
        plan.mayBeNullish = mayBeNullish;
        return plan;
    }

    plan = planDefinedJSONType(context, defined);
    JSONValuePlan expectedPlan;
    if (expected && (plan.kind == JSONValueKind::Generic || plan.kind == JSONValueKind::Array)) {
        TypeID expectedAndDefined = types.withoutNullish(expected);
        if (!types.at(expectedAndDefined).isNever())
            expectedPlan = planDefinedJSONType(context, expectedAndDefined);
    }

    if (plan.kind == JSONValueKind::Array && expectedPlan.kind == JSONValueKind::Array) {
        TypeID element = types.intersectionOf(plan.element, expectedPlan.element);
        if (!types.at(element).isNever())
            plan.element = element;
    }
    if (plan.kind == JSONValueKind::Generic && removePartsOfJSONPlanThatCannotPass(context, expectedPlan, defined)) {
        plan = WTF::move(expectedPlan);
        plan.isExpected = true;
    }
    plan.mayBeNullish = mayBeNullish;
    return plan;
}

static TypeID typeCoveredByJSONPlan(const AccessContext& context, const JSONValuePlan& plan)
{
    TypeArena& types = context.types;
    switch (plan.kind) {
    case JSONValueKind::Generic:
    case JSONValueKind::Nullish:
        return TypeTable::never;
    case JSONValueKind::Scalar:
    case JSONValueKind::Array:
        return plan.tested;
    case JSONValueKind::Objects: {
        TypeID covered = TypeTable::never;
        for (LayoutID layout : plan.layouts)
            covered = types.unionOf(covered, types.forLayout(layout));
        return covered;
    }
    case JSONValueKind::ObjectParts: {
        TypeID covered = TypeTable::never;
        for (const JSONValuePlan& part : plan.parts)
            covered = types.unionOf(covered, typeCoveredByJSONPlan(context, part));
        return covered;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool holdsListInJSONWritePlan(const JSONValuePlan& plan)
{
    return plan.kind == JSONValueKind::Array || (plan.kind == JSONValueKind::ObjectParts && std::ranges::any_of(plan.parts, holdsListInJSONWritePlan));
}

bool holdsListAmongJSONMembers(const AccessContext& context, const JSONObjectPlan& plan)
{
    auto someIsList = [&](std::span<const JSONMemberPlan> members) {
        return std::ranges::any_of(members, [&](const JSONMemberPlan& member) {
            TypeID proven = provenTypeOfJSONMember(context, plan.layout, member.field);
            return !hasNoJSONText(context, proven) && holdsListInJSONWritePlan(planJSONValue(context, proven, writtenTypeOfJSONMember(context, plan.layout, member.field)));
        });
    };
    return (plan.membersInAnyOrder && someIsList(plan.membersInAnyOrder->span())) || std::ranges::any_of(plan.orders, [&](const JSONKeyOrderPlan& order) {
        return someIsList(order.members.span());
    });
}

uint32_t numberOfJSONMembersWrittenOutFor(const AccessContext& context, const JSONObjectPlan& plan)
{
    auto numberWithText = [&](std::span<const JSONMemberPlan> members) {
        return static_cast<uint32_t>(std::ranges::count_if(members, [&](const JSONMemberPlan& member) {
            return !hasNoJSONText(context, provenTypeOfJSONMember(context, plan.layout, member.field));
        }));
    };
    uint32_t count = plan.membersInAnyOrder ? numberWithText(plan.membersInAnyOrder->span()) : 0;
    for (const JSONKeyOrderPlan& order : plan.orders)
        count += numberWithText(order.members.span());
    return count;
}

namespace {

struct JSONUnitGoneThrough {
    Vector<LayoutID, 4> layoutsAround;
    uint32_t membersLeft { mostJSONMembersWrittenOutInOneUnit };
};

struct JSONSerializersCalled {
    void add(LayoutID layout)
    {
        if (isListed.get(layout.index()))
            return;
        isListed.set(layout.index());
        layouts.append(layout);
    }

    Vector<LayoutID, 4> layouts;
    BitVector isListed;
};

} // anonymous namespace

static bool slotsProvePlansOfListedJSONObjects(const AccessContext& context, const JSONValuePlan& plan, BitVector& layoutsAsked)
{
    switch (plan.kind) {
    case JSONValueKind::Generic:
    case JSONValueKind::Nullish:
    case JSONValueKind::Scalar:
        return true;
    case JSONValueKind::Array:
        return slotsProvePlansOfListedJSONObjects(context, planJSONValue(context, plan.element, { }), layoutsAsked);
    case JSONValueKind::ObjectParts:
        return std::ranges::all_of(plan.parts, [&](const JSONValuePlan& part) {
            return slotsProvePlansOfListedJSONObjects(context, part, layoutsAsked);
        });
    case JSONValueKind::Objects:
        break;
    }

    return std::ranges::all_of(plan.layouts, [&](LayoutID layout) {
        if (!layoutsAsked.add(layout.index()))
            return true;
        std::optional<JSONObjectPlan> object = planJSONObject(context, layout);
        if (!object)
            return false;
        auto slotsProvePlans = [&](std::span<const JSONMemberPlan> members) {
            return std::ranges::all_of(members, [&](const JSONMemberPlan& member) {
                TypeID proven = provenTypeOfJSONMember(context, layout, member.field);
                if (hasNoJSONText(context, proven))
                    return true;
                JSONValuePlan memberPlan = planJSONValue(context, proven, { });
                return memberPlan.kind != JSONValueKind::Generic && slotsProvePlansOfListedJSONObjects(context, memberPlan, layoutsAsked);
            });
        };
        return std::ranges::all_of(object->orders, [&](const JSONKeyOrderPlan& order) {
            return slotsProvePlans(order.members.span());
        }) && (!object->membersInAnyOrder || slotsProvePlans(object->membersInAnyOrder->span()));
    });
}

bool repeatsJSONListAround(std::span<const JSONListAround> listsAround, const JSONListAround& list)
{
    return std::ranges::contains(listsAround, list);
}

static bool listsFoundInJSONListNeverEnd(const AccessContext& context, TypeID lists, TypeID tested)
{
    TypeArena& types = context.types;
    Vector<JSONListAround, 4> listsAround;
    for (TypeID list = types.intersectionOf(lists, tested); !types.at(list).isNever();) {
        JSONListAround found { list, { } };
        if (repeatsJSONListAround(listsAround.span(), found))
            return true;
        listsAround.append(found);
        const ClassPart* part = singleBuiltinClassOf(types, types.at(list));
        TypeID elements = part ? part->expectedContentsAt(context.program.builtins(), ContentsSlot::Element).predicted() : TypeID();
        if (!elements)
            return false;
        list = types.intersectionOf(elements, tested);
    }
    return false;
}

static bool writesWholeJSONValueInUnit(const AccessContext&, const JSONValuePlan&, TypeID typeOfPlan, JSONUnitGoneThrough&, JSONSerializersCalled&, BitVector& layoutsAskedInLists);

static bool writesMembersOfJSONObjectInUnit(const AccessContext& context, const JSONObjectPlan& object, JSONUnitGoneThrough& unit, JSONSerializersCalled& serializers, BitVector& layoutsAskedInLists)
{
    auto writesMembers = [&](std::span<const JSONMemberPlan> members) {
        return std::ranges::all_of(members, [&](const JSONMemberPlan& member) {
            TypeID proven = provenTypeOfJSONMember(context, object.layout, member.field);
            if (hasNoJSONText(context, proven))
                return true;
            TypeID written = writtenTypeOfJSONMember(context, object.layout, member.field);
            JSONValuePlan memberPlan = planJSONValue(context, proven, written);
            return writesWholeJSONValueInUnit(context, memberPlan, memberPlan.isExpected ? written : proven, unit, serializers, layoutsAskedInLists);
        });
    };
    unit.layoutsAround.append(object.layout);
    bool writesAll = std::ranges::all_of(object.orders, [&](const JSONKeyOrderPlan& order) {
        return writesMembers(order.members.span());
    });
    writesAll = writesAll && (!object.membersInAnyOrder || writesMembers(object.membersInAnyOrder->span()));
    unit.layoutsAround.removeLast();
    return writesAll;
}

static bool writesWholeJSONValueInUnit(const AccessContext& context, const JSONValuePlan& plan, TypeID typeOfPlan, JSONUnitGoneThrough& unit, JSONSerializersCalled& serializers, BitVector& layoutsAskedInLists)
{
    TypeArena& types = context.types;
    if (types.at(typeOfPlan).primitives & Primitive::BigInt)
        return false;
    TypeID objectsWithoutPlan = types.differenceOf(types.withOnlyObjects(typeOfPlan), typeCoveredByJSONPlan(context, plan));
    if (!types.at(objectsWithoutPlan).isNever() && !hasNoJSONText(context, objectsWithoutPlan))
        return false;

    switch (plan.kind) {
    case JSONValueKind::Generic:
    case JSONValueKind::Nullish:
    case JSONValueKind::Scalar:
        return true;
    case JSONValueKind::Array: {
        JSONValuePlan element = planJSONValue(context, plan.element, { });
        if (!slotsProvePlansOfListedJSONObjects(context, element, layoutsAskedInLists))
            return false;
        if (listsFoundInJSONListNeverEnd(context, plan.element, plan.tested))
            return false;
        return writesWholeJSONValueInUnit(context, element, plan.element, unit, serializers, layoutsAskedInLists);
    }
    case JSONValueKind::ObjectParts:
        return std::ranges::all_of(plan.parts, [&](const JSONValuePlan& part) {
            return writesWholeJSONValueInUnit(context, part, typeCoveredByJSONPlan(context, part), unit, serializers, layoutsAskedInLists);
        });
    case JSONValueKind::Objects:
        break;
    }

    for (LayoutID layout : plan.layouts) {
        std::optional<JSONObjectPlan> object = planJSONObject(context, layout);
        if (!object)
            return false;
        uint32_t members = numberOfJSONMembersWrittenOutFor(context, *object);
        if (unit.layoutsAround.contains(layout) || members > unit.membersLeft) {
            if (!context.plan.conventions().serializerOf(layout))
                return false;
            serializers.add(layout);
            continue;
        }
        unit.membersLeft -= members;
        if (!writesMembersOfJSONObjectInUnit(context, *object, unit, serializers, layoutsAskedInLists))
            return false;
    }
    return true;
}

bool writesWholeJSONValue(const AccessContext& context, const JSONValuePlan& plan, TypeID typeOfPlan, bool& callsSerializers)
{
    JSONSerializersCalled serializers;
    JSONUnitGoneThrough unitOfCall;
    BitVector layoutsAskedInLists;
    if (!writesWholeJSONValueInUnit(context, plan, typeOfPlan, unitOfCall, serializers, layoutsAskedInLists))
        return false;
    for (size_t index = 0; index < serializers.layouts.size(); ++index) {
        std::optional<JSONObjectPlan> object = planJSONObject(context, serializers.layouts[index]);
        if (!object)
            return false;
        JSONUnitGoneThrough serializer { { }, roomForJSONMembersInsideObjectOfSerializer(context, *object) };
        if (!writesMembersOfJSONObjectInUnit(context, *object, serializer, serializers, layoutsAskedInLists))
            return false;
    }
    callsSerializers = !serializers.layouts.isEmpty();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
