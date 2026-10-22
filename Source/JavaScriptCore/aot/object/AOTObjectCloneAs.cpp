/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObjectCopy.h"

#if ENABLE(AOT)

#include "AOTFieldMovePlanCache.h"
#include "AOTInstance.h"
#include "AOTKeyOrderTable.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramIdentifiers.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "MarkedSpace.h"
#include "StructureCache.h"
#include <algorithm>

namespace JSC { namespace AOT {

static std::unique_ptr<FieldMovePlan> createCloneAsPlan(VM& vm, State& state, Structure* structure, const LayoutData& layout, const BitVector& replaced)
{
    auto plan = makeUnique<FieldMovePlan>();
    RuntimeLayoutTable& layouts = state.objects->layouts;
    std::span<const FieldData> fields = layout.fields();
    Vector<uint16_t, 16> order;
    BitVector present;
    bool canMoveProperties = true;
    structure->forEachProperty(vm, [&](const auto& entry) {
        const FieldData* field = layouts.field(vm, layout, entry.key());
        if (!field || field->slot.representation() == SlotRepresentation::DeferredClosure) {
            canMoveProperties = false;
            return false;
        }
        uint16_t index = layout.indexOf(*field);
        order.append(index);
        present.set(index);
        if (!replaced.get(index))
            plan->appendPropertyToField(layouts, entry.offset(), *field);
        return true;
    });
    if (!canMoveProperties)
        return plan;
    for (uint16_t i = 0; i < layout.numberOfFields; ++i) {
        if (replaced.get(i) && !present.get(i)) {
            order.append(i);
            present.set(i);
        }
        if (!present.get(i) && !(fields[i].flags & fieldIsOptional))
            return plan;
    }
    uint32_t orderAndCount = unorderedOrderWord;
    if (!layout.keysAreNeverEnumerated) {
        auto numbered = state.objects->keyOrders.orderStartingWith(layout, order.span());
        if (!numbered)
            return plan;
        orderAndCount = *numbered;
    }
    for (uint16_t i = 0; i < layout.numberOfFields; ++i) {
        const FieldData& field = fields[i];
        if (!present.get(i) && !field.isOutOfLine() && traitsOf(field.slot.representation()).absentBits)
            plan->appendSetAbsent(field);
    }
    plan->finish(state.objects->structures.objectStructure(vm, layout.shape, structure->realm()->objectPrototype()), layout.size, orderAndCount);
    return plan;
}

static const FieldMovePlan& cloneAsPlan(VM& vm, State& state, Structure* structure, const LayoutData& layout, BitVector&& replaced)
{
    FieldMovePlanCache& plans = state.objects->caches->movePlans;
    FieldMovePlanKey key;
    key.purpose = FieldMovePlanPurpose::CloneAs;
    key.targetWords = static_cast<uint64_t>(shapeWordOf(layout.shape)) << 32;
    key.sourceStructure = structure;
    key.replacedFields = WTF::move(replaced);
    if (const FieldMovePlan* found = plans.find(key))
        return *found;
    auto plan = createCloneAsPlan(vm, state, structure, layout, key.replacedFields);
    return plans.add(vm, WTF::move(key), WTF::move(plan));
}

static Object* cloneAsWithExtraProperties(VM& vm, State& state, JSObject* source, const LayoutData& layout, std::span<const EncodedJSValue> replacedFields)
{
    struct SourceProperty {
        UniquedStringImpl* name;
        PropertyOffset offset;
        const FieldData* field;
    };
    Structure* structure = source->structure();
    JSGlobalObject* realm = structure->realm();
    std::span<const FieldData> fields = layout.fields();
    Vector<SourceProperty, 16> sourceProperties;
    bool hasExtraKeys = false;
    structure->forEachProperty(vm, [&](const auto& entry) {
        const FieldData* field = state.objects->layouts.field(vm, layout, entry.key());
        hasExtraKeys |= !field;
        sourceProperties.append({ entry.key(), entry.offset(), field });
        return true;
    });
    if (hasExtraKeys && !layout.extraPropertiesSlot())
        return nullptr;
    Object* object = Object::create(vm, state.objects->structures.objectStructure(vm, layout.shape, realm->objectPrototype()), layout, layout.size, unorderedOrderWord);

    Vector<uint16_t, 16> keys;
    Vector<Object::ExtraProperty, 8> extras;
    uint32_t numberOfExtrasInKeys = 0;
    BitVector present;
    auto tryAppendKey = [&](uint32_t entry) {
        if (entry > maxKeyOrderEntry)
            return false;
        keys.append(static_cast<uint16_t>(entry));
        return true;
    };
    auto tryAddField = [&](uint32_t index, JSValue value) {
        if (extras.size() > numberOfExtrasInKeys) {
            numberOfExtrasInKeys = static_cast<uint32_t>(extras.size());
            if (!tryAppendKey(layout.numberOfFields + numberOfExtrasInKeys - 1))
                return false;
        }
        present.set(index);
        return tryAppendKey(index) && object->tryInitialize(vm, fields[index], value);
    };

    for (auto& property : sourceProperties) {
        JSValue value = source->getDirect(property.offset);
        if (property.field) {
            uint32_t index = layout.indexOf(*property.field);
            JSValue replaced = JSValue::decode(replacedFields[index]);
            if (!tryAddField(index, replaced ? replaced : value))
                return nullptr;
            continue;
        }
        if (property.name == vm.propertyNames->underscoreProto.impl())
            return nullptr;
        extras.append({ property.name, value });
    }
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        if (present.get(i))
            continue;
        if (JSValue replaced = JSValue::decode(replacedFields[i])) {
            if (!tryAddField(i, replaced))
                return nullptr;
        } else if (!(fields[i].flags & fieldIsOptional))
            return nullptr;
    }

    JSObject* extra = extras.isEmpty() ? nullptr : Object::createExtraProperties(realm, extras.span());
    auto order = object->initialKeyOrder(vm, keys.span(), extra);
    if (!order)
        return nullptr;
    object->setInitialKeyOrder(vm, *order, extra);
    object->tryFinishBirth();
    return object;
}

Object* tryCloneAs(VM& vm, JSObject* source, const LayoutData& layout, std::span<const EncodedJSValue> replacedFields)
{
    RELEASE_ASSERT(replacedFields.size() == layout.numberOfFields);
    if (!isPlainDataObject(source))
        return nullptr;
    Structure* structure = source->structure();
    Instance* instance = Instance::of(structure->realm());
    if (!instance)
        return nullptr;
    State& state = *instance->state;
    RuntimeLayoutTable& layouts = state.objects->layouts;
    if (!layouts.hasLayoutForShape(layout.shape) || &layouts.layoutForShape(layout.shape) != &layout)
        return nullptr;
    if (layout.kind() != LayoutKind::Record)
        return nullptr;

    BitVector replaced;
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        if (JSValue::decode(replacedFields[i]))
            replaced.set(i);
    }

    DeferGC deferGC(vm);
    const FieldMovePlan& plan = cloneAsPlan(vm, state, structure, layout, WTF::move(replaced));
    if (!plan.isUsable())
        return cloneAsWithExtraProperties(vm, state, source, layout, replacedFields);
    JSObject* created = tryCreateByPlan(vm, plan, source);
    if (!created)
        return nullptr;
    Object* object = uncheckedDowncast<Object>(created);
    for (uint32_t i = 0; i < layout.numberOfFields; ++i) {
        JSValue replacement = JSValue::decode(replacedFields[i]);
        if (replacement && !object->tryInitialize(vm, layout.fields()[i], replacement))
            return nullptr;
    }
    object->tryFinishBirth();
    return object;
}

static unsigned inlineCapacityFor(size_t numberOfProperties)
{
    size_t size = MarkedSpace::optimalSizeFor(JSFinalObject::allocationSize(numberOfProperties));
    return std::min<unsigned>((size - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue), JSFinalObject::maxInlineCapacity);
}

namespace {

class OrdinaryCopyPlanBuilder {
public:
    OrdinaryCopyPlanBuilder(VM& vm, JSGlobalObject* realm, State& state, ExcludedRun excluded)
        : m_vm(vm)
        , m_realm(realm)
        , m_state(state)
        , m_excluded(excluded)
    {
    }

    std::unique_ptr<FieldMovePlan> planFor(const Object&);
    std::unique_ptr<FieldMovePlan> planFor(Structure*);

private:
    bool isExcluded(uint32_t identifier) const { return std::ranges::contains(m_excluded.identifiers, identifier); }
    bool isExcluded(UniquedStringImpl* name) const
    {
        std::optional<uint32_t> identifier = m_vm.aotState().identifiers->numberOf(name);
        return identifier && isExcluded(*identifier);
    }
    bool begin(size_t numberOfProperties);
    std::optional<PropertyOffset> add(UniquedStringImpl*);
    std::unique_ptr<FieldMovePlan> finish();
    static std::unique_ptr<FieldMovePlan> neverApplies() { return makeUnique<FieldMovePlan>(); }

    VM& m_vm;
    JSGlobalObject* m_realm;
    State& m_state;
    ExcludedRun m_excluded;
    Structure* m_structure { nullptr };
    std::unique_ptr<FieldMovePlan> m_plan;
};

}

// A literal that begins with a copy mostly goes on with a name or two.
bool OrdinaryCopyPlanBuilder::begin(size_t numberOfProperties)
{
    if (numberOfProperties >= JSFinalObject::maxInlineCapacity)
        return false;
    m_structure = m_realm->structureCache().emptyObjectStructureForPrototype(m_realm, m_realm->objectPrototype(), inlineCapacityFor(numberOfProperties + 1));
    m_plan = makeUnique<FieldMovePlan>();
    return true;
}

std::optional<PropertyOffset> OrdinaryCopyPlanBuilder::add(UniquedStringImpl* name)
{
    PropertyOffset offset = invalidOffset;
    m_structure = Structure::addPropertyTransition(m_vm, m_structure, PropertyName(name), 0, offset);
    if (m_structure->isDictionary() || !isInlineOffset(offset))
        return std::nullopt;
    return offset;
}

std::unique_ptr<FieldMovePlan> OrdinaryCopyPlanBuilder::finish()
{
    m_plan->finish(m_structure, 0, 0);
    return WTF::move(m_plan);
}

std::unique_ptr<FieldMovePlan> OrdinaryCopyPlanBuilder::planFor(const Object& source)
{
    auto listed = m_state.objects->keyOrders.propertiesInOrder(m_vm, source.layout(), source.orderWord(), Object::SymbolKeys::Include);
    if (!listed)
        return neverApplies();
    size_t numberOfExcludedFields = std::ranges::count_if(*listed, [&](auto& property) {
        return isExcluded(property.field->name);
    });
    if (!begin(listed->size() - numberOfExcludedFields))
        return neverApplies();
    for (auto& property : *listed) {
        if (isExcluded(property.field->name))
            continue;
        std::optional<PropertyOffset> offset = add(property.name);
        if (!offset)
            return neverApplies();
        m_plan->appendFieldToInlineProperty(m_state.objects->layouts, *property.field, *offset);
    }
    return finish();
}

std::unique_ptr<FieldMovePlan> OrdinaryCopyPlanBuilder::planFor(Structure* source)
{
    if (source->isDictionary() || !source->canPerformFastPropertyEnumerationCommon() || hasIndexedProperties(source->indexingType()) || source->hasNonReifiedStaticProperties())
        return neverApplies();
    struct CopiedProperty {
        UniquedStringImpl* name;
        PropertyOffset offset;
    };
    Vector<CopiedProperty, 16> properties;
    bool hasOnlyDataProperties = true;
    source->forEachProperty(m_vm, [&](const PropertyTableEntry& entry) {
        if (entry.attributes() & PropertyAttribute::DontEnum || PropertyName(entry.key()).isPrivateName() || isExcluded(entry.key()))
            return true;
        hasOnlyDataProperties = !(entry.attributes() & PropertyAttribute::AccessorOrCustomAccessorOrValue) && entry.key() != m_vm.propertyNames->underscoreProto.impl();
        properties.append({ entry.key(), entry.offset() });
        return hasOnlyDataProperties;
    });
    if (!hasOnlyDataProperties || !begin(properties.size()))
        return neverApplies();
    for (auto& property : properties) {
        std::optional<PropertyOffset> offset = add(property.name);
        if (!offset)
            return neverApplies();
        m_plan->appendPropertyToInlineProperty(property.offset, *offset);
    }
    return finish();
}

JSFinalObject* tryCopyAsOrdinaryObject(VM& vm, JSGlobalObject* realm, JSObject* source, ExcludedRun excluded)
{
    Instance* instance = Instance::of(realm);
    if (!instance || source->structure()->realm() != realm)
        return nullptr;
    State& state = *instance->state;
    Object* compiled = compiledObjectOrNull(source);
    FieldMovePlanKey key { FieldMovePlanPurpose::CopyAsOrdinaryObject };
    key.targetWords = excluded.numberPlusOne;
    if (compiled) {
        if (compiled->hasExtraProperties() || compiled->hasOutOfLineKeyOrder() || compiled->layout().keysAreNeverEnumerated)
            return nullptr;
        key.sourceWords = compiled->orderAndShapeWords() & orderCountAndShapeMask;
    } else {
        if (source->type() != FinalObjectType)
            return nullptr;
        key.sourceStructure = source->structure();
    }

    DeferGC deferGC(vm);
    FieldMovePlanCache& plans = state.objects->caches->movePlans;
    const FieldMovePlan* plan = plans.find(key);
    if (!plan) {
        OrdinaryCopyPlanBuilder builder(vm, realm, state, excluded);
        plan = &plans.add(vm, WTF::move(key), compiled ? builder.planFor(*compiled) : builder.planFor(source->structure()));
    }
    if (!plan->isUsable())
        return nullptr;
    if (plan->sourceHasDeferredClosures)
        compiled->materializeDeferredClosures(vm);
    JSFinalObject* copy = JSFinalObject::create(vm, plan->structure);
    return applyMovesToNewObject(vm, *plan, copy, source) ? copy : nullptr;
}

bool isPlainDataObject(JSValue value)
{
    if (!value.isCell() || value.asCell()->type() != FinalObjectType)
        return false;
    Structure* structure = value.asCell()->structure();
    return !structure->hasPolyProto()
        && structure->storedPrototype() == structure->realm()->objectPrototype()
        && !hasIndexedProperties(structure->indexingType())
        && !structure->isDictionary()
        && structure->isStructureExtensible()
        && !structure->hasAnyKindOfGetterSetterProperties()
        && !structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto()
        && !structure->hasNonEnumerableProperties()
        && !structure->hasNonConfigurableProperties()
        && !structure->hasUnderscoreProtoPropertyExcludingOriginalProto()
        && structure->isQuickPropertyAccessAllowedForEnumeration();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
