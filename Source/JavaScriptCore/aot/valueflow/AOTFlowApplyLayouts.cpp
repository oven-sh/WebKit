/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowApply.h"

#if ENABLE(AOT)

#include "AOTFlowSiteData.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPhaseTimes.h"
#include "AOTThreads.h"
#include "AOTTypeClosure.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using FamilyFieldTypeCache = UncheckedKeyHashMap<uint64_t, std::optional<Type>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

struct AppendedFieldInputs {
    // Indexed by layout, then by appended field.
    Vector<Vector<std::optional<Type>>> typesDeclaredInFamilies;
    BitVector layoutsNeverStoredOtherTypes;
};

} // anonymous namespace

static BitVector findLayoutsNeverStoredOtherTypes(const Program& program, const FlowResults& results, const Vector<FlowLayoutUpdate>& updates)
{
    auto baseOf = [&](LayoutID id) {
        const LayoutFlow& flow = updates[id.index()].flow;
        if (flow.baseState == BaseState::Layout)
            return flow.base;
        return flow.baseState == BaseState::Unknown ? predictedHeritageOf(program.at(id)) : LayoutID();
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

static void applyFieldUpdates(Layout& layout, const FlowLayoutUpdate& update)
{
    RELEASE_ASSERT(update.fields.size() == layout.fields.size());
    for (size_t index = 0; index < layout.fields.size(); ++index) {
        Field& field = layout.fields[index];
        const FlowFieldUpdate& fieldUpdate = update.fields[index];
        // What is stored under the name says nothing of what a getter answers.
        if (!fieldUpdate.accessor.isAccessor())
            applyPosition(field.declared, fieldUpdate.position);
        field.flow = fieldUpdate.flow;

        bool wasAnalyzed = field.flow.location.writers == WriterState::AllKnown;
        if (wasAnalyzed && !field.flow.stability.contains(FieldStability::IsNeverDeleted))
            field.flags.remove(FieldFlags::IsRequired);
    }
}

static void applyLayoutUpdate(Layout& layout, const FlowLayoutUpdate& update)
{
    layout.flow = update.flow;
    layout.classFlow = update.classFlow;
    applyFieldUpdates(layout, update);
}

// The slot holds the GetterSetter, and the type of the slot is that of the getter.
static void becomeAccessor(TypeArena& arena, Field& field, const FlowAccessorField& accessor)
{
    field.form = accessor.form;
    field.flags.remove(FieldFlags::IsImmutable);
    Type getter = accessor.getters.isEmpty() ? Type::function() : Type::never();
    for (FunctionID function : accessor.getters)
        getter = getter.unionWith(Type::forFunction(function), arena);
    field.slotType = arena.intern(getter);
}

static void applyAccessorFields(TypeArena& arena, Layout& layout, const FlowLayoutUpdate& update)
{
    for (size_t index = 0; index < layout.fields.size(); ++index) {
        if (update.fields[index].accessor.isAccessor())
            becomeAccessor(arena, layout.fields[index], update.fields[index].accessor);
    }
}

// `function Symbol(..) { .. (this as any).links = undefined }`: the constructor makes room for a name that its own type does not have. All that is known of the field is what stands in for its value.
static bool isPlaceholder(const Program& program, const Field& field)
{
    return field.key && field.form.isEmpty() && field.declared.flags.contains(PositionFlags::IsExpected) && program.at(field.declared.type).isUndefinedOrNull();
}

static std::optional<Type> uniteTypesDeclaredIn(const Program& program, std::span<const LayoutID> layouts, PropertyKeyID key, TypeArena& arena)
{
    std::optional<Type> united;
    for (LayoutID member : layouts) {
        std::optional<uint32_t> index = program.at(member).findField(key);
        if (!index)
            continue;
        const Field& declaration = fieldAt(program, declarationOf(program, { member, *index }));
        // It declares nothing: it waits for what the others declare.
        if (isPlaceholder(program, declaration))
            continue;
        const Type& type = program.at(slotTypeOf(program, declaration));
        united = united ? united->unionWith(type, arena) : type;
        if (united->isAnything())
            break;
    }
    return united;
}

static std::optional<Type> typeDeclaredInFamilies(const Program& program, FamilyFieldTypeCache& cache, std::span<const LayoutSetID> families, std::span<const LayoutID> views, PropertyKeyID key, TypeArena& arena)
{
    if (families.empty())
        return std::nullopt;
    std::optional<Type> declared = uniteTypesDeclaredIn(program, views, key, arena);
    for (LayoutSetID family : families) {
        uint64_t cacheKey = static_cast<uint64_t>(family.index()) << 32 | key.index();
        const std::optional<Type>& declaredInFamily = cache.ensure(cacheKey, [&] {
            return uniteTypesDeclaredIn(program, program.at(family).members.span(), key, arena);
        }).iterator->value;
        if (declaredInFamily)
            declared = declared ? declared->unionWith(*declaredInFamily, arena) : *declaredInFamily;
    }
    if (!declared || declared->isAnything())
        return std::nullopt;
    return declared;
}

Vector<Vector<std::optional<Type>>> findTypesDeclaredInFamilies(const Program& program, const Vector<FlowLayoutUpdate>& updates, TypeArena& arena)
{
    Vector<Vector<std::optional<Type>>> types(updates.size());
    FamilyFieldTypeCache cache;
    BitVector declaredLayoutsWithFields(program.layouts.size());
    for (size_t index = 0; index < updates.size(); ++index) {
        if (!updates[index].appendedFields.isEmpty())
            declaredLayoutsWithFields.quickSet(declaredLayoutOf(program, LayoutID(index)).index());
    }
    Vector<Vector<LayoutID>> declaredViews = declaredViewsOf(program, declaredLayoutsWithFields);
    for (size_t index = 0; index < updates.size(); ++index) {
        if (updates[index].appendedFields.isEmpty())
            continue;
        Vector<LayoutSetID, 2> families = declaredFamiliesOf(program, LayoutID(index));
        std::span<const LayoutID> views = declaredViews[declaredLayoutOf(program, LayoutID(index)).index()].span();
        types[index] = WTF::map(updates[index].appendedFields, [&](const FlowAppendedField& appended) {
            return typeDeclaredInFamilies(program, cache, families.span(), views, appended.key, arena);
        });
    }
    return types;
}

namespace {

struct TypeOfPlaceholder {
    FieldReference field;
    Type declared;
};

} // anonymous namespace

// A narrower view of the object declares the name (`interface TransientSymbol extends Symbol { links: .. }`), and typed code stores through that view.
static Vector<TypeOfPlaceholder> findTypesOfPlaceholders(const Program& program, TypeArena& arena)
{
    BitVector declaredLayoutsWithPlaceholders(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Instance || layout.isDerived())
            continue;
        if (std::ranges::any_of(layout.fields, [&](const Field& field) { return !field.flags.contains(FieldFlags::IsInherited) && isPlaceholder(program, field); }))
            declaredLayoutsWithPlaceholders.quickSet(index);
    }
    Vector<TypeOfPlaceholder> found;
    if (declaredLayoutsWithPlaceholders.isEmpty())
        return found;
    FamilyFieldTypeCache cache;
    Vector<Vector<LayoutID>> declaredViews = declaredViewsOf(program, declaredLayoutsWithPlaceholders);
    for (size_t index : declaredLayoutsWithPlaceholders) {
        LayoutID id(static_cast<uint32_t>(index));
        const Layout& layout = program.at(id);
        Vector<LayoutSetID, 2> families = declaredFamiliesOf(program, id);
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (layout.fields[field].flags.contains(FieldFlags::IsInherited) || !isPlaceholder(program, layout.fields[field]))
                continue;
            std::optional<Type> declared = typeDeclaredInFamilies(program, cache, families.span(), declaredViews[index].span(), layout.fields[field].key, arena);
            // A boxed slot keeps an int32 an int32 for by-name reads: see applyTypeDeclaredInFamily().
            if (!declared || (declared->isNumberOrNullish() && declared->primitives & Primitive::Double))
                continue;
            found.append({ { id, field }, declared->unionWith(program.at(layout.fields[field].declared.type), arena) });
        }
    }
    return found;
}

// A store is tested against the type of the field. The run time fills the members of constant data without a test,
// so a field that data fills with a value outside the type that the families declare does not take that type.
static void dropTypesThatDataLiesOutside(const Program& program, const Vector<FlowLayoutUpdate>& updates, Vector<Vector<std::optional<Type>>>& types)
{
    auto baseOf = [&](LayoutID id) {
        const LayoutFlow& flow = updates[id.index()].flow;
        return flow.baseState == BaseState::Layout ? flow.base : LayoutID();
    };
    for (const Data& data : program.data) {
        forEachDataNodeInPreorder(data.root, [&](uint32_t, const DataNode& node) {
            const auto* object = node.tryAs<DataNode::Object>();
            if (!object)
                return;
            for (LayoutID layout = object->layout; layout; layout = baseOf(layout)) {
                Vector<std::optional<Type>>& typesOfLayout = types[layout.index()];
                const Vector<FlowAppendedField>& appendedFields = updates[layout.index()].appendedFields;
                for (size_t index = 0; index < typesOfLayout.size(); ++index) {
                    if (!typesOfLayout[index])
                        continue;
                    bool liesOutside = std::ranges::any_of(object->members, [&](const DataMember& member) {
                        return member.key == appendedFields[index].key && !includesData(program, *typesOfLayout[index], member.value);
                    });
                    if (liesOutside)
                        typesOfLayout[index] = std::nullopt;
                }
            }
        });
    }
}

static void applyTypeDeclaredInFamily(const Program& program, TypeArena& arena, const std::optional<Type>& declared, Field& field)
{
    if (!declared)
        return;
    const Type& proven = program.at(field.declared.type);
    bool fitsDeclaredType = proven.isSubsetOf(declared->withPrimitives(Primitive::Absent), program);
    // Nothing is ever stored that lies outside `anything`. So where all writers are known and what they store is not proven, that says nothing against the type that the families declare:
    // the field takes it, and a store is tested against it, as where the writers are not known.
    if (field.flow.stability.contains(FieldStability::IsNeverStoredWithOtherType)) {
        if (fitsDeclaredType)
            return;
        field.flow.stability.remove(FieldStability::IsNeverStoredWithOtherType);
    }
    const Type& slotType = fitsDeclaredType ? proven : *declared;
    if (!fitsDeclaredType)
        field.declared.type = arena.intern(*declared);
    // An unboxed double is as wide as a boxed value, and a boxed slot keeps an int32 an int32 for by-name reads. So the slot takes every value, and who reads it expects the number.
    if (!fitsDeclaredType && slotType.isNumberOrNullish() && slotType.primitives & Primitive::Double)
        field.declared.flags.add(PositionFlags::IsExpected);
}

static void appendFlowFields(const Program& program, TypeArena& arena, std::span<const std::optional<Type>> typesDeclaredInFamilies, bool mustKeepTypes, Layout& layout, const FlowLayoutUpdate& update)
{
    for (size_t index = 0; index < update.appendedFields.size(); ++index) {
        const FlowAppendedField& appended = update.appendedFields[index];
        RELEASE_ASSERT(!layout.findField(appended.key));
        Field field;
        field.key = appended.key;
        field.declared.type = TypeTable::anything;
        if (!appended.accessor.isAccessor())
            applyPosition(field.declared, appended.position);
        field.flags = { FieldFlags::IsAddedLate };
        field.flow = appended.flow;
        if (appended.accessor.isAccessor())
            becomeAccessor(arena, field, appended.accessor);
        else if (!mustKeepTypes)
            applyTypeDeclaredInFamily(program, arena, typesDeclaredInFamilies[index], field);
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

static void completeFields(const FlowApplyContext& context, TypeArena& arena, const AppendedFieldInputs& inputs, LayoutID id, BitVector& done)
{
    Program& program = context.program;
    Vector<LayoutID, 8> incompleteChain;
    for (LayoutID current = id; current && !done.quickSet(current.index()); current = provenBaseOf(program.at(current)))
        incompleteChain.append(current);

    while (!incompleteChain.isEmpty()) {
        LayoutID current = incompleteChain.takeLast();
        const FlowLayoutUpdate& update = context.application.layouts[current.index()];
        appendInheritedFields(program, current);
        appendFlowFields(program, arena, inputs.typesDeclaredInFamilies[current.index()].span(), inputs.layoutsNeverStoredOtherTypes.get(current.index()), program.at(current), update);
        appendLiteralKeyOrders(program.at(current), update);
    }
}

void applyLayoutUpdates(const FlowApplyContext& context)
{
    Program& program = context.program;
    const Vector<FlowLayoutUpdate>& updates = context.application.layouts;
    RELEASE_ASSERT(updates.size() == program.layouts.size());

    PhaseTimes times(context.build, PassID::ApplyFlowResults);
    TypeArena arena(program.types);
    // Reads the field types as declared, so it has to run before any update is applied.
    AppendedFieldInputs inputs { findTypesDeclaredInFamilies(program, updates, arena), findLayoutsNeverStoredOtherTypes(program, context.results, updates) };
    dropTypesThatDataLiesOutside(program, updates, inputs.typesDeclaredInFamilies);
    Vector<TypeOfPlaceholder> typesOfPlaceholders = findTypesOfPlaceholders(program, arena);
    times.didFinish("findTypesDeclaredInFamilies"_s);

    forEachIndexInParallel(context.build.parallelLoops(), program.layouts.size(), [&](unsigned index) {
        applyLayoutUpdate(program.layouts[index], updates[index]);
    });
    for (size_t index = 0; index < program.layouts.size(); ++index)
        applyAccessorFields(arena, program.layouts[index], updates[index]);
    // Where flow has proven nothing of the field, it takes the type, and every store is tested against it.
    for (const TypeOfPlaceholder& placeholder : typesOfPlaceholders) {
        Field& field = program.at(placeholder.field.layout).fields[placeholder.field.field];
        if (!isPlaceholder(program, field))
            continue;
        field.declared.type = arena.intern(placeholder.declared);
        field.declared.flags.remove(PositionFlags::IsExpected);
    }
    program.setKeysStoredThroughUnknownReceivers(Vector<PropertyKeyID>(context.results.keysStoredThroughUnknownReceivers()));
    program.indexClasses();
    times.didFinish("applyLayoutUpdate"_s);

    BitVector done(program.layouts.size());
    for (size_t index = 0; index < program.layouts.size(); ++index)
        completeFields(context, arena, inputs, LayoutID(index), done);
    markEnforcedFields(program, context.build);
    times.didFinish("completeFields"_s);

    program.indexNames(context.build.parallelLoops());
    times.didFinish("indexNames"_s);
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
