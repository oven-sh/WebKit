/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTNameClass.h"
#include "AOTOpcodeSets.h"
#include "AOTSlotRepresentation.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t closedOperationsOfCopyInWalkCopies = 3;

namespace {

enum class KindOfCopyInWalkCopies : uint8_t {
    StoresIntoLiteral,
    DefinesOnLiteral,
    MakesObject,
    MakesObjectByStores,
    DefinesOnObjectThatStands,
};

struct ObjectCopyInWalkCopies {
    bool makesObject() const { return kind == KindOfCopyInWalkCopies::MakesObject || kind == KindOfCopyInWalkCopies::MakesObjectByStores; }
    bool stores() const { return kind == KindOfCopyInWalkCopies::StoresIntoLiteral || kind == KindOfCopyInWalkCopies::MakesObjectByStores; }

    Walker& walker;
    KindOfCopyInWalkCopies kind;
    LayoutID layoutOfLiteral;
};

struct CopiedOrderInWalkCopies {
    OwnKeyOrder keys;
    Vector<FieldIndex, 8> fields;
    Vector<FieldIndex, 8> fieldsOfTaker;
};

struct PropertiesInWalkCopies {
    void put(FieldIndex field, Reference value)
    {
        if (isListed.get(field)) {
            values[fields.find(field)] = value;
            return;
        }
        isListed.set(field);
        fields.append(field);
        values.append(value);
    }

    LayoutID layout;
    BitVector isListed;
    Vector<FieldIndex, 8> fields;
    Vector<Reference, 8> values;
};

struct LiteralInSightInWalkCopies {
    Vector<ValueID, 4> names;
    PropertiesInWalkCopies properties;
    bool keepsItsNames { false };
};

} // anonymous namespace

static PropertyKeyID keyOfFieldInWalkCopies(const Walker& walker, LayoutID layout, FieldIndex field)
{
    return walker.plan().slots().keyOf(walker.program(), { layout, field });
}

static bool isPlainPropertyInWalkCopies(const Walker& walker, FieldReference field)
{
    const Program& program = walker.program();
    const PlacedSlot& slot = walker.plan().slot(field.layout, field.field);
    if (field.field >= program.at(field.layout).fields.size() || !slot.hasStorage() || !canLeaveSlot(slot.representation))
        return false;
    FieldFacts facts = walker.facts().field(field);
    const Field& declared = program.at(field.layout).fields[field.field];
    if (facts.facts.contains(FieldFact::IsAccessor) || !facts.formOfValue.isEmpty() || slot.holdsAccessor || slot.deferredFunction || slot.representation == SlotRepresentation::DeferredClosure)
        return false;
    return declared.key && !declared.computedKey && declared.form.isEmpty() && !declared.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsBrandOfMethods, FieldFlags::IsHidden });
}

static bool onlyObjectPrototypeStandsAboveObjectsOfInWalkCopies(const Walker& walker, LayoutID layout)
{
    const Layout& record = walker.program().at(layout);
    return record.kind == LayoutKind::Record && !record.constructor && !record.has(LayoutFlags::IsConstructorFunction) && !record.has(LayoutFlags::IsOfCreatedObject);
}

static std::optional<FieldIndex> findFieldThatTakesInWalkCopies(const ObjectCopyInWalkCopies& copy, LayoutID layoutOfSource, FieldIndex field, PropertyKeyID key)
{
    const Walker& walker = copy.walker;
    const Program& program = walker.program();
    LayoutID taker = copy.makesObject() ? layoutOfSource : copy.layoutOfLiteral;
    if (copy.stores() && (isOnObjectPrototype(program.nameClassOf(key)) || !isClosedKey(program, program.builtins().canonicalID(Builtins::ObjectPrototype), key) || !onlyObjectPrototypeStandsAboveObjectsOfInWalkCopies(walker, taker)))
        return std::nullopt;
    if (copy.makesObject() || copy.kind == KindOfCopyInWalkCopies::DefinesOnObjectThatStands)
        return isPlainPropertyInWalkCopies(walker, { layoutOfSource, field }) ? std::optional(field) : std::nullopt;
    std::optional<FieldReference> found = findField(program, taker, key);
    if (!found || found->layout != taker || !isPlainPropertyInWalkCopies(walker, *found))
        return std::nullopt;
    return found->field;
}

static bool createdFieldsHoldNoSymbolToCopyInWalkCopies(const ObjectCopyInWalkCopies& copy, const OwnKeyOrder& listed)
{
    const Walker& walker = copy.walker;
    const Program& program = walker.program();
    const Layout& layout = program.at(listed.layout);
    for (FieldIndex field : listed.created) {
        PropertyKeyID key = keyOfFieldInWalkCopies(walker, listed.layout, field);
        if (!key)
            return false;
        OptionSet<FieldForm> form = walker.facts().field({ listed.layout, field }).formOfValue;
        if (field < layout.fields.size())
            form.add(layout.fields[field].form);
        if (form.contains(FieldForm::IsAccessor) || walker.plan().slot(listed.layout, field).holdsAccessor)
            return false;
        if (isSymbolKind(program.at(key).kind) && !form.contains(FieldForm::IsNotEnumerable))
            return false;
    }
    return true;
}

static bool copyListsEveryFilledFieldInWalkCopies(const Walker& walker, const CopiedOrderInWalkCopies& order)
{
    const LinkTables& linked = walker.facts().linked();
    LayoutID layout = order.keys.layout;
    for (FieldIndex field = 0; field < walker.program().at(layout).fields.size(); ++field) {
        const LinkedCell* cell = linked.findCellOfField({ layout, field });
        if (cell && cell->isFilled && !order.fields.contains(field))
            return false;
    }
    return true;
}

static bool fillCopiedOrderInWalkCopies(const ObjectCopyInWalkCopies& copy, const OwnKeyOrder& listed, bool isInSight, CopiedOrderInWalkCopies& order)
{
    const Walker& walker = copy.walker;
    const Program& program = walker.program();
    order.keys = listed;
    if (!listed.layout)
        return listed.fields.isEmpty();
    if (!isInSight && listed.fields.size() > mostFieldsOfOwnKeyWalk)
        return false;

    const Layout& layout = program.at(listed.layout);
    if (copy.makesObject() && layout.kind != LayoutKind::Record)
        return false;

    for (FieldIndex field : listed.fields) {
        PropertyKeyID key = keyOfFieldInWalkCopies(walker, listed.layout, field);
        if (!key)
            return false;
        NameClass nameClass = program.nameClassOf(key);
        if (nameClass == NameClass::Proto || mayBeElementKey(nameClass))
            return false;
        if (field < layout.fields.size() && !layout.fields[field].form.isEmpty())
            return false;
        std::optional<FieldIndex> taker = findFieldThatTakesInWalkCopies(copy, listed.layout, field, key);
        if (!taker)
            return false;
        order.fields.append(field);
        order.fieldsOfTaker.append(*taker);
    }
    if (!createdFieldsHoldNoSymbolToCopyInWalkCopies(copy, listed))
        return false;
    if (!isInSight && order.fields.size() > mostFieldsOfOwnKeyWalk)
        return false;
    return !copy.makesObject() || copyListsEveryFilledFieldInWalkCopies(walker, order);
}

static Vector<CopiedOrderInWalkCopies, 2> planCopyByLayoutsInWalkCopies(const ObjectCopyInWalkCopies& copy, const OwnKeyPlan& plan)
{
    Vector<CopiedOrderInWalkCopies, 2> orders;
    for (const OwnKeyOrder& listed : plan.orders) {
        CopiedOrderInWalkCopies order;
        if (fillCopiedOrderInWalkCopies(copy, listed, false, order) && order.keys.layout)
            orders.append(WTF::move(order));
    }
    return orders;
}

static LayoutID layoutOfLiteralMadeByInWalkCopies(const Program& program, const BodyValue& made)
{
    if (made.opcode() == Opcode::NewObject)
        return made.as<Opcode::NewObject>().prototypeOperand() ? LayoutID() : made.as<Opcode::NewObject>().layout();
    if (made.opcode() != Opcode::Data)
        return { };
    const auto* object = program.data[made.as<Opcode::Data>().data().index()].root.tryAs<DataNode::Object>();
    return object ? object->layout : LayoutID();
}

static std::optional<LiteralInSightInWalkCopies> findLiteralInSightInWalkCopies(Walker& walker, const BodyValue& user, ValueID operand)
{
    const BodyIndex& index = walker.currentBodyIndex();
    const Program& program = walker.program();
    LiteralInSightInWalkCopies literal;
    ValueID allocated = operand;
    while (allocated && !index.isParameter(allocated) && inputIdentityOpcodes.contains(index.opcodeOf(allocated))) {
        literal.names.append(allocated);
        allocated = index.value(allocated).operand(0);
    }
    if (!allocated || index.isParameter(allocated))
        return std::nullopt;
    BlockID blockOfLiteral = index.blockOf(allocated);
    BlockID blockOfUser = index.blockOf(user.id());
    bool standsInBlockInFront = blockOfLiteral != blockOfUser;
    if (standsInBlockInFront && (!index.dominates(blockOfLiteral, blockOfUser) || index.loopOf(blockOfLiteral) != index.loopOf(blockOfUser) || index.hasIrreducibleFlow()))
        return std::nullopt;
    literal.names.append(allocated);
    literal.keepsItsNames = standsInBlockInFront || (walker.walksStretchInsideAnother() && allocated.index() < index.block(blockOfUser).firstValue().index() + walker.beginOfStretchBeingWalked());
    LayoutID layout = layoutOfLiteralMadeByInWalkCopies(program, index.value(allocated));
    if (!layout || program.at(layout).kind != LayoutKind::Record || !walker.plan().slots().holdsCompiledObjects(layout))
        return std::nullopt;

    for (ValueID name : literal.names) {
        if (index.useCount(name) == BodyIndex::maxCountedUses)
            return std::nullopt;
    }
    Vector<uint32_t, 4> mentions;
    mentions.fill(0, literal.names.size());
    uint32_t endOfBlockOfLiteral = index.block(blockOfLiteral).endOfValues().index();
    uint32_t beginOfBlockOfUser = index.block(blockOfUser).firstValue().index();
    for (uint32_t id = allocated.index() + 1; id < user.id().index(); ++id) {
        if (standsInBlockInFront && id == endOfBlockOfLiteral)
            id = beginOfBlockOfUser;
        if (id >= user.id().index())
            break;
        BodyValue between = index.value(ValueID(id));
        Opcode opcode = between.opcode();
        bool onlyFillsOrHandsOn = true;
        between.forEachOperand([&](ValueID mentioned, uint32_t operandIndex) {
            size_t place = literal.names.find(mentioned);
            if (place == notFound)
                return;
            ++mentions[place];
            bool fills = (opcode == Opcode::PropDefine && operandIndex == OperandIndex::PropDefine::object) || (opcode == Opcode::CopyDataProperties && operandIndex == OperandIndex::CopyDataProperties::target);
            bool handsOn = !operandIndex && literal.names.contains(between.id());
            onlyFillsOrHandsOn &= fills || handsOn;
        });
        if (!onlyFillsOrHandsOn)
            return std::nullopt;
    }
    for (size_t place = 0; place < literal.names.size(); ++place) {
        if (literal.names[place] != operand && mentions[place] != index.useCount(literal.names[place]))
            return std::nullopt;
    }
    if (literal.keepsItsNames) {
        uint32_t mentionsByUser = 0;
        user.forEachOperand([&](ValueID mentioned, uint32_t) {
            mentionsByUser += mentioned == operand;
        });
        size_t placeOfOperand = literal.names.find(operand);
        if (placeOfOperand == notFound || mentions[placeOfOperand] + mentionsByUser != index.useCount(operand))
            return std::nullopt;
    }

    Reference built = walker.referenceOf(operand);
    if (!built || !built.isValue())
        return std::nullopt;
    ValueID cell = underlyingCellOf(walker.code(), built.id());
    if (walker.code().isConstant(cell) || !walker.state().isFresh(Reference::value(cell)))
        return std::nullopt;
    std::optional<OwnKeyOrder> keys = ownKeysInSight(walker, built);
    if (!keys || (keys->layout && keys->layout != layout))
        return std::nullopt;

    literal.properties.layout = layout;
    for (FieldIndex field : keys->created) {
        Reference held = walker.state().slot(Reference::value(cell), { layout, field });
        TypeID type = held ? walker.typeOf(held) : TypeID();
        if (!type || (walker.types().at(type).primitives & (Primitive::Absent | Primitive::Unspecified)) || !isPlainPropertyInWalkCopies(walker, { layout, field }))
            return std::nullopt;
        literal.properties.put(field, held);
    }
    return literal;
}

static Reference buildLiteralInWalkCopies(Walker& walker, Origin origin, const PropertiesInWalkCopies& properties)
{
    if (walker.isUnreachable())
        return { };
    Draft allocation(Opcode::NewObject, origin);
    allocation.standsForNoValueOfInput = true;
    allocation.setLayout(properties.layout);
    allocation.setFields(properties.fields.span());
    allocation.setValuesOperands(properties.values.span());
    walker.chooseConservativeFields(allocation);
    Reference object = walker.isUnreachable() ? Reference() : walker.build(WTF::move(allocation));
    noteFreshObject(walker, object, properties.fields.size());
    return object;
}

static void bindLiteralInWalkCopies(Walker& walker, const LiteralInSightInWalkCopies& literal, Reference object)
{
    if (literal.keepsItsNames)
        return;
    for (ValueID name : literal.names)
        walker.bind(name, object);
}

static bool takeFieldsOfSourceInWalkCopies(Walker& walker, const CopiedOrderInWalkCopies& order, Reference source, Origin origin, PropertiesInWalkCopies& properties)
{
    for (size_t index = 0; index < order.fields.size(); ++index) {
        Reference value = valueOfOwnKey(walker, source, order.keys.layout, order.fields[index], origin);
        if (!value)
            return false;
        properties.put(order.fieldsOfTaker[index], value);
    }
    return true;
}

static Reference buildArmsInWalkCopies(Walker& walker, Reference source, std::span<const CopiedOrderInWalkCopies> orders, bool layoutsAreProven, Origin origin, NOESCAPE const Invocable<Reference(const CopiedOrderInWalkCopies&, Reference)> auto& buildArm, NOESCAPE const Invocable<Reference()> auto& buildAsWritten)
{
    walker.recordSpeculation();
    BlockID asWritten = walker.addBlock(BlockFrequency::Unlikely);
    BlockID behindCopy = walker.addBlock();
    Reference filled = walker.addParameter(behindCopy, { }, Representation::Cell);

    Reference narrowedByArmInFront;
    for (size_t index = 0; index < orders.size(); ++index) {
        bool isLast = index + 1 == orders.size();
        bool nextArmHasSameLayout = !isLast && orders[index + 1].keys.layout == orders[index].keys.layout;
        bool armInFrontHasSameLayout = index && orders[index - 1].keys.layout == orders[index].keys.layout;
        BlockID next = isLast ? asWritten : walker.addBlock();
        WaysOutOfOwnKeyTests waysOut { nextArmHasSameLayout ? asWritten : next, nextArmHasSameLayout ? next : asWritten, asWritten };

        Reference object = armInFrontHasSameLayout ? narrowedByArmInFront : source;
        if (continueIfOwnKeys(walker, object, orders[index].keys, layoutsAreProven || armInFrontHasSameLayout, origin, waysOut)) {
            if (Reference built = buildArm(orders[index], object))
                walker.jumpTo(behindCopy, singleElementSpan(built));
        }
        narrowedByArmInFront = object;
        if (walker.hasFailed())
            return { };
        walker.continueIn(next);
    }

    if (Reference built = walker.isUnreachable() ? Reference() : buildAsWritten())
        walker.jumpTo(behindCopy, singleElementSpan(built));
    if (walker.hasFailed())
        return { };
    walker.continueIn(behindCopy);
    return walker.isUnreachable() ? Reference() : filled;
}

static Reference buildObjectMadeByCopyInWalkCopies(Walker& walker, const Draft& written, const BodyValue& value, const CopiedOrderInWalkCopies& order, Reference source)
{
    Origin origin = written.origin();
    auto namesLeftOut = value.as<Opcode::CopyObject>().excludedKeys();
    if (!order.keys.layout || order.fields.isEmpty() || !namesLeftOut.isEmpty()) {
        Vector<std::pair<PropertyKeyID, Reference>, 8> definitions;
        for (FieldIndex field : order.fields) {
            PropertyKeyID key = keyOfFieldInWalkCopies(walker, order.keys.layout, field);
            if (std::ranges::contains(namesLeftOut, key))
                continue;
            Reference held = valueOfOwnKey(walker, source, order.keys.layout, field, origin);
            if (!held)
                return { };
            definitions.append({ key, held });
        }
        Draft empty(Opcode::NewDictionary, origin);
        empty.inputValueGiven = value.id();
        empty.expected = written.expected;
        empty.resultIsReturnedAtOnce = written.resultIsReturnedAtOnce && definitions.isEmpty();
        walker.chooseConservativeFields(empty);
        Reference rest = walker.build(WTF::move(empty));
        for (auto [key, held] : definitions) {
            if (!rest || walker.isUnreachable() || walker.hasFailed())
                return { };
            Draft define(Opcode::PropDefine, origin);
            walker.chooseConservativeFields(define);
            define.setObjectOperand(rest);
            define.setKey(key);
            define.setValueOperand(held);
            walker.build(WTF::move(define));
        }
        if (!namesLeftOut.isEmpty())
            walker.count(StatisticID::ObjectRestsExpandedToDefines);
        return rest;
    }

    PropertiesInWalkCopies properties;
    properties.layout = order.keys.layout;
    if (!takeFieldsOfSourceInWalkCopies(walker, order, source, origin, properties))
        return { };
    Draft allocation(Opcode::NewObject, origin);
    allocation.inputValueGiven = value.id();
    allocation.expected = written.expected;
    allocation.resultIsReturnedAtOnce = written.resultIsReturnedAtOnce;
    allocation.setLayout(properties.layout);
    allocation.setFields(properties.fields.span());
    allocation.setValuesOperands(properties.values.span());
    walker.chooseConservativeFields(allocation);
    Reference object = walker.isUnreachable() ? Reference() : walker.build(WTF::move(allocation));
    noteFreshObject(walker, object, properties.fields.size());
    return object;
}

bool buildCopyThatMakesObject(Walker& walker, const BodyValue& value)
{
    if (value.opcode() != Opcode::CopyObject || !walker.has(Mechanism::TypedOperations))
        return false;
    auto operation = value.as<Opcode::CopyObject>();
    Reference source = walker.referenceOf(operation.sourceOperand());
    if (!source)
        return false;
    ObjectCopyInWalkCopies copy { walker, operation.assigns() ? KindOfCopyInWalkCopies::MakesObjectByStores : KindOfCopyInWalkCopies::MakesObject, { } };

    if (!source.isValue() || !operation.excludedOperands().empty())
        return false;
    bool leavesNamesOut = !operation.excludedKeys().isEmpty();
    if (leavesNamesOut) {
        if (operation.assigns() || walker.someFrameWalksStretchInsideAnother() || walker.hasStringsInBuilders())
            return false;
        copy.kind = KindOfCopyInWalkCopies::DefinesOnObjectThatStands;
    }

    if (std::optional<OwnKeyOrder> listed = ownKeysInSight(walker, source)) {
        CopiedOrderInWalkCopies order;
        if (fillCopiedOrderInWalkCopies(copy, *listed, true, order)) {
            Draft written = walker.draftOf(value);
            if (Reference made = walker.hasFailed() ? Reference() : buildObjectMadeByCopyInWalkCopies(walker, written, value, order, source))
                walker.bind(value.id(), made);
            return true;
        }
    }

    if (leavesNamesOut && !walker.isInLoop())
        return false;
    OwnKeyPlan plan = planOwnKeyWalk(walker, operation.sourceOperand(), source);
    Vector<CopiedOrderInWalkCopies, 2> orders = planCopyByLayoutsInWalkCopies(copy, plan);
    if (orders.isEmpty() || !shouldOpenGuess(walker, source, walker.types().forLayout(orders[0].keys.layout), plan.isDeclared, closedOperationsOfCopyInWalkCopies))
        return false;
    Draft written = walker.draftOf(value);
    if (walker.hasFailed())
        return true;
    written.resultIsReturnedAtOnce = false;
    bool isTailCall = std::exchange(written.isTailCall, false);

    Reference made = buildArmsInWalkCopies(walker, source, orders.span(), plan.layoutsAreProven, written.origin(), [&](const CopiedOrderInWalkCopies& order, Reference tested) {
        return buildObjectMadeByCopyInWalkCopies(walker, written, value, order, tested);
    }, [&] {
        Draft asWritten = written;
        asWritten.isTailCall = isTailCall;
        walker.chooseConservativeFields(asWritten);
        return walker.build(WTF::move(asWritten));
    });
    if (made)
        walker.bind(value.id(), made);
    return true;
}

static bool isLayoutOfPlainLiteralInWalkCopies(const Program& program, LayoutID layout)
{
    const Layout& record = program.at(layout);
    if (record.kind != LayoutKind::Record || record.writtenBuiltinBase || record.computedKeyCount || record.has(LayoutFlags::HasMembersWithoutLayout))
        return false;
    return std::ranges::all_of(record.fields, [](const Field& declared) {
        return declared.form.isEmpty();
    });
}

namespace {

struct SourceOfAssignmentInWalkCopies {
    const CopiedOrderInWalkCopies& order() const { return orders[0]; }
    unsigned numberOfArms() const { return isInSight ? 1 : orders.size() + !!withoutProperties; }

    ValueID inBody;
    Reference built;
    Vector<CopiedOrderInWalkCopies, 2> orders;
    TypeID withoutProperties;
    bool isInSight { false };
    bool layoutIsProven { false };
};

} // anonymous namespace

static TypeID partWithoutPropertiesInWalkCopies(Walker& walker, Reference source)
{
    TypeArena& types = walker.types();
    TypeID whole = walker.typeOf(source);
    TypeID withProperties = types.withoutPrimitives(whole, primitivesWithoutPropertiesToCopy);
    TypeID without = types.differenceOf(whole, withProperties);
    return types.at(withProperties).isNever() || types.at(without).isNever() || types.testablePartOf(without) != without ? TypeID() : without;
}

static bool planSourceOfAssignmentInWalkCopies(const ObjectCopyInWalkCopies& copy, SourceOfAssignmentInWalkCopies& source, unsigned mostArms, bool mayAskTests, bool mayBeEmpty)
{
    Walker& walker = copy.walker;
    if (!source.built || !source.built.isValue())
        return false;
    if (std::optional<OwnKeyOrder> listed = ownKeysInSight(walker, source.built)) {
        CopiedOrderInWalkCopies order;
        if (fillCopiedOrderInWalkCopies(copy, *listed, true, order)) {
            source.orders.append(WTF::move(order));
            source.isInSight = true;
            return true;
        }
    }
    if (!mayAskTests || !mostArms)
        return false;
    OwnKeyPlan plan = planOwnKeyWalk(walker, source.inBody, source.built, mostFieldsOfOwnKeyWalk, mostArms);
    Vector<CopiedOrderInWalkCopies, 2> orders = planCopyByLayoutsInWalkCopies(copy, plan);
    if (orders.isEmpty() || !shouldOpenGuess(walker, source.built, walker.types().forLayout(orders[0].keys.layout), plan.isDeclared, closedOperationsOfCopyInWalkCopies))
        return false;
    auto listsKeys = [](const OwnKeyOrder& listed) {
        return !!listed.layout;
    };
    if (copy.kind == KindOfCopyInWalkCopies::DefinesOnLiteral && plan.coversMakers && orders.size() < static_cast<size_t>(std::ranges::count_if(plan.orders, listsKeys)))
        return false;
    auto isFinished = [&](const CopiedOrderInWalkCopies& order) {
        return listsAllRequiredFields(walker, order.keys.layout, order.keys.created.span());
    };
    if (orders.containsIf(isFinished)) {
        orders.removeAllMatching([&](const CopiedOrderInWalkCopies& order) {
            return !isFinished(order);
        });
    }
    CopiedOrderInWalkCopies likeliest = WTF::move(orders[0]);
    orders.removeAt(0);
    orders.removeAllMatching([&](const CopiedOrderInWalkCopies& order) {
        return !plan.coversMakers || !isMadeWithKeys(walker, order.keys.layout, order.keys.created.span());
    });
    source.orders.append(WTF::move(likeliest));
    source.orders.appendVector(WTF::move(orders));
    source.layoutIsProven = plan.layoutsAreProven;
    if (!mayBeEmpty)
        return true;
    for (LayoutID layout : plan.layoutsWithoutRequiredFields) {
        CopiedOrderInWalkCopies order;
        if (walker.isInLoop() && source.orders.size() < mostArms && fillCopiedOrderInWalkCopies(copy, OwnKeyOrder { layout, { }, { } }, false, order))
            source.orders.append(WTF::move(order));
    }
    if (source.orders.size() < mostArms && walker.isInLoop())
        source.withoutProperties = partWithoutPropertiesInWalkCopies(walker, source.built);
    return true;
}

static void walkSourceArmsInWalkCopies(Walker& walker, const SourceOfAssignmentInWalkCopies& source, Origin origin, BlockID whereTestsFail, NOESCAPE const Invocable<void(const CopiedOrderInWalkCopies*, Reference)> auto& walkArm)
{
    if (source.isInSight) {
        walkArm(&source.order(), source.built);
        return;
    }
    Reference precedingLayoutResult;
    LayoutID layoutInFront;
    for (size_t place = 0; place < source.orders.size(); ++place) {
        const CopiedOrderInWalkCopies& order = source.orders[place];
        bool isLast = place + 1 == source.orders.size();
        bool hasSameLayoutAsNext = !isLast && source.orders[place + 1].keys.layout == order.keys.layout;
        bool hasSameLayoutAsLast = layoutInFront && layoutInFront == order.keys.layout;
        BlockID next = isLast && !source.withoutProperties ? whereTestsFail : walker.addBlock();
        WaysOutOfOwnKeyTests waysOut { hasSameLayoutAsNext ? whereTestsFail : next, hasSameLayoutAsNext ? next : whereTestsFail, whereTestsFail };
        Reference tested = hasSameLayoutAsLast ? precedingLayoutResult : source.built;
        bool passes = continueIfOwnKeys(walker, tested, order.keys, source.layoutIsProven || hasSameLayoutAsLast, origin, waysOut);
        if (walker.hasFailed())
            return;
        precedingLayoutResult = tested;
        layoutInFront = order.keys.layout;
        if (passes)
            walkArm(&order, tested);
        if (walker.hasFailed() || next == whereTestsFail)
            return;
        walker.continueIn(next);
        if (walker.isUnreachable())
            return;
    }
    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(source.built);
    test.setTestedType(source.withoutProperties);
    Reference hasNone = walker.build(WTF::move(test));
    if (!hasNone)
        return;
    BlockID nothingToCopy = walker.addBlock();
    walker.branch(hasNone, nothingToCopy, whereTestsFail);
    walker.continueIn(nothingToCopy);
    if (!walker.isUnreachable() && !walker.hasFailed())
        walkArm(nullptr, source.built);
}

static Reference makeSourceInSightAgainInWalkCopies(Walker& walker, const SourceOfAssignmentInWalkCopies& source, Origin origin)
{
    if (!source.isInSight || !source.order().keys.layout || !walker.currentBodyIndex().hasOneUse(source.inBody) || !isLayoutOfPlainLiteralInWalkCopies(walker.program(), source.order().keys.layout))
        return { };
    ValueID cell = source.built && source.built.isValue() ? underlyingCellOf(walker.code(), source.built.id()) : ValueID();
    if (!cell || walker.code().isConstant(cell) || !walker.state().isFresh(Reference::value(cell)))
        return { };
    PropertiesInWalkCopies held;
    const CopiedOrderInWalkCopies& order = source.order();
    held.layout = order.keys.layout;
    if (order.keys.created.size() != order.fields.size())
        return { };
    for (size_t index = 0; index < order.fields.size(); ++index) {
        FieldIndex field = order.fields[index];
        Reference value = field == order.keys.created[index] && isPlainPropertyInWalkCopies(walker, { held.layout, field }) ? valueOfOwnKey(walker, source.built, held.layout, field, origin) : Reference();
        if (!value)
            return { };
        held.put(field, value);
    }
    return buildLiteralInWalkCopies(walker, origin, held);
}

static constexpr unsigned maxArmsOfCopyInWalkCopies = 6;

static unsigned maxArmsOfSourceInWalkCopies(unsigned numberOfArms)
{
    return std::min(maxArmsOfCopiedSource, std::max(1u, maxArmsOfCopyInWalkCopies / numberOfArms));
}

bool assignSourcesFieldByField(Walker& walker, const BodyValue& value)
{
    if (value.opcode() != Opcode::Call || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::TypedOperations))
        return false;
    auto call = value.as<Opcode::Call>();
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (call.isDoneInPlace() || arguments.size() < 2)
        return false;
    {
        const BodyIndex& index = walker.currentBodyIndex();
        ValueID allocated = arguments[0];
        while (allocated && !index.isParameter(allocated) && inputIdentityOpcodes.contains(index.opcodeOf(allocated)))
            allocated = index.value(allocated).operand(0);
        if (!allocated || index.isParameter(allocated) || (index.opcodeOf(allocated) != Opcode::NewObject && index.opcodeOf(allocated) != Opcode::Data))
            return false;
    }
    Reference callee = walker.referenceOf(call.calleeOperand());
    if (!callee)
        return false;
    Type typeOfCallee = walker.types().at(walker.typeOf(callee));
    BuiltinID builtin = typeOfCallee.isObject() ? singleBuiltinOf(walker.types(), typeOfCallee) : BuiltinID();
    if (!builtin || walker.program().builtins().canonicalID(builtin) != Builtins::ObjectAssign)
        return false;

    std::optional<LiteralInSightInWalkCopies> literal = findLiteralInSightInWalkCopies(walker, value, arguments[0]);
    if (!literal || !isLayoutOfPlainLiteralInWalkCopies(walker.program(), literal->properties.layout))
        return false;
    Reference cell = Reference::value(underlyingCellOf(walker.code(), walker.referenceOf(arguments[0]).id()));
    if (!walker.state().cellFacts(cell).containsAll({ CellFact::NotFrozen, CellFact::IsExtensible, CellFact::HasLayoutPrototype }))
        return false;

    ObjectCopyInWalkCopies copy { walker, KindOfCopyInWalkCopies::StoresIntoLiteral, literal->properties.layout };
    Vector<SourceOfAssignmentInWalkCopies, 4> planned;
    unsigned numberOfArms = 1;
    for (ValueID argument : arguments.subspan(1)) {
        SourceOfAssignmentInWalkCopies source { argument, literal->names.contains(argument) ? Reference() : walker.referenceOf(argument), { }, { }, false, false };
        if (!planSourceOfAssignmentInWalkCopies(copy, source, maxArmsOfSourceInWalkCopies(numberOfArms), true, true))
            break;
        numberOfArms *= source.numberOfArms();
        planned.append(WTF::move(source));
    }
    if (planned.isEmpty())
        return false;

    Draft written = walker.draftOf(value);
    if (walker.hasFailed())
        return true;
    walker.recordExpandedObjectAssign();
    written.resultIsReturnedAtOnce = false;
    bool isTailCall = std::exchange(written.isTailCall, false);
    Origin origin = written.origin();
    Vector<Reference, 4> operandsAsWritten(written.argumentsOperands());
    walker.count(StatisticID::OwnKeyWalksExpanded, static_cast<uint32_t>(planned.size()));

    auto assignAsWrittenFrom = [&](size_t firstSource, const PropertiesInWalkCopies& properties) -> Reference {
        Reference object = buildLiteralInWalkCopies(walker, origin, properties);
        if (!object)
            return { };
        bindLiteralInWalkCopies(walker, *literal, object);
        Vector<Reference, 4> operands;
        operands.append(object);
        operands.append(operandsAsWritten.span().subspan(1 + firstSource));
        for (size_t place = 1 + firstSource; place < arguments.size(); ++place) {
            if (literal->names.contains(arguments[place]))
                operands[place - firstSource] = object;
        }
        for (size_t later = firstSource; later < planned.size(); ++later) {
            if (Reference madeAgain = makeSourceInSightAgainInWalkCopies(walker, planned[later], origin))
                operands[1 + later - firstSource] = madeAgain;
        }
        Draft rest = written;
        rest.isTailCall = isTailCall;
        rest.setArgumentsOperands(operands.span());
        walker.chooseConservativeFields(rest);
        walker.build(WTF::move(rest));
        return walker.isUnreachable() ? Reference() : object;
    };

    BlockID continuationBlock;
    Reference assigned;
    Reference object;
    auto assignSourcesFrom = [&](auto& self, size_t first, const PropertiesInWalkCopies& properties) -> void {
        if (walker.isUnreachable() || walker.hasFailed())
            return;
        if (first == planned.size()) {
            object = planned.size() + 1 < arguments.size() ? assignAsWrittenFrom(planned.size(), properties) : buildLiteralInWalkCopies(walker, origin, properties);
            if (object && continuationBlock)
                walker.jumpTo(continuationBlock, singleElementSpan(object));
            return;
        }
        const SourceOfAssignmentInWalkCopies& source = planned[first];
        BlockID asWritten;
        if (!source.isInSight) {
            if (!continuationBlock) {
                walker.recordSpeculation();
                continuationBlock = walker.addBlock();
                assigned = walker.addParameter(continuationBlock, { }, Representation::Cell);
            }
            asWritten = walker.addBlock(BlockFrequency::Unlikely);
        }
        walkSourceArmsInWalkCopies(walker, source, origin, asWritten, [&](const CopiedOrderInWalkCopies* order, Reference tested) {
            PropertiesInWalkCopies taken = properties;
            if (!order || takeFieldsOfSourceInWalkCopies(walker, *order, tested, origin, taken))
                self(self, first + 1, taken);
        });
        if (!asWritten || walker.hasFailed())
            return;
        walker.continueIn(asWritten);
        if (Reference object = walker.isUnreachable() ? Reference() : assignAsWrittenFrom(first, properties))
            walker.jumpTo(continuationBlock, singleElementSpan(object));
    };
    assignSourcesFrom(assignSourcesFrom, 0, literal->properties);
    if (walker.hasFailed())
        return true;
    if (continuationBlock) {
        walker.continueIn(continuationBlock);
        object = walker.isUnreachable() ? Reference() : assigned;
    }
    if (object) {
        bindLiteralInWalkCopies(walker, *literal, object);
        walker.bind(value.id(), object);
    }
    return true;
}

namespace {

struct FillOfLiteralInWalkCopies {
    bool isCopy { false };
    SourceOfAssignmentInWalkCopies source;
    FieldIndex field { 0 };
    Reference value;
};

} // anonymous namespace

static bool planFillOfLiteralInWalkCopies(const ObjectCopyInWalkCopies& copy, const LiteralInSightInWalkCopies& literal, ValueID target, const BodyValue& operation, unsigned mostArms, FillOfLiteralInWalkCopies& fill)
{
    Walker& walker = copy.walker;
    if (operation.opcode() == Opcode::CopyDataProperties) {
        auto spread = operation.as<Opcode::CopyDataProperties>();
        if (spread.targetOperand() != target || literal.names.contains(spread.sourceOperand()) || !spread.excludedKeys().isEmpty() || !spread.excludedOperands().empty())
            return false;
        fill.isCopy = true;
        fill.source = SourceOfAssignmentInWalkCopies { spread.sourceOperand(), walker.referenceOf(spread.sourceOperand()), { }, { }, false, false };
        return planSourceOfAssignmentInWalkCopies(copy, fill.source, mostArms, walker.isInLoop(), false);
    }
    auto definition = operation.as<Opcode::PropDefine>();
    if (definition.objectOperand() != target || literal.names.contains(definition.valueOperand()) || !definition.key())
        return false;
    std::optional<FieldReference> found = findField(walker.program(), copy.layoutOfLiteral, definition.key());
    if (!found || found->layout != copy.layoutOfLiteral || !isPlainPropertyInWalkCopies(walker, *found))
        return false;
    fill.field = found->field;
    fill.value = walker.referenceOf(definition.valueOperand());
    if (!fill.value || !fill.value.isRecord())
        return false;
    Type type = walker.types().at(walker.typeOf(fill.value));
    return type.isJavaScriptValue() && !(type.primitives & (Primitive::Absent | Primitive::Unspecified));
}

static ValueID literalThatWaysOfCopyMakeAnewInWalkCopies(Walker& walker, const BodyValue& copy)
{
    if (!walker.isInLoop() || !walker.opensGuesses() || walker.someFrameWalksStretchInsideAnother())
        return { };
    auto spread = copy.as<Opcode::CopyDataProperties>();
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID allocated = spread.targetOperand();
    while (allocated && !index.isParameter(allocated) && inputIdentityOpcodes.contains(index.opcodeOf(allocated)))
        allocated = index.value(allocated).operand(0);
    if (!allocated || index.isParameter(allocated) || index.opcodeOf(allocated) != Opcode::NewObject || index.blockOf(allocated) != index.blockOf(copy.id()) || spread.sourceOperand().index() > allocated.index())
        return { };
    for (uint32_t id = allocated.index() + 1; id < copy.id().index(); ++id) {
        if (!inputIdentityOpcodes.contains(index.opcodeOf(ValueID(id))))
            return { };
    }
    return walker.canThreadBranchFrom(allocated) ? allocated : ValueID();
}

bool walkCopyApartFromSourceWithoutProperties(Walker& walker, const BodyValue& copy)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::EscapeAnalysis))
        return false;
    ValueID input = copy.as<Opcode::CopyDataProperties>().sourceOperand();
    Reference given = walker.referenceOf(input);
    if (!given || !given.isValue())
        return false;
    TypeArena& types = walker.types();
    TypeID whole = walker.typeOf(given);
    TypeID withProperties = types.withoutPrimitives(whole, primitivesWithoutPropertiesToCopy);
    TypeID without = types.differenceOf(whole, withProperties);
    if (types.at(withProperties).isNever() || types.at(without).isNever() || types.testablePartOf(without) != without)
        return false;
    ValueID literal = literalThatWaysOfCopyMakeAnewInWalkCopies(walker, copy);
    if (!literal)
        return false;

    Origin origin = walker.originOf(copy);
    origin.isTailCall = false;
    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(given);
    test.setTestedType(without);
    Reference hasNone = walker.build(WTF::move(test));
    if (!hasNone || !hasNone.isValue())
        return false;
    auto nameSourceOnWay = [&](bool isOnTakenEdge) {
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(given);
        narrowed.setTestOperand(hasNone);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(isOnTakenEdge);
        walker.bind(input, walker.build(WTF::move(narrowed)));
    };

    BlockID somethingToCopy = walker.addBlock();
    walker.count(StatisticID::ObjectSpreadsSplitOnEmptiness);
    BlockID nothingToCopy = walker.addBlock();
    walker.branch(hasNone, nothingToCopy, somethingToCopy);
    for (bool isOnTakenEdge : { true, false }) {
        walker.continueIn(isOnTakenEdge ? nothingToCopy : somethingToCopy);
        nameSourceOnWay(isOnTakenEdge);
        if (!walker.hasFailed() && !walker.threadBranchFrom(literal))
            walker.fail("a way of a copy does not walk the end of its block"_s);
        walker.bind(input, given);
    }
    return true;
}

bool defineOwnKeysOfSourceOnObjectThatStands(Walker& walker, const BodyValue& copy)
{
    auto spread = copy.as<Opcode::CopyDataProperties>();
    if (!walker.has(Mechanism::TypedOperations) || !spread.excludedOperands().empty() || walker.someFrameWalksStretchInsideAnother() || walker.hasStringsInBuilders())
        return false;
    Reference target = walker.referenceOf(spread.targetOperand());
    if (!target || !target.isValue())
        return false;
    ObjectCopyInWalkCopies planned { walker, KindOfCopyInWalkCopies::DefinesOnObjectThatStands, { } };
    SourceOfAssignmentInWalkCopies source;
    source.inBody = spread.sourceOperand();
    source.built = walker.referenceOf(source.inBody);
    if (!planSourceOfAssignmentInWalkCopies(planned, source, maxArmsOfCopiedSource, walker.isInLoop() && walker.opensGuesses(), true) || !source.order().keys.layout)
        return false;
    LayoutID layoutOfTarget = singleLayoutOf(walker.types(), walker.types().at(walker.typeOf(target)));
    bool bringsName = false;
    bool targetListsEveryName = layoutOfTarget && std::ranges::all_of(source.orders, [&](const CopiedOrderInWalkCopies& order) {
        return std::ranges::all_of(order.fields, [&](FieldIndex field) {
            PropertyKeyID key = keyOfFieldInWalkCopies(walker, order.keys.layout, field);
            if (std::ranges::contains(spread.excludedKeys(), key))
                return true;
            bringsName = true;
            std::optional<FieldReference> listed = findField(walker.program(), layoutOfTarget, key);
            return listed && listed->layout == layoutOfTarget && isPlainPropertyInWalkCopies(walker, *listed);
        });
    });
    if (targetListsEveryName && bringsName)
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(copy.id()));
    uint32_t place = copy.id().index() - block.firstValue().index();
    Origin origin = walker.originOf(copy);
    origin.isTailCall = false;
    BlockID asWritten;
    BlockID behind;
    if (!source.isInSight) {
        walker.recordSpeculation();
        asWritten = walker.addBlock(BlockFrequency::Unlikely);
        behind = walker.addBlock();
    }

    bool isCounted = false;
    walkSourceArmsInWalkCopies(walker, source, origin, asWritten, [&](const CopiedOrderInWalkCopies* order, Reference tested) {
        Vector<std::pair<PropertyKeyID, Reference>, 8> definitions;
        for (FieldIndex field : order ? order->fields.span() : std::span<const FieldIndex>()) {
            PropertyKeyID key = keyOfFieldInWalkCopies(walker, order->keys.layout, field);
            if (std::ranges::contains(spread.excludedKeys(), key))
                continue;
            Reference value = valueOfOwnKey(walker, tested, order->keys.layout, field, origin);
            if (!value && !walker.isUnreachable() && !walker.hasFailed())
                walker.fail("an own key that a copy defines has no value, and the way goes on"_s);
            if (!value)
                return;
            definitions.append({ key, value });
        }
        if (!std::exchange(isCounted, true))
            walker.count(StatisticID::ObjectSpreadsExpandedToDefines);
        for (auto [key, value] : definitions) {
            Draft define(Opcode::PropDefine, origin);
            walker.chooseConservativeFields(define);
            define.setObjectOperand(target);
            define.setKey(key);
            define.setValueOperand(value);
            walker.build(WTF::move(define));
            if (walker.isUnreachable() || walker.hasFailed())
                return;
        }
        if (behind)
            walker.jumpTo(behind, { });
    });
    if (walker.hasFailed())
        return true;
    if (behind) {
        walker.continueIn(asWritten);
        {
            Walker::ClosedArm closedArm(walker, true);
            if (!walker.isUnreachable())
                walker.walkStretch(block.id(), place, place + 1);
            if (!walker.isUnreachable() && !walker.hasFailed())
                walker.jumpTo(behind, { });
        }
        if (walker.hasFailed())
            return true;
        walker.continueIn(behind);
    }
    walker.walkStretch(block.id(), place + 1, place + 1);
    return true;
}

bool fillLiteralInSight(Walker& walker, const BodyValue& firstCopy)
{
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    ValueID target = firstCopy.as<Opcode::CopyDataProperties>().targetOperand();
    std::optional<LiteralInSightInWalkCopies> literal = findLiteralInSightInWalkCopies(walker, firstCopy, target);
    if (!literal || literal->keepsItsNames || !isLayoutOfPlainLiteralInWalkCopies(walker.program(), literal->properties.layout))
        return false;
    Reference cell = Reference::value(underlyingCellOf(walker.code(), walker.referenceOf(target).id()));
    if (!walker.state().cellFacts(cell).containsAll({ CellFact::NotFrozen, CellFact::IsExtensible, CellFact::HasLayoutPrototype }))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(firstCopy.id()));
    uint32_t firstValue = block.firstValue().index();
    uint32_t placeOfFirstCopy = firstCopy.id().index() - firstValue;
    ObjectCopyInWalkCopies copy { walker, KindOfCopyInWalkCopies::DefinesOnLiteral, literal->properties.layout };

    Vector<FillOfLiteralInWalkCopies, 4> fills;
    unsigned numberOfArms = 1;
    uint32_t walkedUpTo = placeOfFirstCopy;
    for (uint32_t place = placeOfFirstCopy; place < walker.endOfStretchBeingWalked(); ++place) {
        BodyValue next = index.value(ValueID(firstValue + place));
        if (next.opcode() != Opcode::CopyDataProperties && next.opcode() != Opcode::PropDefine) {
            if (!mayStandBehindCopy(walker, index, next, target))
                break;
            continue;
        }
        walker.walkStretch(block.id(), walkedUpTo, place);
        walkedUpTo = place;
        if (walker.isUnreachable() || walker.hasFailed())
            return true;
        FillOfLiteralInWalkCopies fill;
        if (!planFillOfLiteralInWalkCopies(copy, *literal, target, next, maxArmsOfSourceInWalkCopies(numberOfArms), fill))
            break;
        if (fill.isCopy)
            numberOfArms *= fill.source.numberOfArms();
        fills.append(WTF::move(fill));
        walkedUpTo = place + 1;
    }
    if (fills.isEmpty())
        return false;

    Origin origin = walker.originOf(firstCopy);
    origin.isTailCall = false;
    walker.count(StatisticID::OwnKeyWalksExpanded, static_cast<uint32_t>(fills.size()) - 1);
    bool asksTests = fills.containsIf([](const FillOfLiteralInWalkCopies& fill) {
        return fill.isCopy && !fill.source.isInSight;
    });

    BlockID behindFills;
    Reference filled;
    BlockID asWritten;
    bool leavesCopyOfCallee = asksTests && walker.mayLeaveCopyForCall();
    if (asksTests) {
        walker.recordSpeculation();
        asWritten = leavesCopyOfCallee ? walker.blockWhereCopyLeavesForCall() : walker.addBlock(BlockFrequency::Unlikely);
        if (numberOfArms > 1) {
            behindFills = walker.addBlock();
            filled = walker.addParameter(behindFills, { }, Representation::Cell);
        }
    }

    auto fillAsWritten = [&] {
        walker.continueIn(asWritten);
        Walker::ClosedArm closedArm(walker, true);
        Reference writtenAgain = walker.isUnreachable() ? Reference() : buildLiteralInWalkCopies(walker, origin, literal->properties);
        if (!writtenAgain)
            return;
        bindLiteralInWalkCopies(walker, *literal, writtenAgain);
        for (const FillOfLiteralInWalkCopies& fill : fills) {
            if (Reference madeAgain = fill.isCopy ? makeSourceInSightAgainInWalkCopies(walker, fill.source, origin) : Reference())
                walker.bind(fill.source.inBody, madeAgain);
        }
        walker.walkStretch(block.id(), placeOfFirstCopy, walkedUpTo);
        if (walker.isUnreachable() || walker.hasFailed())
            return;
        if (!behindFills) {
            behindFills = walker.addBlock();
            filled = walker.addParameter(behindFills, { }, Representation::Cell);
        }
        Reference asFilled = walker.referenceOf(target);
        walker.jumpTo(behindFills, singleElementSpan(asFilled));
    };
    bool goesOn = true;
    if (asksTests && numberOfArms == 1) {
        for (FillOfLiteralInWalkCopies& fill : fills) {
            if (goesOn && fill.isCopy && !fill.source.isInSight)
                goesOn = continueIfOwnKeys(walker, fill.source.built, fill.source.order().keys, fill.source.layoutIsProven, origin, asWritten);
        }
        if (walker.hasFailed())
            return true;
        BlockID passed = walker.currentBlock();
        if (!leavesCopyOfCallee)
            fillAsWritten();
        if (walker.hasFailed())
            return true;
        if (goesOn)
            walker.continueIn(passed);
    }

    Reference object;
    auto fillFrom = [&](auto& self, size_t first, PropertiesInWalkCopies properties) -> void {
        while (first < fills.size() && !fills[first].isCopy) {
            properties.put(fills[first].field, fills[first].value);
            ++first;
        }
        if (walker.isUnreachable() || walker.hasFailed())
            return;
        if (first == fills.size()) {
            object = buildLiteralInWalkCopies(walker, origin, properties);
            if (object && behindFills)
                walker.jumpTo(behindFills, singleElementSpan(object));
            return;
        }
        const SourceOfAssignmentInWalkCopies& source = fills[first].source;
        if (numberOfArms == 1) {
            if (takeFieldsOfSourceInWalkCopies(walker, source.order(), source.built, origin, properties))
                self(self, first + 1, WTF::move(properties));
            return;
        }
        walkSourceArmsInWalkCopies(walker, source, origin, asWritten, [&](const CopiedOrderInWalkCopies* order, Reference tested) {
            PropertiesInWalkCopies taken = properties;
            if (!order || takeFieldsOfSourceInWalkCopies(walker, *order, tested, origin, taken))
                self(self, first + 1, WTF::move(taken));
        });
    };
    if (goesOn)
        fillFrom(fillFrom, 0, literal->properties);
    if (walker.hasFailed())
        return true;
    if (asWritten && !leavesCopyOfCallee && numberOfArms > 1)
        fillAsWritten();
    if (walker.hasFailed())
        return true;
    if (behindFills) {
        walker.continueIn(behindFills);
        object = walker.isUnreachable() ? Reference() : filled;
    }
    if (object)
        bindLiteralInWalkCopies(walker, *literal, object);
    walker.walkStretch(block.id(), walkedUpTo, walkedUpTo);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
