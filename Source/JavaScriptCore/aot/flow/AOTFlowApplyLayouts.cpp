/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApply.h"

#if ENABLE(AOT)

#include "AOTFlowSiteData.h"
#include "AOTSlotRepresentation.h"
#include "AOTThreads.h"
#include "AOTVisibleStores.h"
#include <algorithm>
#include <array>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// The integers that a slot of four bytes holds beside its markers, with and without undefined and null.
struct InlineIntegerTypes {
    InlineIntegerTypes(Program&, const ParallelLoops&);

    TypeID withNullish(bool withUndefined, bool withNull) const { return byNullish[(withUndefined ? 1 : 0) | (withNull ? 2 : 0)]; }
    // An allocation writes its initial values without a test. And where flow proves of the objects of a layout that no store brings another type, what it proves of them rests on the types it saw.
    bool mustKeepType(LayoutID layout, uint32_t field) const { return fieldsInitializedWithOtherValues[layout.index()].get(field) || layoutsNeverStoredOtherTypes.get(layout.index()); }

    std::array<TypeID, 4> byNullish;
    Vector<BitVector> fieldsInitializedWithOtherValues;
    BitVector layoutsNeverStoredOtherTypes;
    VisibleStores visibleStores;
};

struct FamilyMemberType {
    Type type;
    Type predicted;
    OptionSet<StoredKind> stored;
};

// By layout, for each name that flow appends to it.
using FamilyMemberTypes = Vector<Vector<std::optional<FamilyMemberType>>>;

} // anonymous namespace

static bool isInlineInt32OrNullish(const DataNode& node)
{
    if (const DataNode::Number* number = node.tryAs<DataNode::Number>()) {
        std::optional<int32_t> integer = tryConvertToStrictInt32(number->value);
        return integer && *integer >= minInlineInt32;
    }
    return node.is<DataNode::Undefined>() || node.is<DataNode::Null>();
}

// An allocation writes its initial values without a test, and so does the creation of constant data.
static Vector<BitVector> findFieldsInitializedOutside(const Program& program, const Type& admitted)
{
    Vector<BitVector> found(program.layouts.size());
    auto addField = [&](FieldReference field) {
        FieldReference declaration = declarationOf(program, field);
        found[declaration.layout.index()].set(declaration.field);
    };
    for (const Data& data : program.data) {
        forEachDataNodeInPreorder(data.root, [&](uint32_t, const DataNode& node) {
            const DataNode::Object* object = node.tryAs<DataNode::Object>();
            if (!object)
                return;
            for (const DataMember& member : object->members) {
                std::optional<uint32_t> field = program.at(object->layout).findField(member.key);
                if (field && !isInlineInt32OrNullish(member.value))
                    addField({ object->layout, *field });
            }
        });
    }
    for (const Function& function : program.functions) {
        for (const Value& value : function.values) {
            if (value.opcode != Opcode::NewObject && value.opcode != Opcode::CloneAs)
                continue;
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                std::optional<uint32_t> initialValue = value.variadicOperandIndex(operandIndex);
                if (!initialValue || function.at(operand).type == TypeID() || program.at(function.at(operand).type).isSubsetOf(admitted, program))
                    return;
                addField({ value.layout(), value.fields()[*initialValue] });
            });
        }
    }
    return found;
}

InlineIntegerTypes::InlineIntegerTypes(Program& program, const ParallelLoops&)
{
    for (unsigned index = 0; index < byNullish.size(); ++index) {
        uint16_t nullish = (index & 1 ? Primitive::Undefined : 0) | (index & 2 ? Primitive::Null : 0);
        byNullish[index] = program.types.intern(Type::integer({ minInlineInt32, INT32_MAX }).withPrimitives(nullish));
    }
    fieldsInitializedWithOtherValues = findFieldsInitializedOutside(program, program.at(byNullish[3]));
}

// What flow proves of objects it proves by allocation class, and the set of a layout is what holds for all of its classes. The objects of a layout have the fields of the layouts above it too.
static BitVector findLayoutsNeverStoredOtherTypes(const Program& program, const FlowResults& results, const Vector<FlowLayoutUpdate>& updates)
{
    auto baseOf = [&](LayoutID id) {
        const LayoutFlow& flow = updates[id.index()].flow;
        if (flow.baseState == BaseState::Layout)
            return flow.base;
        return flow.baseState == BaseState::Unknown ? program.at(id).predictedBase : LayoutID();
    };
    BitVector found(updates.size());
    for (size_t index = 0; index < updates.size(); ++index) {
        bool hasClassNeverStoredOtherTypes = std::ranges::any_of(updates[index].flow.allocationClasses, [&](AllocationClassID allocationClass) {
            return results.at(allocationClass).impossibleMutations.contains(ObjectMutation::StoreOfOtherType);
        });
        if (!hasClassNeverStoredOtherTypes)
            continue;
        for (LayoutID current(index); current && !found.quickSet(current.index()); current = baseOf(current)) { }
    }
    return found;
}

static bool showsOnlyInt32s(OptionSet<StoredKind> stored)
{
    return stored.contains(StoredKind::Int32) && !stored.contains(StoredKind::Other);
}

// The type of the int32s that the field is expected to hold: by the type that is written for it, or, where flow does not see every writer, by the stores that the program shows.
static TypeID findExpectedIntegers(const Program& program, const InlineIntegerTypes& integers, const Field& field, OptionSet<StoredKind> stored)
{
    const Type& type = program.at(field.declared.type);
    bool holdsNumbers = type.isNumberOrNullish() && type.primitives & Primitive::Double;
    if (holdsNumbers && field.declared.predicted) {
        const Type& predicted = program.at(field.declared.predicted);
        if (predicted.primitives & Primitive::Integer && predicted.isInt32OrNullish() && !(predicted.primitives & Primitive::Absent))
            return integers.withNullish(predicted.primitives & Primitive::Undefined, predicted.primitives & Primitive::Null);
    }
    if ((holdsNumbers || type.isAnything()) && showsOnlyInt32s(stored) && field.flow.location.writers != WriterState::AllKnown)
        return integers.withNullish(stored.contains(StoredKind::Undefined), stored.contains(StoredKind::Null));
    return { };
}

// The slot of a field that is expected to hold int32s holds nothing else. Any other value is a value of another type: the field leaves its slot.
// Where flow proves that no store brings another type, the proofs about the field and its layout rest on that: the field never leaves its slot. Its type stays as flow gives it.
static void keepExpectedIntegersInSlot(const Program& program, const InlineIntegerTypes& integers, const Layout& layout, Field& field, OptionSet<StoredKind> stored, bool mustKeepType)
{
    if (mustKeepType || !isObjectLayout(layout.kind) || field.flags.contains(FieldFlags::IsInherited) || field.flow.stability.contains(FieldStability::IsNeverStoredWithOtherType))
        return;
    TypeID expected = findExpectedIntegers(program, integers, field, stored);
    if (!expected)
        return;

    if (!field.declared.written)
        field.declared.written = field.declared.type;
    field.declared.type = expected;
    if (field.declared.predicted && !program.at(field.declared.predicted).isSubsetOf(program.at(expected), program))
        field.declared.predicted = { };
}

static void applyFieldUpdates(const Program& program, const InlineIntegerTypes& integers, LayoutID id, Layout& layout, const FlowLayoutUpdate& update)
{
    RELEASE_ASSERT(update.fields.size() == layout.fields.size());
    for (size_t index = 0; index < layout.fields.size(); ++index) {
        Field& field = layout.fields[index];
        const FlowFieldUpdate& fieldUpdate = update.fields[index];
        applyPosition(field.declared, fieldUpdate.position);
        field.flow = fieldUpdate.flow;

        bool wasAnalyzed = field.flow.location.writers == WriterState::AllKnown;
        if (wasAnalyzed && !field.flow.stability.contains(FieldStability::IsNeverDeleted))
            field.flags.remove(FieldFlags::IsRequired);
        keepExpectedIntegersInSlot(program, integers, layout, field, integers.visibleStores.kindsStoredIn(id, static_cast<uint32_t>(index)), integers.mustKeepType(id, static_cast<uint32_t>(index)));
    }
}

static void applyLayoutUpdate(const Program& program, const InlineIntegerTypes& integers, LayoutID id, Layout& layout, const FlowLayoutUpdate& update)
{
    layout.flow = update.flow;
    layout.classFlow = update.classFlow;
    applyFieldUpdates(program, integers, id, layout, update);
}

static void uniteTypesDeclaredIn(const Program& program, const VisibleStores& visibleStores, LayoutSetID family, PropertyKeyID key, std::optional<FamilyMemberType>& united, TypeArena& arena)
{
    for (LayoutID member : program.at(family).members) {
        std::optional<uint32_t> index = program.at(member).findField(key);
        if (!index)
            continue;
        const Field& field = fieldAt(program, declarationOf(program, { member, *index }));
        const Type& type = program.at(field.declared.type);
        Type predicted = field.declared.predicted ? program.at(field.declared.predicted) : type;
        if (!field.flags.contains(FieldFlags::IsRequired))
            predicted = predicted.withPrimitives(type.primitives & Primitive::Undefined);
        OptionSet<StoredKind> stored = visibleStores.kindsStoredIn(member, *index);
        if (united)
            united = FamilyMemberType { united->type.unionWith(type, arena), united->predicted.unionWith(predicted, arena), united->stored | stored };
        else
            united = FamilyMemberType { type, WTF::move(predicted), stored };
    }
}

static std::optional<FamilyMemberType> typeDeclaredInFamily(const Program& program, const VisibleStores& visibleStores, LayoutID id, PropertyKeyID key, TypeArena& arena)
{
    std::optional<FamilyMemberType> declared;
    for (LayoutSetID family : declaredFamiliesOf(program, id))
        uniteTypesDeclaredIn(program, visibleStores, family, key, declared, arena);
    if (!declared || declared->type.isAnything())
        return std::nullopt;
    return declared;
}

// Looks at the fields as they were read: what flow proves of the objects of one layout says nothing about those of another.
static FamilyMemberTypes findTypesDeclaredInFamilies(const Program& program, const VisibleStores& visibleStores, const Vector<FlowLayoutUpdate>& updates, TypeArena& arena)
{
    FamilyMemberTypes types(updates.size());
    for (size_t index = 0; index < updates.size(); ++index) {
        types[index] = WTF::map(updates[index].appendedFields, [&](const FlowAppendedField& appended) {
            return typeDeclaredInFamily(program, visibleStores, LayoutID(index), appended.key, arena);
        });
    }
    return types;
}

// The slot of a name that flow appends holds what the program's types declare for the name. A store that they do not admit is a store of another type.
static void applyTypeDeclaredInFamily(const Program& program, TypeArena& arena, const std::optional<FamilyMemberType>& declared, Field& field, bool mustKeepType)
{
    if (!declared || mustKeepType || field.flow.stability.contains(FieldStability::IsNeverStoredWithOtherType))
        return;
    if (!program.at(field.declared.type).isSubsetOf(declared->type.withPrimitives(Primitive::Absent), program))
        field.declared.type = arena.intern(declared->type);
    Type type = program.at(field.declared.type);
    if (field.declared.predicted && !program.at(field.declared.predicted).isSubsetOf(type, program))
        field.declared.predicted = { };
    if (!field.declared.predicted && declared->predicted != type && declared->predicted.isSubsetOf(type, program))
        field.declared.predicted = arena.intern(declared->predicted);
}

static void appendFlowFields(const Program& program, const InlineIntegerTypes& integers, TypeArena& arena, std::span<const std::optional<FamilyMemberType>> declaredInFamily, LayoutID id, Layout& layout, const FlowLayoutUpdate& update)
{
    bool mustKeepTypes = integers.layoutsNeverStoredOtherTypes.get(id.index());
    for (size_t index = 0; index < update.appendedFields.size(); ++index) {
        const FlowAppendedField& appended = update.appendedFields[index];
        RELEASE_ASSERT(appended.position.type && !layout.findField(appended.key));
        Field field;
        field.key = appended.key;
        field.declared.type = appended.position.type;
        field.declared.predicted = appended.position.predicted;
        field.flags = { FieldFlags::IsAppendedByFlow };
        field.flow = appended.flow;
        applyTypeDeclaredInFamily(program, arena, declaredInFamily[index], field, mustKeepTypes);
        keepExpectedIntegersInSlot(program, integers, layout, field, declaredInFamily[index] ? declaredInFamily[index]->stored : OptionSet<StoredKind> { }, mustKeepTypes);
        // A number takes eight bytes boxed or not, and boxed an int32 stays one for whoever reads it by name.
        const Type& type = program.at(field.declared.type);
        if (type.isNumberOrNullish() && type.primitives & Primitive::Double) {
            field.declared.type = appended.position.type;
            field.declared.predicted = appended.position.predicted;
        }
        layout.appendField(WTF::move(field));
    }
}

// A key that is no field goes to the extra properties, and no object with extra properties has one of the orders.
static void appendLiteralKeyOrders(Layout& layout, const FlowLayoutUpdate& update)
{
    // Lists of different keys are lists of different fields, so an order can only repeat one that was listed by index.
    size_t numberOfIndexOrders = layout.flow.constructorOrders.size();
    for (const Vector<PropertyKeyID>& keys : update.literalKeyOrders) {
        FieldOrder order;
        bool listsOnlyFields = std::ranges::all_of(keys, [&](PropertyKeyID key) {
            std::optional<uint32_t> field = layout.findField(key);
            if (field)
                order.fields.append(*field);
            return field.has_value();
        });
        if (listsOnlyFields && !std::ranges::contains(layout.flow.constructorOrders.span().first(numberOfIndexOrders), order))
            layout.flow.constructorOrders.append(WTF::move(order));
    }
}

static LayoutID provenBaseOf(const Layout& layout)
{
    return layout.flow.baseState == BaseState::Layout ? layout.flow.base : LayoutID();
}

static void completeFields(const FlowApplyContext& context, const InlineIntegerTypes& integers, TypeArena& arena, const FamilyMemberTypes& declaredInFamilies, LayoutID id, BitVector& done)
{
    Program& program = context.program;
    Vector<LayoutID, 8> incompleteChain;
    for (LayoutID current = id; current && !done.quickSet(current.index()); current = provenBaseOf(program.at(current)))
        incompleteChain.append(current);

    while (!incompleteChain.isEmpty()) {
        LayoutID current = incompleteChain.takeLast();
        appendInheritedFields(program, current);
        appendFlowFields(program, integers, arena, declaredInFamilies[current.index()].span(), current, program.at(current), context.application.layouts[current.index()]);
        appendLiteralKeyOrders(program.at(current), context.application.layouts[current.index()]);
    }
}

void applyLayoutUpdates(const FlowApplyContext& context)
{
    Program& program = context.program;
    const Vector<FlowLayoutUpdate>& updates = context.application.layouts;
    RELEASE_ASSERT(updates.size() == program.layouts.size());

    InlineIntegerTypes integers(program, context.build.parallelLoops());
    integers.layoutsNeverStoredOtherTypes = findLayoutsNeverStoredOtherTypes(program, context.results, updates);
    TypeArena arena(program.types);
    FamilyMemberTypes declaredInFamilies = findTypesDeclaredInFamilies(program, integers.visibleStores, updates, arena);
    forEachIndexInParallel(context.build.parallelLoops(), program.layouts.size(), [&](unsigned index) {
        applyLayoutUpdate(program, integers, LayoutID(index), program.layouts[index], updates[index]);
    });
    program.indexClasses();

    BitVector done(program.layouts.size());
    for (size_t index = 0; index < program.layouts.size(); ++index)
        completeFields(context, integers, arena, declaredInFamilies, LayoutID(index), done);
    program.indexNames();
}

void keepExpectedIntegersInSlots(Program& program, BuildContext& build)
{
    InlineIntegerTypes integers(program, build.parallelLoops());
    for (size_t index = 0; index < program.layouts.size(); ++index) {
        Layout& layout = program.layouts[index];
        for (uint32_t field = 0; field < layout.fields.size(); ++field)
            keepExpectedIntegersInSlot(program, integers, layout, layout.fields[field], integers.visibleStores.kindsStoredIn(LayoutID(index), field), integers.mustKeepType(LayoutID(index), field));
    }
    for (size_t index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].isDerived())
            appendInheritedFields(program, LayoutID(index));
    }
}

void applyVariableUpdates(const FlowApplyContext& context)
{
    Program& program = context.program;
    const Vector<FlowVariableUpdate>& updates = context.application.variables;
    RELEASE_ASSERT(updates.size() == program.variables.size());

    forEachIndexInParallel(context.build.parallelLoops(), program.variables.size(), [&](unsigned index) {
        Variable& variable = program.variables[index];
        applyPosition(variable.declared, updates[index].position);
        variable.flow = updates[index].flow;
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
