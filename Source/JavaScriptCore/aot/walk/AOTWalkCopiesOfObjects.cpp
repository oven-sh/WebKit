/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations, copies of objects: `{ c, ...base }`, `const { a, ...rest } = o`, `{ ...o }`, `Object.assign({ a: 1 }, defaults, options)`. Where the own keys of the source are known by name and in their order,
// the object that is filled is made with them: what it holds by now, then what is copied, in ONE allocation. The object as it was written is built as ever, nothing comes to need it, and the sweep drops it.
// At a suspension: no form goes past one. At a throw: every read comes in front of the allocation and runs no code. Withheld (or EscapeAnalysis, which sees the literal; or Guesses, for a source that is tested): the copy stays as written.

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

// How many closed operations stand for a copy by the run time at the least: it lists the keys, reads each and defines each.
static constexpr uint32_t closedOperationsOfCopyInWalkCopies = 3;

namespace {

enum class KindOfCopyInWalkCopies : uint8_t {
    // CopyDataProperties: each property is defined on a literal.
    DefinesOnLiteral,
    // Object.assign: each property is stored into a literal.
    StoresIntoLiteral,
    // CopyObject.
    MakesObject,
    // CopyObject[assigns].
    MakesObjectByStores,
};

struct ObjectCopyInWalkCopies {
    bool makesObject() const { return kind == KindOfCopyInWalkCopies::MakesObject || kind == KindOfCopyInWalkCopies::MakesObjectByStores; }
    // Object.assign stores each property, where a spread defines it.
    bool stores() const { return kind == KindOfCopyInWalkCopies::StoresIntoLiteral || kind == KindOfCopyInWalkCopies::MakesObjectByStores; }
    bool excludes(PropertyKeyID key) const { return excludedKeys.contains(key); }

    Walker& walker;
    KindOfCopyInWalkCopies kind;
    // Null: the copy makes its object, of the layout of its source.
    LayoutID layoutOfLiteral;
    // What the operation names, and each computed key that is one key in every run.
    Vector<PropertyKeyID, 8> excludedKeys;
};

struct CopiedOrderInWalkCopies {
    // What the tests of an arm ask. Without a layout: nothing is copied.
    OwnKeyOrder keys;
    // In the order in which the source got them.
    Vector<FieldIndex, 8> fields;
    // By entry of `fields`: the field under the same key in the object that takes it.
    Vector<FieldIndex, 8> fieldsOfTaker;
};

// What a literal holds, in the order in which it got its keys.
struct PropertiesInWalkCopies {
    // A key that is there already keeps its place and takes the new value.
    void put(FieldIndex field, Reference value)
    {
        size_t place = fields.find(field);
        if (place != notFound) {
            values[place] = value;
            return;
        }
        fields.append(field);
        values.append(value);
    }

    LayoutID layout;
    Vector<FieldIndex, 8> fields;
    Vector<Reference, 8> values;
};

struct LiteralInSightInWalkCopies {
    // Of the body: the operand, whatever hands the object on to it, and the allocation.
    Vector<ValueID, 4> names;
    PropertiesInWalkCopies properties;
};

} // anonymous namespace

static PropertyKeyID keyOfFieldInWalkCopies(const Walker& walker, LayoutID layout, FieldIndex field)
{
    return walker.plan().slots().keyOf(walker.program(), { layout, field });
}

// The bits of its slot are all there is to the property, a copy takes it, and an allocation may list it or leave it to a definition by name.
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

// The pattern converts each computed key where it is written, in front of the copy. False: some key is known only when the program runs.
static bool collectExcludedKeysInWalkCopies(Walker& walker, const auto& namedKeys, std::span<const ValueID> computedKeys, Vector<PropertyKeyID, 8>& excludedKeys)
{
    for (PropertyKeyID key : namedKeys)
        excludedKeys.append(key);
    const Code& code = walker.code();
    for (ValueID computed : computedKeys) {
        Reference key = walker.referenceOf(computed);
        if (!key || !key.isRecord())
            return false;
        // A text is its own key.
        ValueID record = key.id();
        if (!code.isConstant(record) && code.at(record).opcode == Opcode::ToPropertyKey)
            record = code.at(record).operand(0);
        if (!code.isConstant(record))
            return false;
        const Operation& constant = code.at(record);
        if (constant.opcode == Opcode::String)
            excludedKeys.append(walker.types().textKey(constant.string()));
        else if (constant.opcode == Opcode::KeyConstant)
            excludedKeys.append(constant.key());
        else
            return false;
    }
    return true;
}

static bool isMadeWithoutPropertiesInWalkCopies(const Walker& walker, const BodyValue& made)
{
    if (made.opcode() == Opcode::NewDictionary)
        return true;
    if (made.opcode() != Opcode::Data)
        return false;
    const auto* object = walker.program().data[made.as<Opcode::Data>().data().index()].root.tryAs<DataNode::Object>();
    return object && object->members.isEmpty();
}

// `const none = {}; return { ...none, [key]: value }`: the object was made by the body without a property, and nothing but this copy ever names it.
static bool sourceIsEmptyDictionaryInWalkCopies(const Walker& walker, ValueID copy, ValueID source)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID value = index.valueBehindCasts(source);
    if (!value || index.isParameter(value) || !isMadeWithoutPropertiesInWalkCopies(walker, index.value(value)))
        return false;
    while (value && value != copy)
        value = index.onlyUsingValue(value);
    return value == copy;
}

// A store into a plain object runs no code, and defines, where nothing above the object answers to the key. While the object stands where its layout says, only Object.prototype stands above it, which gets no name.
// Whoever asks has seen that it stands there: the allocation in sight was handed no prototype, and the state has the link of the cell.
static bool onlyObjectPrototypeStandsAboveObjectsOfInWalkCopies(const Walker& walker, LayoutID layout)
{
    const Layout& record = walker.program().at(layout);
    return record.kind == LayoutKind::Record && !record.constructor && !record.has(LayoutFlags::IsConstructorFunction) && !record.has(LayoutFlags::IsOfCreatedObject);
}

// What is asked of the key of each field that is copied. It answers with the field that takes the value.
static std::optional<FieldIndex> findFieldThatTakesInWalkCopies(const ObjectCopyInWalkCopies& copy, LayoutID layoutOfSource, FieldIndex field, PropertyKeyID key)
{
    const Walker& walker = copy.walker;
    const Program& program = walker.program();
    LayoutID taker = copy.makesObject() ? layoutOfSource : copy.layoutOfLiteral;
    // A test run opens names on Object.prototype, and a setter that it lays there takes the store.
    if (copy.stores() && (isOnObjectPrototype(program.nameClassOf(key)) || !isClosedKey(program, program.builtins().canonicalID(Builtins::ObjectPrototype), key) || !onlyObjectPrototypeStandsAboveObjectsOfInWalkCopies(walker, taker)))
        return std::nullopt;
    // An allocation lists declared fields.
    if (copy.makesObject())
        return isPlainPropertyInWalkCopies(walker, { layoutOfSource, field }) ? std::optional(field) : std::nullopt;
    std::optional<FieldReference> found = findField(program, taker, key);
    if (!found || found->layout != taker || !isPlainPropertyInWalkCopies(walker, *found))
        return std::nullopt;
    return found->field;
}

// A copy takes the properties under symbols too, behind or among the names. One key of the build may stand for a symbol of each run, so no allocation can list such a field for another object.
// False: the object may have an own property under a symbol that the copy would have to take, or an accessor, through which the engine's own copy goes as [[OwnPropertyKeys]] lists its keys.
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
        if (isSymbolKind(program.at(key).kind) && !form.contains(FieldForm::IsNotEnumerable) && !copy.excludes(key))
            return false;
    }
    return true;
}

// Whoever reads a field that every construction fills reads it without a look, on the word that every allocation of the layout lists it.
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

// `isInSight`: the state knows the keys, and no test stands in front of the reads. It builds nothing.
static bool fillCopiedOrderInWalkCopies(const ObjectCopyInWalkCopies& copy, const OwnKeyOrder& listed, bool isInSight, CopiedOrderInWalkCopies& order)
{
    const Walker& walker = copy.walker;
    const Program& program = walker.program();
    order.keys = listed;
    if (!listed.layout)
        return listed.fields.isEmpty();
    if (!isInSight && listed.fields.size() > mostFieldsOfOwnKeyWalk + copy.excludedKeys.size())
        return false;

    const Layout& layout = program.at(listed.layout);
    // What a copy makes is an ordinary object below Object.prototype, as an allocation of a record's layout is.
    if (copy.makesObject() && layout.kind != LayoutKind::Record)
        return false;

    for (FieldIndex field : listed.fields) {
        PropertyKeyID key = keyOfFieldInWalkCopies(walker, listed.layout, field);
        if (!key)
            return false;
        if (copy.excludes(key))
            continue;
        // Set(target, "__proto__", value) gives the target a prototype, whose setters would then run between the reads of the source.
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

// The arms of a plan that the copy can take. Empty: the copy stays as written.
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

// ---- The literal that is filled

// The object that the operand names was allocated by the body, a few operations further up in the block, under the layout of a record. Nothing has named it since but what defines a name on it, copies into it or hands it on to the operand,
// nothing but this code has it, and the state knows what it holds. So an object that is made anew where the walk stands, with what this one holds, is this one to whoever comes later.
static std::optional<LiteralInSightInWalkCopies> findLiteralInSightInWalkCopies(Walker& walker, const BodyValue& user, ValueID operand)
{
    const BodyIndex& index = walker.currentBodyIndex();
    const Program& program = walker.program();
    LiteralInSightInWalkCopies literal;
    ValueID allocated = operand;
    while (allocated && !index.isParameter(allocated) && inputOperationsThatOnlyHandOn.contains(index.opcodeOf(allocated))) {
        literal.names.append(allocated);
        allocated = index.value(allocated).operand(0);
    }
    if (!allocated || index.isParameter(allocated) || index.opcodeOf(allocated) != Opcode::NewObject || index.blockOf(allocated) != index.blockOf(user.id()))
        return std::nullopt;
    literal.names.append(allocated);
    auto allocation = index.value(allocated).as<Opcode::NewObject>();
    LayoutID layout = allocation.layout();
    if (allocation.prototypeOperand() || program.at(layout).kind != LayoutKind::Record || !walker.plan().slots().holdsCompiledObjects(layout))
        return std::nullopt;

    // The index counts the mentions of a value up to a limit of its own. So a literal is gone through for no more users than that.
    for (ValueID name : literal.names) {
        if (index.useCount(name) == BodyIndex::maxCountedUses)
            return std::nullopt;
    }
    Vector<uint32_t, 4> mentions;
    mentions.fill(0, literal.names.size());
    for (uint32_t id = allocated.index() + 1; id < user.id().index(); ++id) {
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
    // Whoever comes later names the operand: every other name is bound to the object as it was written.
    for (size_t place = 0; place < literal.names.size(); ++place) {
        if (literal.names[place] != operand && mentions[place] != index.useCount(literal.names[place]))
            return std::nullopt;
    }

    Reference built = walker.referenceOf(operand);
    if (!built || !built.isValue())
        return std::nullopt;
    ValueID cell = rootOfCellInSweptCode(walker.code(), built.id());
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
    for (ValueID name : literal.names)
        walker.bind(name, object);
}

// Every field is a plain value in its slot, so nothing can tell that all reads come first. False: the walk stands nowhere.
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

// ---- The arms of a copy that goes by the shape of its source and by the order of its keys

// A failed test leads to the next arm that may still take the object, and else to the copy as written, which is built once behind all arms. `buildArm(order, source)` and `buildAsWritten()` answer with the object that is filled or made.
// It answers with that object where the arms have met. Null: the walk stands nowhere.
static Reference buildArmsInWalkCopies(Walker& walker, Reference source, std::span<const CopiedOrderInWalkCopies> orders, bool layoutsAreProven, Origin origin, NOESCAPE const Invocable<Reference(const CopiedOrderInWalkCopies&, Reference)> auto& buildArm, NOESCAPE const Invocable<Reference()> auto& buildAsWritten)
{
    walker.noteGuessOpened();
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
        if (goOnWhereObjectHasOwnKeys(walker, object, orders[index].keys, layoutsAreProven || armInFrontHasSameLayout, origin, waysOut)) {
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

// ---- { c, ...source }, const { a, ...rest } = source

bool buildCopyOfProperties(Walker& walker, const BodyValue& value)
{
    if (value.opcode() != Opcode::CopyDataProperties || !walker.has(Mechanism::TypedOperations))
        return false;
    auto operation = value.as<Opcode::CopyDataProperties>();
    if (sourceIsEmptyDictionaryInWalkCopies(walker, value.id(), operation.sourceOperand()))
        return true;

    Reference source = walker.referenceOf(operation.sourceOperand());
    ObjectCopyInWalkCopies copy { walker, KindOfCopyInWalkCopies::DefinesOnLiteral, { }, { } };
    if (!source || !source.isValue() || !collectExcludedKeysInWalkCopies(walker, operation.excludedKeys(), operation.excludedOperands(), copy.excludedKeys))
        return false;
    std::optional<LiteralInSightInWalkCopies> literal = findLiteralInSightInWalkCopies(walker, value, operation.targetOperand());
    if (!literal)
        return false;
    copy.layoutOfLiteral = literal->properties.layout;
    Origin origin = walker.originOf(value);

    if (std::optional<OwnKeyOrder> listed = ownKeysInSight(walker, source)) {
        CopiedOrderInWalkCopies order;
        if (fillCopiedOrderInWalkCopies(copy, *listed, true, order)) {
            if (order.fields.isEmpty())
                return true;
            if (takeFieldsOfSourceInWalkCopies(walker, order, source, origin, literal->properties)) {
                if (Reference object = buildLiteralInWalkCopies(walker, origin, literal->properties))
                    bindLiteralInWalkCopies(walker, *literal, object);
            }
            return true;
        }
    }

    OwnKeyPlan plan = planOwnKeyWalk(walker, operation.sourceOperand(), source);
    Vector<CopiedOrderInWalkCopies, 2> orders = planCopyByLayoutsInWalkCopies(copy, plan);
    if (orders.isEmpty() || !shouldOpenGuess(walker, source, walker.types().forLayout(orders[0].keys.layout), plan.isDeclared, closedOperationsOfCopyInWalkCopies))
        return false;
    Draft written = walker.draftOf(value);
    if (walker.hasFailed())
        return true;

    Reference filled = buildArmsInWalkCopies(walker, source, orders.span(), plan.layoutsAreProven, origin, [&](const CopiedOrderInWalkCopies& order, Reference tested) -> Reference {
        PropertiesInWalkCopies properties = literal->properties;
        if (!takeFieldsOfSourceInWalkCopies(walker, order, tested, origin, properties))
            return { };
        return buildLiteralInWalkCopies(walker, origin, properties);
    }, [&]() -> Reference {
        Reference object = buildLiteralInWalkCopies(walker, origin, literal->properties);
        if (!object)
            return { };
        // A getter of the source may throw, and whoever catches that finds this object under the names of the literal.
        bindLiteralInWalkCopies(walker, *literal, object);
        written.setTargetOperand(object);
        walker.chooseConservativeFields(written);
        walker.build(WTF::move(written));
        return walker.isUnreachable() ? Reference() : object;
    });
    if (filled)
        bindLiteralInWalkCopies(walker, *literal, filled);
    return true;
}

// ---- { ...source } where no type says which names the object will have

static Reference buildObjectMadeByCopyInWalkCopies(Walker& walker, const Draft& written, const BodyValue& value, const CopiedOrderInWalkCopies& order, Reference source)
{
    Origin origin = written.origin();
    // The copy of nothing is the ordinary object that the operation as written makes.
    if (!order.keys.layout || order.fields.isEmpty()) {
        Draft empty(Opcode::NewDictionary, origin);
        empty.inputValueGiven = value.id();
        empty.expected = written.expected;
        empty.resultIsReturnedAtOnce = written.resultIsReturnedAtOnce;
        walker.chooseConservativeFields(empty);
        return walker.build(WTF::move(empty));
    }

    // The copy gets its fields in the order in which the source got them, so it has the order word of its source.
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
    ObjectCopyInWalkCopies copy { walker, operation.assigns() ? KindOfCopyInWalkCopies::MakesObjectByStores : KindOfCopyInWalkCopies::MakesObject, { }, { } };

    if (sourceIsEmptyDictionaryInWalkCopies(walker, value.id(), operation.sourceOperand())) {
        Draft written = walker.draftOf(value);
        if (Reference made = walker.hasFailed() ? Reference() : buildObjectMadeByCopyInWalkCopies(walker, written, value, { }, source))
            walker.bind(value.id(), made);
        return true;
    }
    // An allocation that leaves fields out need not be one that the layout allows.
    if (!source.isValue() || !operation.excludedKeys().isEmpty() || !operation.excludedOperands().empty())
        return false;

    if (std::optional<OwnKeyOrder> listed = ownKeysInSight(walker, source)) {
        CopiedOrderInWalkCopies order;
        if (fillCopiedOrderInWalkCopies(copy, *listed, true, order)) {
            Draft written = walker.draftOf(value);
            if (Reference made = walker.hasFailed() ? Reference() : buildObjectMadeByCopyInWalkCopies(walker, written, value, order, source))
                walker.bind(value.id(), made);
            return true;
        }
    }

    OwnKeyPlan plan = planOwnKeyWalk(walker, operation.sourceOperand(), source);
    Vector<CopiedOrderInWalkCopies, 2> orders = planCopyByLayoutsInWalkCopies(copy, plan);
    if (orders.isEmpty() || !shouldOpenGuess(walker, source, walker.types().forLayout(orders[0].keys.layout), plan.isDeclared, closedOperationsOfCopyInWalkCopies))
        return false;
    Draft written = walker.draftOf(value);
    if (walker.hasFailed())
        return true;
    // Each arm hands its object to the block in which they meet, so none of them is what the unit returns at once.
    written.resultIsReturnedAtOnce = false;
    written.isTailCall = false;

    Reference made = buildArmsInWalkCopies(walker, source, orders.span(), plan.layoutsAreProven, written.origin(), [&](const CopiedOrderInWalkCopies& order, Reference tested) {
        return buildObjectMadeByCopyInWalkCopies(walker, written, value, order, tested);
    }, [&] {
        Draft asWritten = written;
        walker.chooseConservativeFields(asWritten);
        return walker.build(WTF::move(asWritten));
    });
    if (made)
        walker.bind(value.id(), made);
    return true;
}

// ---- Object.assign(literal, source, ..)

// A member that is no field of the layout may be on its way to the literal: an accessor, a member under a computed key.
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
    ValueID inBody;
    Reference built;
    // With `isInSight`: the one order. Else the first arm of the plan.
    CopiedOrderInWalkCopies order;
    bool isInSight { false };
    bool layoutIsProven { false };
};

} // anonymous namespace

// It builds nothing. False: the call takes the source as written, and every later one with it.
static bool planSourceOfAssignmentInWalkCopies(const ObjectCopyInWalkCopies& copy, SourceOfAssignmentInWalkCopies& source)
{
    Walker& walker = copy.walker;
    if (!source.built || !source.built.isValue())
        return false;
    if (std::optional<OwnKeyOrder> listed = ownKeysInSight(walker, source.built)) {
        if (fillCopiedOrderInWalkCopies(copy, *listed, true, source.order)) {
            source.isInSight = true;
            return true;
        }
        source.order = { };
    }
    OwnKeyPlan plan = planOwnKeyWalk(walker, source.inBody, source.built);
    Vector<CopiedOrderInWalkCopies, 2> orders = planCopyByLayoutsInWalkCopies(copy, plan);
    if (orders.isEmpty() || !shouldOpenGuess(walker, source.built, walker.types().forLayout(orders[0].keys.layout), plan.isDeclared, closedOperationsOfCopyInWalkCopies))
        return false;
    source.order = WTF::move(orders[0]);
    source.layoutIsProven = plan.layoutsAreProven;
    return true;
}

// The literal has no `__proto__` member and no accessor, and it is as it was made. So it stands right below Object.prototype, and only a name that is answered there can make a store into it run code.
// The stores of a planned source add plain properties to the literal, so the next source finds it as plain as the first did.
// A source that is assigned as written may give the literal a prototype with setters, so the call that assigns it assigns every later source too.
bool assignSourcesFieldByField(Walker& walker, const BodyValue& value)
{
    if (value.opcode() != Opcode::Call || !walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::TypedOperations))
        return false;
    auto call = value.as<Opcode::Call>();
    std::span<const ValueID> arguments = call.argumentsOperands();
    Reference callee = call.isDoneInPlace() || arguments.size() < 2 ? Reference() : walker.referenceOf(call.calleeOperand());
    if (!callee)
        return false;
    Type typeOfCallee = walker.types().at(walker.typeOf(callee));
    BuiltinID builtin = typeOfCallee.isObject() ? singleBuiltinOf(walker.types(), typeOfCallee) : BuiltinID();
    if (!builtin || walker.program().builtins().canonicalID(builtin) != Builtins::ObjectAssign)
        return false;

    std::optional<LiteralInSightInWalkCopies> literal = findLiteralInSightInWalkCopies(walker, value, arguments[0]);
    if (!literal || !isLayoutOfPlainLiteralInWalkCopies(walker.program(), literal->properties.layout))
        return false;
    Reference cell = Reference::value(rootOfCellInSweptCode(walker.code(), walker.referenceOf(arguments[0]).id()));
    if (!walker.state().factsOfCell(cell).containsAll({ FactOfCell::NotFrozen, FactOfCell::HasLayoutPrototype }))
        return false;

    ObjectCopyInWalkCopies copy { walker, KindOfCopyInWalkCopies::StoresIntoLiteral, literal->properties.layout, { } };
    Vector<SourceOfAssignmentInWalkCopies, 4> planned;
    for (ValueID argument : arguments.subspan(1)) {
        // The literal itself among the sources is read while it is filled.
        SourceOfAssignmentInWalkCopies source { argument, literal->names.contains(argument) ? Reference() : walker.referenceOf(argument), { }, false, false };
        if (!planSourceOfAssignmentInWalkCopies(copy, source))
            break;
        planned.append(WTF::move(source));
    }
    if (planned.isEmpty())
        return false;

    Draft written = walker.draftOf(value);
    if (walker.hasFailed())
        return true;
    // The call gives the literal, whoever assigns: nothing that is built here is what the unit returns at once.
    written.resultIsReturnedAtOnce = false;
    written.isTailCall = false;
    Origin origin = written.origin();
    Vector<Reference, 4> operandsAsWritten(written.argumentsOperands());

    // The call with the literal as it stands, this source and every later one.
    auto assignAsWrittenFrom = [&](size_t firstSource, const PropertiesInWalkCopies& properties) -> Reference {
        Reference object = buildLiteralInWalkCopies(walker, origin, properties);
        if (!object)
            return { };
        // A getter of a source may throw half way, and whoever catches that finds this object, with what it has got by then, under the names of the literal.
        bindLiteralInWalkCopies(walker, *literal, object);
        Vector<Reference, 4> operands;
        operands.append(object);
        operands.append(operandsAsWritten.span().subspan(1 + firstSource));
        Draft rest = written;
        rest.setArgumentsOperands(operands.span());
        walker.chooseConservativeFields(rest);
        walker.build(WTF::move(rest));
        return walker.isUnreachable() ? Reference() : object;
    };

    BlockID behindCall;
    Reference assigned;
    PropertiesInWalkCopies properties = literal->properties;
    bool goesOn = true;
    for (size_t index = 0; index < planned.size() && goesOn; ++index) {
        SourceOfAssignmentInWalkCopies& source = planned[index];
        Reference tested = source.built;
        if (!source.isInSight) {
            if (!behindCall) {
                walker.noteGuessOpened();
                behindCall = walker.addBlock();
                assigned = walker.addParameter(behindCall, { }, Representation::Cell);
            }
            BlockID asWritten = walker.addBlock(BlockFrequency::Unlikely);
            goesOn = goOnWhereObjectHasOwnKeys(walker, tested, source.order.keys, source.layoutIsProven, origin, asWritten);
            BlockID whereTestsHavePassed = walker.currentBlock();
            walker.continueIn(asWritten);
            if (Reference object = walker.isUnreachable() ? Reference() : assignAsWrittenFrom(index, properties))
                walker.jumpTo(behindCall, singleElementSpan(object));
            if (walker.hasFailed())
                return true;
            if (!goesOn)
                break;
            walker.continueIn(whereTestsHavePassed);
        }
        goesOn = takeFieldsOfSourceInWalkCopies(walker, source.order, tested, origin, properties);
    }

    Reference object;
    if (goesOn)
        object = planned.size() + 1 < arguments.size() ? assignAsWrittenFrom(planned.size(), properties) : buildLiteralInWalkCopies(walker, origin, properties);
    if (walker.hasFailed())
        return true;
    if (behindCall) {
        if (object)
            walker.jumpTo(behindCall, singleElementSpan(object));
        walker.continueIn(behindCall);
        object = walker.isUnreachable() ? Reference() : assigned;
    }
    if (object) {
        bindLiteralInWalkCopies(walker, *literal, object);
        walker.bind(value.id(), object);
    }
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
