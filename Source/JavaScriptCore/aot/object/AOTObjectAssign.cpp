/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTAssignmentPlans.h"
#include "AOTFieldMovePlanCache.h"
#include "AOTKeyOrderTable.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTProgramStrings.h"
#include "AOTState.h"
#include "DeferGC.h"
#include "GCMemoryOperations.h"
#include "JSCInlines.h"
#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

static std::unique_ptr<FieldMovePlan> createFieldCopyPlan(VM& vm, Object& target, const Object& source)
{
    State& state = target.state();
    const LayoutData& layout = target.layout();
    KeyOrderTable& keyOrders = state.objects->keyOrders;
    RuntimeLayoutTable& layouts = state.objects->layouts;
    auto plan = makeUnique<FieldMovePlan>();
    auto neverApplies = [] {
        return makeUnique<FieldMovePlan>();
    };
    auto listed = keyOrders.propertiesInOrder(vm, source.layout(), source.orderWord() & orderAndCountMask, Object::SymbolKeys::Include);
    if (!listed)
        return neverApplies();
    bool listsKeys = !layout.keysAreNeverEnumerated;
    BitVector present;
    Object::KeyOrderStorage storage;
    if (listsKeys) {
        for (uint16_t key : target.keyOrder(state, storage))
            present.set(key);
    }
    uint32_t word = target.orderWord();
    for (auto& property : *listed) {
        const FieldData* to = layouts.field(vm, layout, property.name);
        if (!to || to->flags & fieldIsStoredOnce || to->slot.representation() == SlotRepresentation::DeferredClosure)
            return neverApplies();
        plan->appendFieldToField(layouts, *property.field, *to);
        uint16_t index = target.indexOf(*to);
        if (!listsKeys || present.get(index))
            continue;
        auto next = keyOrders.orderAfterCreating(layout, word, index);
        if (!next)
            return neverApplies();
        word = *next;
        present.set(index);
    }
    // A value that has to be tested against the type of its field is stored faster by an assignment plan, which remembers what it has accepted.
    bool testsValues = std::ranges::any_of(plan->moves, [](const FieldMove& move) {
        return move.kind == FieldMoveKind::BoxedToSlot;
    });
    if (testsValues)
        return neverApplies();
    plan->finish(state.objects->structures.objectStructure(vm, layout.shape), layout.size, word & orderAndCountMask);
    return plan;
}

static Extension* createCopyOf(VM& vm, Extension* extension)
{
    if (!extension)
        return nullptr;
    Extension* copy = Extension::create(vm, extension->granuleBits());
    size_t count = std::popcount(extension->granuleBits());
    memcpySpan(std::span { copy->granules(), count }, std::span { extension->granules(), count });
    return copy;
}

static size_t sizeOfFieldWords(const LayoutData& layout)
{
    static_assert(!(offsetOfFirstField % sizeof(uint64_t)));
    return WTF::roundUpToMultipleOf<sizeof(uint64_t)>(layout.size) - offsetOfFirstField;
}

void Object::copyFieldsOfSameLayout(VM& vm, const Object& source)
{
    const LayoutData& layout = this->layout();
    ASSERT(&layout == &source.layout() && compiledStructure().hasOnlyPlainSlots() && !source.hasExtraProperties() && !source.hasOutOfLineKeyOrder());
    Extension* extension = createCopyOf(vm, source.extension());
    gcSafeMemcpy(&at<uint64_t>(offsetOfFirstField), &source.at<uint64_t>(offsetOfFirstField), sizeOfFieldWords(layout));
    if (layout.hasExtension())
        setExtension(vm, extension);
    setOrderAndCount(source.orderWord() & orderAndCountMask);
    vm.writeBarrier(this);
}

bool Object::tryCopyFieldsOf(VM& vm, const Object& source)
{
    if (this == &source || &source.state() != &state() || layout().kind() != LayoutKind::Record || hasFlagsInOrderWord() || source.hasExtraProperties())
        return false;
    if (hasOutOfLineKeyOrder() || source.hasOutOfLineKeyOrder() || source.layout().keysAreNeverEnumerated)
        return false;
    DeferGC deferGC(vm);
    if (shapeWord() == source.shapeWord() && !countOf(orderWord()) && compiledStructure().hasOnlyPlainSlots()) {
        copyFieldsOfSameLayout(vm, source);
        if (countOf(orderWord()))
            didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Add, nullptr);
        return true;
    }
    FieldMovePlanCache& movePlans = state().objects->caches->movePlans;
    FieldMovePlanKey key { FieldMovePlanPurpose::CopyFields };
    key.sourceWords = source.orderAndShapeWords() & orderCountAndShapeMask;
    key.targetWords = orderAndShapeWords();
    const FieldMovePlan* plan = movePlans.find(key);
    if (!plan)
        plan = &movePlans.add(vm, WTF::move(key), createFieldCopyPlan(vm, *this, source));
    if (!plan->isUsable())
        return false;
    auto& sourceObject = const_cast<Object&>(source);
    if (plan->sourceHasDeferredClosures)
        sourceObject.materializeDeferredClosures(vm);
    if (plan->granuleBits)
        ensureExtensionGranules(vm, plan->granuleBits);
    if (!applyMoves(vm, *plan, this, &sourceObject))
        return false;
    bool didCreateFields = layout().keysAreNeverEnumerated ? !plan->movesNothing() : plan->orderWord != (orderWord() & orderAndCountMask);
    setOrderAndCount(plan->orderWord);
    if (didCreateFields)
        didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Add, nullptr);
    return true;
}

bool Object::tryDefineFields(VM& vm, std::span<UniquedStringImpl* const> names, std::span<const EncodedJSValue> values)
{
    ASSERT(names.size() == values.size());
    const LayoutData& layout = this->layout();
    if (hasFlagsInOrderWord() || hasOutOfLineKeyOrder())
        return false;
    State& state = this->state();
    RuntimeLayoutTable& layouts = state.objects->layouts;
    Vector<const FieldData*, 16> fields;
    for (size_t i = 0; i < names.size(); ++i) {
        const FieldData* field = layouts.field(vm, layout, names[i]);
        if (!field || field->flags & fieldIsStoredOnce || field->slot.representation() == SlotRepresentation::DeferredClosure || !canStoreInField(*this, *field, JSValue::decode(values[i])))
            return false;
        fields.append(field);
    }

    KeyOrderStorage storage;
    Vector<uint16_t, 32> keys(keyOrder(state, storage));
    size_t numberOfKeysBefore = keys.size();
    BitVector isListed;
    for (uint16_t key : keys)
        isListed.set(key);
    for (size_t i = 0; i < fields.size(); ++i) {
        bool didStore = store(vm, *fields[i], JSValue::decode(values[i]));
        RELEASE_ASSERT(didStore);
        uint16_t index = layout.indexOf(*fields[i]);
        if (!isListed.set(index))
            keys.append(index);
    }
    if (keys.size() == numberOfKeysBefore)
        return true;
    if (!layout.keysAreNeverEnumerated)
        setKeyOrder(vm, state, keys.span());
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Add, nullptr);
    return true;
}

namespace {

class FieldStoreRun {
public:
    FieldStoreRun(VM& vm, Object& target, State& state, const AssignmentPlan& plan, JSObject* extra)
        : m_vm(vm)
        , m_target(target)
        , m_state(state)
        , m_plan(plan)
        , m_extra(extra)
    {
    }

    bool store(AssignmentPlan::FieldStore&, JSValue);
    bool didCreateFields() const { return m_didCreateFields; }
    void writeBarrierIfNeeded()
    {
        if (m_didStoreCell)
            m_vm.writeBarrier(&m_target);
    }

private:
    bool acceptsObject(AssignmentPlan::FieldStore&, JSObject*);
    bool isAccepted(AssignmentPlan::FieldStore&, JSValue);
    void listCreatedField(AssignmentPlan::FieldStore&);

    VM& m_vm;
    Object& m_target;
    State& m_state;
    const AssignmentPlan& m_plan;
    JSObject* m_extra;
    bool m_didStoreCell { false };
    bool m_didCreateFields { false };
};

}

static bool isDecidedByStructure(const RuntimeCellTest& test, JSObject* object)
{
    switch (test.kind) {
    case CellTestKind::IsObject:
    case CellTestKind::CellTypeIs:
    case CellTestKind::ShapeIn:
    case CellTestKind::InheritsClassInfo:
        return true;
    case CellTestKind::IsCallable:
        return !(object->inlineTypeFlags() & OverridesGetCallData);
    case CellTestKind::IsPristine:
    case CellTestKind::IsBuiltinCell:
    case CellTestKind::IsClosureOf:
    case CellTestKind::IsSymbol:
    case CellTestKind::SharingModeIs:
    case CellTestKind::IsNamespaceOf:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool FieldStoreRun::acceptsObject(AssignmentPlan::FieldStore& store, JSObject* object)
{
    const RuntimeLayoutTable& layouts = m_state.objects->layouts;
    if (!layouts.fieldAccepts(*store.to, object))
        return false;
    bool acceptsEveryObjectOfStructure = std::ranges::any_of(layouts.typeOf(*store.to).cellTests(), [&](const RuntimeCellTest& test) {
        return isDecidedByStructure(test, object) && layouts.cellPassesTest(test, object);
    });
    if (acceptsEveryObjectOfStructure) {
        store.lastAcceptedStructure = object->structureID();
        m_state.objects->caches->assignmentPlans.didWritePlan();
    }
    return true;
}

ALWAYS_INLINE bool FieldStoreRun::isAccepted(AssignmentPlan::FieldStore& store, JSValue value)
{
    if (store.sourceAlwaysFits)
        return true;
    if (value.isObject())
        return value.asCell()->structureID() == store.lastAcceptedStructure || acceptsObject(store, asObject(value));
    bool isKindAccepted = store.acceptedKinds.containsAny(valueKindOf(value));
    ASSERT(!isKindAccepted || m_state.objects->layouts.fieldAccepts(*store.to, value));
    return isKindAccepted;
}

ALWAYS_INLINE void FieldStoreRun::listCreatedField(AssignmentPlan::FieldStore& store)
{
    m_didCreateFields = true;
    uint32_t word = m_target.orderWord();
    if (store.orderWordBeforeCreation == word) [[likely]] {
        m_target.at<uint32_t>(offsetOfOrderWord) = store.orderWordAfterCreation;
        return;
    }
    m_target.didCreateField(m_state, *store.to);
    if (!isOrderStoredOutOfLine(word) && !m_target.hasOutOfLineKeyOrder()) {
        store.orderWordBeforeCreation = word;
        store.orderWordAfterCreation = m_target.orderWord();
    }
}

bool FieldStoreRun::store(AssignmentPlan::FieldStore& store, JSValue value)
{
    SlotRepresentation representation = store.to->slot.representation();
    uint32_t offset = store.to->slot.offset;
    bool isOutOfLine = store.to->isOutOfLine();
    const SlotTraits traits = traitsOf(representation);
    bool isValueAccepted = isAccepted(store, value);
    bool isDirect = store.isDirect;
    void* slot = &m_target.at<uint8_t>(offset);
    bool existsWithoutStorage = false;
    if (isOutOfLine) {
        slot = m_target.slotOf(*store.to);
        if (!slot && isValueAccepted && isDirect && !store.isAbsentWhenEmpty && m_target.isInKeyOrder(m_state, *store.to)) {
            m_target.ensureExtensionGranules(m_vm, m_plan.granuleBitsFilled);
            existsWithoutStorage = true;
            slot = m_target.slotOf(*store.to);
        }
        if (!slot)
            isDirect = false;
    }
    auto mayFillEmptySlot = [&]() ALWAYS_INLINE_LAMBDA {
        if (store.isAbsentWhenEmpty)
            return !m_extra && m_plan.mayCreateFields;
        existsWithoutStorage = existsWithoutStorage || m_target.isInKeyOrder(m_state, *store.to);
        return existsWithoutStorage;
    };
    if (isDirect && isValueAccepted) {
        bool isEmpty = slotHoldsAbsent(representation, slot);
        if ((!isEmpty || mayFillEmptySlot()) && tryWriteSlot(*m_state.strings, representation, slot, value)) {
            m_didStoreCell |= traits.holdsCell && value.isCell();
            if (isEmpty && !existsWithoutStorage)
                listCreatedField(store);
            return true;
        }
    }
    bool isCreated = !m_target.readInline(*store.to);
    if ((isCreated && !m_plan.mayCreateFields) || !m_target.tryStore(m_vm, *store.to, value))
        return false;
    if (isCreated) {
        m_didCreateFields = true;
        m_target.didCreateField(m_state, *store.to);
    }
    return true;
}

static bool storeExtraProperties(VM& vm, Object& target, const AssignmentPlan& plan, JSObject* extra, const AssignmentSource& source)
{
    if (plan.addsExtraProperties)
        validateMutation(vm, target, ObjectMutation::AddProperty);
    if (target.structureID() != plan.target)
        return false;
    Structure* after = plan.extraPropertiesAfter;
    Structure* before = extra ? extra->structure() : nullptr;
    if (!extra) {
        Butterfly* butterfly = after->outOfLineCapacity() ? Butterfly::create(vm, nullptr, 0, after->outOfLineCapacity(), false, IndexingHeader(), 0) : nullptr;
        extra = JSFinalObject::createWithButterfly(vm, after, butterfly);
    } else if (before->outOfLineCapacity() != after->outOfLineCapacity()) {
        if (source.compiledObject) {
            for (auto& store : plan.extraPropertyStores) {
                if (!source.read(store))
                    return false;
            }
        }
        extra->nukeStructureAndSetButterfly(vm, before->id(), extra->allocateMoreOutOfLineStorage(vm, before->outOfLineCapacity(), after->outOfLineCapacity()));
    }
    for (auto& store : plan.extraPropertyStores) {
        JSValue value = source.read(store);
        if (!value)
            return false;
        extra->putDirectOffset(vm, store.offset, value);
    }
    if (!before) {
        target.setExtraProperties(vm, extra, plan.targetAfter);
        return true;
    }
    if (before != after)
        extra->setStructure(vm, after);
    if (target.structure() != plan.targetAfter)
        target.JSCell::setStructure(vm, plan.targetAfter);
    return true;
}

static std::optional<AssignmentSource> assignmentSourceFor(JSObject* source, std::span<UniquedStringImpl* const> excludedNames)
{
    if (!isCompiledObject(source)) {
        if (source->type() != FinalObjectType || source->canHaveExistingOwnIndexedProperties())
            return std::nullopt;
        return AssignmentSource { source, nullptr, source, 0, excludedNames };
    }
    auto* compiledSource = uncheckedDowncast<Object>(source);
    uint32_t orderWord = compiledSource->orderWord();
    if (orderWord & orderWordIntegrityBits || isOrderStoredOutOfLine(orderWord) || compiledSource->layout().keysAreNeverEnumerated)
        return std::nullopt;
    return AssignmentSource { source, compiledSource, compiledSource->extraProperties(), orderWord, excludedNames };
}

bool Object::tryAssignPropertiesOf(VM& vm, JSObject* source, std::span<UniquedStringImpl* const> excludedNames, PlanCreation creation)
{
    if (orderWord() & orderWordIntegrityBits || source == this)
        return false;
    std::optional<AssignmentSource> assignmentSource = assignmentSourceFor(source, excludedNames);
    if (!assignmentSource)
        return false;
    DeferGC deferGC(vm);
    JSObject* extra = extraProperties();
    State& state = this->state();
    AssignmentPlan* planOrNull = state.objects->caches->assignmentPlans.planFor(vm, *this, extra, *assignmentSource, creation);
    if (!planOrNull || !planOrNull->isUsable)
        return false;
    AssignmentPlan& plan = *planOrNull;
    FieldStoreRun run(vm, *this, state, plan, extra);
    std::span<AssignmentPlan::FieldStore> fieldStores = plan.fieldStores.mutableSpan();
    size_t numberOfFieldStoresBeforeAddedNames = plan.numberOfFieldStoresBeforeAddedNames;
    bool didAddExtraProperties = false;
    bool didStoreEveryProperty = true;
    for (size_t index = 0; didStoreEveryProperty; ++index) {
        if (index == numberOfFieldStoresBeforeAddedNames && !plan.extraPropertyStores.isEmpty()) {
            didStoreEveryProperty = storeExtraProperties(vm, *this, plan, extra, *assignmentSource);
            didAddExtraProperties = didStoreEveryProperty && plan.addsExtraProperties;
        }
        if (!didStoreEveryProperty || index == fieldStores.size())
            break;
        AssignmentPlan::FieldStore& store = fieldStores[index];
        JSValue value = assignmentSource->read(store);
        didStoreEveryProperty = !value || run.store(store, value);
    }
    run.writeBarrierIfNeeded();
    // It can drop the plan of this run, so nothing reads the plan afterward.
    if (run.didCreateFields() || didAddExtraProperties)
        didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Add, nullptr);
    return didStoreEveryProperty;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
