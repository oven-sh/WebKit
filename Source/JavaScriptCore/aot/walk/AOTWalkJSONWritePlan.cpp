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

// Each layout of a union gets a test of its shape and the text of its members written out behind it, at every call.
static constexpr size_t mostLayoutsTestedForOneJSONValue = 4;
// Each order of a layout gets a test of the order word and the text of its members written out behind it.
static constexpr size_t mostKeyOrdersWrittenOutForOneJSONObject = 4;

static PropertyKeyID keyOfToJSONInWritePlan(const AccessContext& context)
{
    return context.program.keyOf(BuiltinNames::toJSON);
}

// ---- What the table of built-ins and the locks of the build say

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

// What a read under the name finds from `start` upward in every run. Null: nothing lies there, and nothing ever will. No answer: nobody knows.
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

// ---- What has no text

// A class object inherits from the class that it extends, which may have a static method of the name. The function objects of coroutines have prototypes of their own.
static bool findsNoToJSONUnderFunction(const AccessContext& context, FunctionID function)
{
    if (classOfClassObject(context.program, function) || context.program.at(function).isCoroutine())
        return false;
    if (!context.plan.closuresStayAsCreated(function))
        return false;
    return findsNoToJSONFrom(context, Builtins::FunctionPrototype);
}

bool hasNoJSONText(const AccessContext& context, TypeID id)
{
    if (!id)
        return false;
    Type type = context.types.at(id);
    if (type.isNever() || !type.isJavaScriptValue() || (type.primitives & ~primitivesWithoutJSONText))
        return false;
    if (!type.hasObjects())
        return true;
    if (!context.types.at(context.types.withOnlyObjects(id)).hasOnlyClosures())
        return false;
    // A copy: a list of the thread's arena need not outlive what is asked while it is gone through.
    Vector<FunctionID, 4> functions;
    forEachFunctionOf(context.types, type, scopedLambda<void(FunctionID)>([&](FunctionID function) {
        functions.append(function);
    }));
    return std::ranges::all_of(functions, [&](FunctionID function) {
        return findsNoToJSONUnderFunction(context, function);
    });
}

// ---- Objects

TypeID provenTypeOfJSONMember(const AccessContext& context, LayoutID layout, uint32_t field)
{
    TypeArena& types = context.types;
    TypeID inSlot = context.plan.slot(layout, field).type;
    const LinkedCell* cell = (*context.facts).linked().findCellOfField({ layout, field });
    TypeID arrives = cell && cell->everyWriterIsKnown ? cell->proven : TypeID();
    TypeID proven = inSlot && arrives ? types.intersectionOf(inSlot, arrives) : inSlot ? inSlot : arrives;
    // The order of the keys lists the field, so its slot is filled.
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
    // An index is listed in front of every name, and `__proto__` is an accessor of Object.prototype.
    NameClass nameClass = program.nameClassOf(key);
    if (mayBeElementKey(nameClass) || nameClass == NameClass::Proto)
        return JSONFieldListing::Unknown;
    if (form.contains(FieldForm::IsAccessor) || facts.facts.contains(FieldFact::IsAccessor) || context.plan.slot(layout, field).holdsAccessor)
        return JSONFieldListing::Unknown;
    return form.contains(FieldForm::IsNotEnumerable) ? JSONFieldListing::LeftOut : JSONFieldListing::Listed;
}

// parentNamedByClassRecord() of image/AOTClassRecords.cpp. False: above the class stands what no record of the program describes.
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

// whoAnswersForWhatStandsAboveObjectsOf() of AOTWalkTypedAccesses.cpp, of a read. While the guard of the class and the name holds, each prototype object from that of the class up to the last that the records name
// has nothing under the name beside what its record declares. So no record on the way may declare a member under the name, or under a key that nobody knows.
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

// Above a record stands Object.prototype alone. Above an instance stand the members of its row of classes, and then Object.prototype. What Object.create() makes has whatever it was handed above it.
static bool planLookupOfToJSON(const AccessContext& context, LayoutID layout, OptionSet<LayoutChange> possibleChanges, JSONObjectPlan& plan)
{
    const Program& program = context.program;
    const Layout& record = program.at(layout);
    PropertyKeyID key = keyOfToJSONInWritePlan(context);
    if (!key || record.has(LayoutFlags::IsOfCreatedObject))
        return false;
    // A row of classes may declare more fields than a layout lists.
    if (context.plan.slots().findField(program, layout, key) || findField(program, layout, key))
        return false;
    // A test run may open the name on it.
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

// No store moves a field out to the extra properties, or creates it there.
static bool everyFieldOfJSONObjectStaysInSlot(const AccessContext& context, LayoutID layout)
{
    TypeArena& types = context.types;
    const Layout& record = context.program.at(layout);
    const LinkTables& link = (*context.facts).linked();
    unsigned numberOfFields = context.plan.layout(layout).numberOfFields();
    for (uint32_t field = 0; field < numberOfFields; ++field) {
        const PlacedSlot& slot = context.plan.slot(layout, field);
        // A store by name creates a field that is declared with other attributes beside its slot, with no descriptor in sight, and the run time lets it: mutationsRuledOutForShapeTest(). A field without storage has nowhere else to lie.
        if (!slot.hasStorage() || slot.holdsAccessor || (field < record.fields.size() && !record.fields[field].form.isEmpty()))
            return false;
        if (slot.representation == SlotRepresentation::JSValue && (!slot.type || types.at(slot.type).isAnything()))
            continue;
        const LinkedCell* cell = link.findCellOfField({ layout, field });
        if (!cell || !cell->everyWriterIsKnown || !cell->proven || !slot.type || !types.isSubset(cell->proven, slot.type))
            return false;
    }
    return true;
}

bool planLooksAtJSONObject(const AccessContext& context, LayoutID layout, JSONObjectPlan& plan)
{
    const Layout& record = context.program.at(layout);
    if (record.kind != LayoutKind::Record && record.kind != LayoutKind::Instance)
        return false;
    // Only a compiled object has slots. An instance of a class below a built-in class is a cell of that class, and a dictionary is an ordinary object.
    if (!isFoundByShapeTest(record) || !context.plan.slots().holdsCompiledObjects(layout))
        return false;

    const LinkTables& link = (*context.facts).linked();
    OptionSet<LayoutChange> possibleChanges = layout.index() < link.layouts.size() ? link.layout(layout).possibleChanges : LinkedLayout { }.possibleChanges;
    if (!planLookupOfToJSON(context, layout, possibleChanges, plan))
        return false;
    // A definition by a descriptor hides a field from enumeration or makes an accessor of it, which moves it out of its slot, or lays a property that is no field.
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
        // No read sees to the granule of a field out of line that may be listed without one.
        if (!slot.hasStorage() || (slot.isOutOfLine() && slot.mayExistWithoutStorage))
            return std::nullopt;
        // A read would make the closure, so the slot is passed by. It may hold whatever its type admits, not the deferred closure alone.
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

// An object has the first so many fields of an order. Between these two counts, both included, finished objects are likely to stop.
struct StopsInKeyOrderOfJSONObject {
    uint32_t least { 0 };
    uint32_t most { 0 };
};

} // anonymous namespace

// The plan lists no counts at which makers stop: an allocation that lists the beginning of an order has left no trace. So by what is written: a finished object has every field that its type requires,
// and none of those that nobody creates: a method or an accessor of a class lies above its instances, whose written type lists it all the same. It says where an arm pays and never what is right.
// Nothing: no finished object has a beginning of the order for its keys.
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

// The longest stop of each order first: that is what the declaration, an allocation or a maker lists. An order that objects come in and that has no arm would turn them away at every call.
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
    // `nick?: string`: the objects that leave out what their type lets them leave out, those first that leave out the most.
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

    // An order is numbered within its shape.
    const PlannedLayout& planned = context.plan.layout(layout);
    if (planned.keysAreNeverEnumerated || context.plan.shapes().shapesOf(layout).count != 1)
        return std::nullopt;
    // An index is listed in front of every name, wherever the order of the keys has it.
    for (uint32_t field = 0; field < planned.numberOfFields(); ++field) {
        PropertyKeyID key = context.plan.slots().keyOf(context.program, { layout, field });
        if (key && context.program.at(key).kind == PropertyKeyKind::Text && context.program.isIndexLike(key))
            return std::nullopt;
    }
    if (!planKeyOrdersOfJSONObject(context, layout, plan.orders))
        return std::nullopt;
    return plan;
}

// How likely objects of a layout are the data that a program writes as JSON, the likeliest first. A total order of the layouts, by nothing but the layouts.
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
    // A copy: a list of the thread's arena need not outlive what is asked while it is gone through.
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

// ---- Instances of built-in classes

// A pristine instance finds the member of the built-ins that the table lists under the name, or nothing where `findsNothing`, whatever runs. `aspects` gets what has to be pristine for that.
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

// toJSON converts the date to a primitive and calls toISOString, each by name. A test run may replace any of them.
static bool findsMembersOfDateAsBuiltForJSON(const AccessContext& context)
{
    PristineAspectSet aspects;
    auto isAsBuilt = [&](BuiltinName name) {
        return pristineInstanceFindsForJSON(context, BuiltinClasses::Date, name, false, aspects);
    };
    if (!isAsBuilt(BuiltinNames::toJSON) || !isAsBuilt(BuiltinNames::toISOString) || !std::ranges::all_of(context.program.builtins().keysReadBy(Protocol::ToPrimitive), isAsBuilt))
        return false;
    return (aspects | PristineAspectSet { PristineAspects::Names }) == PristineAspectSet { PristineAspects::Names };
}

BuiltinOverloadID findGetTimeForJSON(const AccessContext& context)
{
    const BuiltinTable& builtins = context.program.builtins();
    BuiltinMemberLookup getTime = builtins.findForInstance(BuiltinClasses::Date, BuiltinNames::getTime);
    if (getTime.outcome != BuiltinMemberLookup::Outcome::Found || !getTime.member)
        return { };
    // The function is called whatever the name finds by then: the host reads the time out of the cell.
    BuiltinID function = builtins.canonicalID(getTime.member);
    if (hasFeatureGuard(builtins, function))
        return { };
    BuiltinTypeContext typing = builtinTypeContextOf(context);
    BuiltinOperandTypes operands;
    operands.receiver = context.types.builtinClass(BuiltinClasses::Date);
    Vector<BuiltinOverloadID, 4> reached = overloadsReachedBy(typing, function, BuiltinCallMode::Call, operands);
    if (reached.size() != 1 || builtins.at(reached[0]).isUndeclared() || !overloadTakesOperands(typing, reached[0], BuiltinCallForm::Whole, operands))
        return { };
    return reached[0];
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
        // Such a test says nothing of the storage.
        if (builtinClass == BuiltinClasses::Array)
            return { };
        break;
    case PristineTest::MayPass:
    case PristineTest::ClosureAsCreated:
        break;
    }

    JSONValuePlan plan;
    plan.tested = types.testablePartOf(types.builtinClass(builtinClass));
    // What passes the test has to be an instance of the class, and of no class below it that the test cannot tell.
    if (!types.isSubset(plan.tested, types.builtinClass(builtinClass)))
        return { };
    if (builtinClass == BuiltinClasses::Date && findsMembersOfDateAsBuiltForJSON(context) && findGetTimeForJSON(context)) {
        plan.kind = JSONValueKind::Scalar;
        plan.row = Opcode::JSONAppendDate;
        return plan;
    }
    PristineAspectSet none;
    if (builtinClass == BuiltinClasses::Array && pristineInstanceFindsForJSON(context, builtinClass, BuiltinNames::toJSON, true, none)) {
        TypeID writtenElement = part.expectedContentsAt(builtins, ContentsSlot::Element).predicted();
        plan.kind = JSONValueKind::Array;
        plan.element = writtenElement ? writtenElement : TypeID(TypeTable::anything);
        return plan;
    }
    return { };
}

// ---- Values

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
    // A number further from zero may be written with an exponent.
    if (defined.isInteger() && defined.integers.isSubsetOf(Range::exact()))
        return scalarPlanOfJSON(Opcode::JSONAppendInteger, defined.isInt32() ? TypeID(TypeTable::int32) : types.integer(Range::exact()));
    if (defined.isNumber())
        return scalarPlanOfJSON(Opcode::JSONAppendNumber, TypeTable::number);
    if (defined.isAnyString())
        return scalarPlanOfJSON(Opcode::JSONAppendString, TypeTable::string);

    JSONValuePlan plan;
    plan.kind = JSONValueKind::ObjectParts;
    // A copy: the parts that an id names may move when the arena interns others.
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

// A test of an expected type is worth building only where a value of the proven type can pass it, and has the type behind it.
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

// ---- Whether a plan is worth building

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

// What one unit writes, gone through as the writer goes through it.
struct JSONUnitGoneThrough {
    // The outermost first.
    Vector<LayoutID, 4> layoutsAround;
    uint32_t membersLeft { mostJSONMembersWrittenOutInOneUnit };
};

// Each once, in the order in which they were first called.
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

// The members of an object in a list are written in every turn. A member whose slot proves no plan stands behind a test of its own or goes to the run time, which looks at the value: the host, which writes an object slot by slot, is the shorter way.
static bool slotsProvePlansOfListedJSONObjects(const AccessContext& context, const JSONValuePlan& plan, Vector<LayoutID, 4>& layoutsAround)
{
    switch (plan.kind) {
    case JSONValueKind::Generic:
    case JSONValueKind::Nullish:
    case JSONValueKind::Scalar:
        return true;
    case JSONValueKind::Array:
        return slotsProvePlansOfListedJSONObjects(context, planJSONValue(context, plan.element, { }), layoutsAround);
    case JSONValueKind::ObjectParts:
        return std::ranges::all_of(plan.parts, [&](const JSONValuePlan& part) {
            return slotsProvePlansOfListedJSONObjects(context, part, layoutsAround);
        });
    case JSONValueKind::Objects:
        break;
    }

    return std::ranges::all_of(plan.layouts, [&](LayoutID layout) {
        if (layoutsAround.contains(layout))
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
                return memberPlan.kind != JSONValueKind::Generic && slotsProvePlansOfListedJSONObjects(context, memberPlan, layoutsAround);
            });
        };
        layoutsAround.append(layout);
        bool proveAll = std::ranges::all_of(object->orders, [&](const JSONKeyOrderPlan& order) {
            return slotsProvePlans(order.members.span());
        });
        proveAll = proveAll && (!object->membersInAnyOrder || slotsProvePlans(object->membersInAnyOrder->span()));
        layoutsAround.removeLast();
        return proveAll;
    });
}

bool repeatsJSONListAround(std::span<const JSONListAround> listsAround, const JSONListAround& list)
{
    return std::ranges::contains(listsAround, list);
}

// By what the type of a list says of its elements, which is all that anybody says of them. Such a list would be turned away behind work that was for nothing.
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

static bool writesWholeJSONValueInUnit(const AccessContext&, const JSONValuePlan&, TypeID typeOfPlan, JSONUnitGoneThrough&, JSONSerializersCalled&);

static bool writesMembersOfJSONObjectInUnit(const AccessContext& context, const JSONObjectPlan& object, JSONUnitGoneThrough& unit, JSONSerializersCalled& serializers)
{
    auto writesMembers = [&](std::span<const JSONMemberPlan> members) {
        return std::ranges::all_of(members, [&](const JSONMemberPlan& member) {
            TypeID proven = provenTypeOfJSONMember(context, object.layout, member.field);
            if (hasNoJSONText(context, proven))
                return true;
            TypeID written = writtenTypeOfJSONMember(context, object.layout, member.field);
            JSONValuePlan memberPlan = planJSONValue(context, proven, written);
            return writesWholeJSONValueInUnit(context, memberPlan, memberPlan.isExpected ? written : proven, unit, serializers);
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

static bool writesWholeJSONValueInUnit(const AccessContext& context, const JSONValuePlan& plan, TypeID typeOfPlan, JSONUnitGoneThrough& unit, JSONSerializersCalled& serializers)
{
    TypeArena& types = context.types;
    // JSON.stringify throws on a BigInt.
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
        Vector<LayoutID, 4> layoutsAround;
        if (!slotsProvePlansOfListedJSONObjects(context, element, layoutsAround))
            return false;
        // `items: number[]` in a slot that is proven a list and no more: the plan has the elements from what is written, and the writer goes down by the plan.
        if (listsFoundInJSONListNeverEnd(context, plan.element, plan.tested))
            return false;
        return writesWholeJSONValueInUnit(context, element, plan.element, unit, serializers);
    }
    case JSONValueKind::ObjectParts:
        return std::ranges::all_of(plan.parts, [&](const JSONValuePlan& part) {
            return writesWholeJSONValueInUnit(context, part, typeCoveredByJSONPlan(context, part), unit, serializers);
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
        if (!writesMembersOfJSONObjectInUnit(context, *object, unit, serializers))
            return false;
    }
    return true;
}

bool writesWholeJSONValue(const AccessContext& context, const JSONValuePlan& plan, TypeID typeOfPlan, bool& callsSerializers)
{
    JSONSerializersCalled serializers;
    JSONUnitGoneThrough unitOfCall;
    if (!writesWholeJSONValueInUnit(context, plan, typeOfPlan, unitOfCall, serializers))
        return false;
    // The list grows while it is gone through.
    for (size_t index = 0; index < serializers.layouts.size(); ++index) {
        std::optional<JSONObjectPlan> object = planJSONObject(context, serializers.layouts[index]);
        if (!object)
            return false;
        JSONUnitGoneThrough serializer { { }, roomForJSONMembersInsideObjectOfSerializer(context, *object) };
        if (!writesMembersOfJSONObjectInUnit(context, *object, serializer, serializers))
            return false;
    }
    callsSerializers = !serializers.layouts.isEmpty();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
