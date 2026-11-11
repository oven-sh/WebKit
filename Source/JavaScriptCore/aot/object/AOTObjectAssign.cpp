/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTAssignmentPlans.h"
#include "AOTClosure.h"
#include "AOTFieldMoves.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTKeyOrderTable.h"
#include "AOTMappedCopyInlines.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTPropertyWalk.h"
#include "AOTState.h"
#include "DeferGCInlines.h"
#include "GCMemoryOperations.h"
#include "JSCInlines.h"
#include "ObjectConstructorInlines.h"
#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

static Extension* createCopyOf(VM& vm, Extension* extension)
{
    if (!extension)
        return nullptr;
    size_t size = extensionCellSizeFor(std::popcount(extension->granuleBits()));
    void* copy = vm.heap.cagedAuxiliarySpace<SubspaceAccess::OnMainThread>()->allocate(vm, size, nullptr, AllocationFailureMode::Assert);
    memcpy(copy, extension, size);
    vm.mutatorFence();
    return static_cast<Extension*>(copy);
}

static size_t sizeOfFieldWords(const LayoutData& layout)
{
    static_assert(!(offsetOfFirstField % sizeof(uint64_t)));
    return WTF::roundUpToMultipleOf<sizeof(uint64_t)>(layout.size) - offsetOfFirstField;
}

void Object::copyFieldsOfSameLayout(VM& vm, const Object& source)
{
    const LayoutData& layout = this->layout();
    ASSERT(&layout == &source.layout() && !source.hasKeyOrderOfItsOwn());
    Extension* extension = createCopyOf(vm, source.extension());
    gcSafeMemcpy(&at<uint64_t>(offsetOfFirstField), &source.at<uint64_t>(offsetOfFirstField), sizeOfFieldWords(layout));
    if (source.bearsMark())
        setBearsMark(true);
    else
        setIsInBirth(false);
    if (layout.hasExtension())
        setExtension(vm, extension);
    setOrderAndCount(source.orderWord() & orderAndCountMask);
    vm.writeBarrier(this);
}

static bool tryMakeClosuresForCopyOfWords(VM& vm, Object& source)
{
    constexpr uint16_t flagsOfOtherFields = fieldIsPrivate | fieldIsBrand | fieldAttributeFlags;
    for (const FieldData& field : source.layout().fields()) {
        if (field.flags & flagsOfOtherFields)
            return false;
        if (field.slot.representation() == SlotRepresentation::DeferredClosure && source.at<uint32_t>(field.slot.offset) == uncreatedDeferredClosure)
            source.materializeDeferredClosure(vm, field);
    }
    return true;
}

ALWAYS_INLINE_INTO_OTHER_FILES Object* Object::createCopyOfWords(VM& vm, const Object& source, Structure* structure)
{
    const LayoutData& layout = source.layout();
    Object* copy = new (NotNull, allocateCell<Object>(vm, layout.size)) Object(vm, structure);
    if (!layout.hasExtension()) {
        copy->at<uint32_t>(offsetOfOrderWord) = source.orderWord() & orderAndCountMask;
        copy->at<uint32_t>(offsetOfShapeWord) = source.shapeWordWithMark();
        gcSafeMemcpy(&copy->at<uint64_t>(offsetOfFirstField), &source.at<uint64_t>(offsetOfFirstField), sizeOfFieldWords(layout));
        copy->JSCell::finishCreation(vm);
        vm.writeBarrier(copy);
        return copy;
    }
    zeroSpan(std::span { reinterpret_cast<uint8_t*>(copy) + sizeof(JSCell), layout.size - sizeof(JSCell) });
    copy->at<uint32_t>(offsetOfOrderWord) = unorderedOrderWord;
    copy->at<uint32_t>(offsetOfShapeWord) = source.shapeWordWithMark();
    copy->JSCell::finishCreation(vm);
    copy->copyFieldsOfSameLayout(vm, source);
    return copy;
}

static bool hasPrivateNames(VM& vm, Structure* structure)
{
    if (structure->isQuickPropertyAccessAllowedForEnumeration())
        return false;
    bool found = false;
    structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        found = PropertyName(entry.key()).isPrivateName();
        return !found;
    });
    return found;
}

static Structure* structureBeforeIntegrityLevel(Structure* structure)
{
    TransitionKind kind = structure->transitionKind();
    if (kind == TransitionKind::PreventExtensions || kind == TransitionKind::Seal || kind == TransitionKind::Freeze)
        return structure->previousID();
    return structure;
}

static bool isCopiedAsDictionary(Structure* structure)
{
    if (structure->typeInfo().type() != FinalObjectType || !structure->isDictionary() || structure->inlineSize() + structure->outOfLineSize() <= static_cast<unsigned>(Structure::s_maxTransitionLength))
        return false;
    if (!structure->canPerformFastPropertyEnumeration() || structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || structure->hasNonEnumerableProperties() || structure->hasNonConfigurableProperties())
        return false;
    if (structure->variant() != Structure::StructureVariant::Normal)
        return false;
    return !structure->mayHaveImmutableProperties() && !structure->mayBePrototype() && !structure->didPreventExtensions() && !structure->hasPolyProto();
}

NEVER_INLINE bool flattenOnceUnlessCopiedAsDictionary(VM& vm, JSObject* source)
{
    return !isCopiedAsDictionary(source->structure()) && flattenOnce(vm, source);
}

static NEVER_INLINE Structure* tryCopyStructureOfDictionary(VM& vm, Structure* structure)
{
    return isCopiedAsDictionary(structure) ? Structure::toCacheableDictionaryTransition(vm, structure) : nullptr;
}

JSFinalObject* tryCopyWithElements(VM& vm, JSObject* source)
{
    Structure* structure = source->structure();
    IndexingType indexingType = structure->indexingType();
    bool hasVector = hasInt32(indexingType) || hasDouble(indexingType) || hasContiguous(indexingType);
    if (source->type() != FinalObjectType || !(hasVector || (indexingType & IndexingShapeMask) == ArrayStorageShape) || isCopyOnWrite(structure->indexingMode()))
        return nullptr;
    if (structure->isDictionary() || structure->isCompiledStructure() || !structure->canPerformFastPropertyEnumerationCommon() || !structure->isQuickPropertyAccessAllowedForEnumeration() || source->hasNonReifiedStaticProperties())
        return nullptr;
    if (structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || structure->mayHaveImmutableProperties())
        return nullptr;
    if (structure->variant() != Structure::StructureVariant::Normal || structure->mayBePrototype() || structure->didPreventExtensions() || structure->hasBeenFlattenedBefore() || structure->hasPolyProto())
        return nullptr;
    if (structure->hasNonEnumerableProperties() || structure->hasNonConfigurableProperties()) {
        bool isPlain = !hasVector;
        structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            isPlain = isPlain && !entry.attributes();
            return isPlain;
        });
        if (!isPlain)
            return nullptr;
    }

    Butterfly* elements = source->butterfly();
    SparseArrayValueMap* sparse = hasVector ? nullptr : elements->arrayStorage()->m_sparseMap.get();
    if (sparse) {
        if (sparse->sparseMode() || sparse->lengthIsReadOnly() || sparse->hasAnyKindOfGetterSetterProperties())
            return nullptr;
        for (const SparseArrayEntry& entry : *sparse) {
            if (entry.attributes())
                return nullptr;
        }
    }
    DeferGC deferGC(vm);
    size_t capacity = structure->outOfLineCapacity();
    size_t bytesOfElements = hasVector ? static_cast<size_t>(elements->vectorLength()) * sizeof(EncodedJSValue) : ArrayStorage::sizeFor(elements->arrayStorage()->vectorLength());
    Butterfly* butterfly = Butterfly::tryCreateUninitialized(vm, nullptr, 0, capacity, true, bytesOfElements);
    if (!butterfly)
        return nullptr;
    memcpy(butterfly->propertyStorage() - capacity, elements->propertyStorage() - capacity, capacity * sizeof(EncodedJSValue) + sizeof(IndexingHeader) + bytesOfElements);
    if (!hasVector) {
        butterfly->arrayStorage()->m_indexBias = 0;
        butterfly->arrayStorage()->m_sparseMap.clear();
    }
    JSFinalObject* copy = JSFinalObject::createWithButterflyCopyingInlineStorage(vm, structure, butterfly, source->inlineStorage());
    if (sparse) {
        SparseArrayValueMap* map = SparseArrayValueMap::create(vm);
        for (const SparseArrayEntry& entry : *sparse)
            map->add(copy, entry.index()).iterator->forceSet(vm, map, entry.get(), 0);
        vm.mutatorFence();
        butterfly->arrayStorage()->m_sparseMap.set(vm, copy, map);
    }
    return copy;
}

static JSFinalObject* tryCopyPlainExtraProperties(VM& vm, JSObject* extra)
{
    if (hasIndexedProperties(extra->indexingType())) [[unlikely]]
        return tryCopyWithElements(vm, extra);
    Structure* structure = structureBeforeIntegrityLevel(extra->structure());
    if (!structure || !checkStructureForClone(structure, false)) {
        structure = structure ? tryCopyStructureOfDictionary(vm, structure) : nullptr;
        if (!structure)
            return nullptr;
    }
    if (extra->hasNonReifiedStaticProperties() || !structure->canPerformFastPropertyEnumerationCommon() || hasPrivateNames(vm, structure))
        return nullptr;
    // Every value is in place before the copy's fence, so a concurrent marker never sees an empty slot.
    Butterfly* butterfly = structure->outOfLineCapacity() ? Butterfly::create(vm, nullptr, 0, structure->outOfLineCapacity(), false, IndexingHeader(), 0) : nullptr;
    PropertyOffset maxOffset = structure->maxOffset();
    for (PropertyOffset offset = firstOutOfLineOffset; offset <= maxOffset; ++offset)
        butterfly->propertyStorage()[offsetInOutOfLineStorage(offset)].setWithoutWriteBarrier(extra->getDirect(offset));
    return JSFinalObject::createWithButterflyCopyingInlineStorage(vm, structure, butterfly, extra->inlineStorage());
}

JSFinalObject* tryCopyDictionary(VM& vm, JSObject* source)
{
    return tryCopyPlainExtraProperties(vm, source);
}

static ALWAYS_INLINE Structure* structureToName(JSObject* extraProperties)
{
    Structure* structure = extraProperties->structure();
    return structure->isDictionary() || hasIndexedProperties(structure->indexingType()) ? nullptr : structure;
}

NEVER_INLINE Object* Object::tryCreateCopyOfFieldsAndExtraProperties(VM& vm, const Object& source)
{
    const CompiledStructure& structure = source.compiledStructure();
    const LayoutData& layout = source.layout();
    if (source.orderWord() & orderWordHasFieldOutsideSlot || structure.hasOtherPrototype())
        return nullptr;
    bool hasExtraProperties = source.hasExtraProperties();
    if (hasExtraProperties ? !layout.extraPropertiesSlot() || source.hasOutOfLineKeyOrder() : !structure.flags().contains(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype))
        return nullptr;
    DeferGC deferGC(vm);
    if (!structure.isCopiedByWords() && !tryMakeClosuresForCopyOfWords(vm, const_cast<Object&>(source)))
        return nullptr;
    if (!hasExtraProperties)
        return createCopyOfWords(vm, source, source.structure());
    JSObject* extraProperties = tryCopyPlainExtraProperties(vm, source.extraProperties());
    if (!extraProperties)
        return nullptr;
    StructureTable& structures = source.state().objects->structures;
    Object* copy = createCopyOfWords(vm, source, structures.objectStructure(vm, layout.shape));
    CompiledStructure* named = structures.withExtraProperties(vm, copy->compiledStructure(), structureToName(extraProperties));
    copy->setExtraProperties(vm, extraProperties, named);
    if (!structure.extraPropertiesStructureID() && named->extraPropertiesStructureID() == source.extraProperties()->structureID())
        const_cast<Object&>(source).JSCell::setStructure(vm, named);
    return copy;
}

Object* Object::tryCreateCopyOfFields(VM& vm, const Object& source)
{
    const CompiledStructure& structure = source.compiledStructure();
    const LayoutData& layout = source.layout();
    if (layout.kind() != LayoutKind::Record || source.hasKeyOrderOfItsOwn() || structure.mayBePrototype())
        return nullptr;
    if (source.hasFlagsInOrderWord() || !structure.flags().contains(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype)) [[unlikely]]
        return tryCreateCopyOfFieldsAndExtraProperties(vm, source);
    DeferGC deferGC(vm);
    if (!structure.isCopiedByWords() && !tryMakeClosuresForCopyOfWords(vm, const_cast<Object&>(source)))
        return nullptr;
    return createCopyOfWords(vm, source, source.structure());
}

static ALWAYS_INLINE bool holdsKeyOrderFor(const CompiledStructure& ordered, const CompiledStructure& unordered)
{
    return ordered.hasOutOfLineKeyOrder() && !ordered.hasKeyOrderOfOneObject() && !ordered.hasExtraProperties() && !ordered.mayBePrototype()
        && ordered.hasOtherPrototype() == unordered.hasOtherPrototype() && ordered.storedPrototype() == unordered.storedPrototype() && ordered.realm() == unordered.realm();
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Object::tryCopyWordsOfSameLayout(VM& vm, const Object& source)
{
    if (shapeWord() != source.shapeWord() || this == &source || orderWord() != unorderedOrderWord)
        return false;
    uint32_t orderWordOfSource = source.orderWord();
    if (orderWordOfSource & ~(orderAndCountMask | orderWordIntegrityBits))
        return false;
    const CompiledStructure& structure = compiledStructure();
    constexpr OptionSet<CompiledStructure::Flag> flagsOfOtherStorage { CompiledStructure::Flag::HasExtraProperties, CompiledStructure::Flag::HasOutOfLineKeyOrder };
    if (!structure.hasOnlyPlainSlots() || structure.flags().containsAny(flagsOfOtherStorage) || structure.mayBePrototype())
        return false;
    const LayoutData& layout = structure.layout();
    if (layout.kind() != LayoutKind::Record)
        return false;
    bool takesStructure = structureID() != source.structureID();
    if (takesStructure && !holdsKeyOrderFor(source.compiledStructure(), structure))
        return false;
    ASSERT(takesStructure == isOrderStoredOutOfLine(orderWordOfSource));
    std::optional<SlotData> extensionSlot = structure.extensionSlot();
    Extension* extension = extensionSlot ? createCopyOf(vm, reinterpret_cast<Extension*>(const_cast<uint8_t*>(extensionStorageOrNull(&source, *extensionSlot)))) : nullptr;
    gcSafeMemcpy(&at<uint64_t>(offsetOfFirstField), &source.at<uint64_t>(offsetOfFirstField), sizeOfFieldWords(layout));
    if (source.bearsMark())
        setBearsMark(true);
    else
        setIsInBirth(false);
    if (extension) {
        if (extensionSlot->representation() == SlotRepresentation::Cell)
            at<Extension*>(extensionSlot->offset) = extension;
        else
            at<uint32_t>(extensionSlot->offset) = MarkedBlockCage::encode(cageBaseOf(this), reinterpret_cast<uint8_t*>(extension) - offsetOfEmptyExtensionInCage);
    }
    if (takesStructure) {
        ASSERT(source.structure()->typeInfoBlob() == structure.typeInfoBlob());
        setStructureIDDirectly(source.structureID());
        State& state = this->state();
        if (const MappedCopy*& place = state.instance.copiesWithoutFrame()[AllocationWords::placeOfCopyWithoutFrame(orderAndShapeWordsWithMark(), source.orderAndShapeWordsWithMark(), 0)]; !place)
            place = &state.objects->caches->assignmentPlans.copyOfNoSource();
    }
    at<uint32_t>(offsetOfOrderWord) = orderWordOfSource & orderAndCountMask;
    vm.writeBarrier(this);
    return true;
}

static RepeatedCopy tryRepeatAssignmentBetweenLayouts(VM&, State&, Object& to, const Object& from, uint32_t excludedRunNumberPlusOne);

bool Object::tryCopyFieldsOf(VM& vm, const Object& source)
{
    if (tryCopyWordsOfSameLayout(vm, source)) [[likely]]
        return true;
    if (this == &source || &source.state() != &state() || layout().kind() != LayoutKind::Record || hasFlagsInOrderWord())
        return false;
    if (hasOutOfLineKeyOrder() || source.hasOutOfLineKeyOrder())
        return false;
    bool mayCopyWords = shapeWord() == source.shapeWord() && !countOf(orderWord());
    bool copiesExtraProperties = source.hasExtraProperties();
    if (copiesExtraProperties && (!mayCopyWords || !layout().extraPropertiesSlot() || source.orderWord() & orderWordHasFieldOutsideSlot || source.compiledStructure().hasOtherPrototype() || compiledStructure().mayBePrototype()))
        return false;
    DeferGC deferGC(vm);
    if (mayCopyWords && !compiledStructure().hasOnlyPlainSlots()) [[unlikely]]
        mayCopyWords = tryMakeClosuresForCopyOfWords(vm, const_cast<Object&>(source));
    JSObject* extraProperties = mayCopyWords && copiesExtraProperties ? tryCopyPlainExtraProperties(vm, source.extraProperties()) : nullptr;
    if (copiesExtraProperties && !extraProperties)
        return false;
    if (mayCopyWords) {
        copyFieldsOfSameLayout(vm, source);
        if (extraProperties)
            setExtraProperties(vm, extraProperties, state().objects->structures.withExtraProperties(vm, compiledStructure(), structureToName(extraProperties)));
        if (countOf(orderWord()))
            didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Add, nullptr);
        return true;
    }
    if (!countOf(source.orderWord()))
        return true;
    return tryRepeatAssignmentBetweenLayouts(vm, state(), *this, source, 0) != RepeatedCopy::IsNotDone;
}

bool Object::tryDefineFields(VM& vm, std::span<UniquedStringImpl* const> names, std::span<const EncodedJSValue> values, std::span<const unsigned> attributes)
{
    ASSERT(names.size() == values.size() && (attributes.empty() || attributes.size() == names.size()));
    const LayoutData& layout = this->layout();
    if (hasFlagsInOrderWord() || hasOutOfLineKeyOrder())
        return false;
    State& state = this->state();
    RuntimeLayoutTable& layouts = state.objects->layouts;
    Vector<const FieldData*, 16> fields;
    Vector<std::optional<AcceptedValue>, 16> acceptedValues;
    bool someLeaveSlots = false;
    uint64_t granuleBits = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        const FieldData* field = layouts.field(vm, layout, names[i]);
        if (!field || isPreparedBeforeLookups(vm, names[i]) || field->flags & fieldAttributeFlags || field->slot.representation() == SlotRepresentation::DeferredClosure)
            return false;
        fields.append(field);
        if (!attributes.empty() && attributes[i]) {
            if (field->isPrivate() || !field->canLeaveSlot() || isInSlot(*field) || attributes[i] & PropertyAttribute::CustomAccessorOrValue)
                return false;
            someLeaveSlots = true;
            acceptedValues.append(std::nullopt);
            continue;
        }
        std::optional<AcceptedValue> accepted = acceptInSlot(*this, *field, JSValue::decode(values[i]));
        if (!accepted)
            return false;
        acceptedValues.append(accepted);
        if (field->isOutOfLine())
            granuleBits |= granuleBitOf(field->slot.offset);
    }

    Vector<uint16_t, 32> keys;
    BitVector isListed(layout.numberOfFields);
    keys.append(keyOrder(state));
    for (uint16_t key : keys)
        isListed.set(key);
    if (someLeaveSlots) {
        for (size_t i = 0; i < fields.size(); ++i) {
            if (!acceptedValues[i] && isListed.get(layout.indexOf(*fields[i])))
                return false;
        }
    }

    if (granuleBits)
        ensureExtensionGranules(vm, granuleBits);

    size_t numberOfKeysBefore = keys.size();
    for (size_t i = 0; i < fields.size(); ++i) {
        if (acceptedValues[i]) {
            bool didStore = store(vm, *fields[i], *acceptedValues[i]);
            RELEASE_ASSERT(didStore);
        }
        uint16_t index = layout.indexOf(*fields[i]);
        if (isListed.set(index))
            continue;
        keys.append(index);
        if (acceptedValues[i] && layout.isClass())
            state.objects->classes.didShadowMember(layout, *fields[i]);
    }
    if (keys.size() == numberOfKeysBefore)
        return true;
    setKeyOrder(vm, state, keys.span());
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Add, nullptr);
    if (!someLeaveSlots)
        return true;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (acceptedValues[i])
            continue;
        if (layout.isClass())
            state.objects->classes.didShadowMember(layout, *fields[i]);
        moveFieldToExtraProperties(vm, *fields[i], { JSValue::decode(values[i]), attributes[i] }, SlotExitReason::Redefinition);
    }
    return true;
}

namespace {

class FieldStoreRun {
public:
    FieldStoreRun(VM& vm, Object& target, State& state, AssignmentPlan& plan, JSObject* extra)
        : m_vm(vm)
        , m_target(target)
        , m_state(state)
        , m_plan(plan)
        , m_extra(extra)
    {
    }

    bool tryRunAsBefore(const AssignmentSource&);
    bool tryRunListingOnce(const AssignmentSource&);
    size_t emptySlotsFilled() const { return m_emptySlotsFilled; }

private:
    enum class CountEmptySlots : bool { No, Yes };
    template<CountEmptySlots> bool tryStoreEveryProperty(const AssignmentSource&);
    bool acceptsObject(AssignmentPlan::FieldStore&, JSObject*);
    bool isAccepted(AssignmentPlan::FieldStore&, JSValue);

    VM& m_vm;
    Object& m_target;
    State& m_state;
    AssignmentPlan& m_plan;
    JSObject* m_extra;
    bool m_didStoreCell { false };
    size_t m_emptySlotsFilled { 0 };
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
    default:
        return false;
    }
}

bool FieldStoreRun::acceptsObject(AssignmentPlan::FieldStore& store, JSObject* object)
{
    const RuntimeLayoutTable& layouts = m_state.objects->layouts;
    if (!layouts.fieldAccepts(*store.to, object))
        return false;
    const RuntimeType& type = layouts.typeOf(*store.to);
    bool acceptsEveryObjectOfStructure = !type.contents() && std::ranges::any_of(type.testsOfMoreThanOneFunction(), [&](const RuntimeCellTest& test) {
        return isDecidedByStructure(test, object) && layouts.cellPassesTest(test, object);
    });
    if (acceptsEveryObjectOfStructure) {
        store.lastAcceptedStructure = object->structureID();
        m_state.objects->caches->assignmentPlans.didWritePlan(m_plan);
    }
    return true;
}

ALWAYS_INLINE bool FieldStoreRun::isAccepted(AssignmentPlan::FieldStore& store, JSValue value)
{
    if (store.sourceAlwaysFits)
        return true;
    if (store.sourceFitsUnlessUndefinedOrNull && !value.isUndefinedOrNull())
        return true;
    if (value.isInt32() && value.asInt32() >= store.leastInt32 && value.asInt32() <= store.greatestInt32)
        return true;
    if (value.isObject())
        return value.asCell()->structureID() == store.lastAcceptedStructure || acceptsObject(store, asObject(value));
    bool isKindAccepted = store.acceptedKinds.containsAny(valueKindOf(value));
    ASSERT(!isKindAccepted || m_state.objects->layouts.fieldAccepts(*store.to, value));
    return isKindAccepted;
}

static NEVER_INLINE bool hasEveryExtraPropertyOfPlan(const AssignmentPlan& plan, const AssignmentSource& source)
{
    return std::ranges::all_of(plan.extraPropertyStores, [&](auto& store) {
        return !!source.read(store);
    });
}

enum class StoreToEveryName : bool { No, Yes };
static ALWAYS_INLINE JSObject* createEmptyExtraProperties(VM& vm, Structure* after, StoreToEveryName storeToEveryName)
{
    size_t capacity = after->outOfLineCapacity();
    Butterfly* butterfly = capacity ? Butterfly::createUninitialized(vm, nullptr, 0, capacity, false, 0) : nullptr;
    for (size_t index = storeToEveryName == StoreToEveryName::Yes ? after->outOfLineSize() : 0; index < capacity; ++index) {
        butterfly->propertyStorage()[-static_cast<ptrdiff_t>(index) - 1].clear();
        WTF::compilerFence();
    }
    std::array<WriteBarrierBase<Unknown>, extraPropertiesInlineCapacity> emptyInlineStorage;
    for (auto& slot : emptyInlineStorage)
        slot.clear();
    RELEASE_ASSERT(after->inlineCapacity() == extraPropertiesInlineCapacity);
    return JSFinalObject::createWithButterflyCopyingInlineStorage(vm, after, butterfly, emptyInlineStorage.data());
}

static bool storeNewExtraPropertiesFromOrdinaryObject(VM& vm, Object& target, const AssignmentPlan& plan, const JSObject* from)
{
    if (plan.addsExtraProperties)
        validateMutation(vm, target, ObjectMutation::AddProperty);
    JSObject* extra = createEmptyExtraProperties(vm, plan.extraPropertiesAfter, StoreToEveryName::Yes);
    std::array<const uint8_t*, 2> sourceStorages { reinterpret_cast<const uint8_t*>(from->inlineStorageUnsafe()), reinterpret_cast<const uint8_t*>(from->butterfly() ? from->outOfLineStorage() : nullptr) };
    std::array<uint8_t*, 2> targetStorages { reinterpret_cast<uint8_t*>(extra->inlineStorageUnsafe()), reinterpret_cast<uint8_t*>(extra->butterfly() ? extra->outOfLineStorage() : nullptr) };
    for (auto& store : plan.extraPropertyStores) {
        EncodedJSValue value = *reinterpret_cast<const EncodedJSValue*>(sourceStorages[store.isOutsideInSource] + store.bytesInSource);
        // The object goes unused, but the collector may find it on the stack and visit every name.
        if (!value) [[unlikely]] {
            for (size_t index = 0; index < extra->structure()->outOfLineCapacity(); ++index)
                extra->butterfly()->propertyStorage()[-static_cast<ptrdiff_t>(index) - 1].clear();
            return false;
        }
        *reinterpret_cast<EncodedJSValue*>(targetStorages[store.isOutsideInTarget] + store.bytesInTarget) = value;
    }
    target.setExtraProperties(vm, extra, plan.targetAfter);
    return true;
}

static bool storeExtraProperties(VM& vm, Object& target, const AssignmentPlan& plan, JSObject* extra, const AssignmentSource& source)
{
    if (target.structureID() != plan.target)
        return false;
    if (!extra && !source.compiledObject)
        return storeNewExtraPropertiesFromOrdinaryObject(vm, target, plan, source.object);
    if (plan.addsExtraProperties)
        validateMutation(vm, target, ObjectMutation::AddProperty);
    Structure* after = plan.extraPropertiesAfter;
    Structure* before = extra ? extra->structure() : nullptr;
    if (!extra)
        extra = createEmptyExtraProperties(vm, after, StoreToEveryName::No);
    else if (before->outOfLineCapacity() != after->outOfLineCapacity()) {
        if (source.compiledObject && !hasEveryExtraPropertyOfPlan(plan, source))
            return false;
        extra->nukeStructureAndSetButterfly(vm, before->id(), extra->allocateMoreOutOfLineStorage(vm, before->outOfLineCapacity(), after->outOfLineCapacity()));
    }
    for (auto& store : plan.extraPropertyStores) {
        JSValue value = source.read(store);
        if (!value)
            return false;
        extra->locationForOffset(store.offset)->setWithoutWriteBarrier(value);
    }
    vm.writeBarrier(extra);
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

static ALWAYS_INLINE uint64_t loadSlotBits(unsigned width, const void* slot)
{
    switch (width) {
    case 1:
        return *static_cast<const uint8_t*>(slot);
    case 2:
        return *static_cast<const uint16_t*>(slot);
    case 4:
        return *static_cast<const uint32_t*>(slot);
    case 8:
        return *static_cast<const uint64_t*>(slot);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE void* slotIn(const Object& object, Extension* extensionOrNull, const FieldData& field)
{
    if (!field.isOutOfLine()) [[likely]]
        return &object.at<uint8_t>(field.slot.offset);
    return extensionOrNull ? extensionOrNull->slotOrNull(field.slot.offset) : nullptr;
}

template<FieldStoreRun::CountEmptySlots countEmptySlots>
ALWAYS_INLINE bool FieldStoreRun::tryStoreEveryProperty(const AssignmentSource& source)
{
    if (m_extra) {
        for (auto& store : m_plan.extraPropertyStores) {
            if (!source.read(store))
                return false;
        }
    }
    constexpr size_t numberOfFieldsOnTheStack = 64;
    std::array<uint64_t, numberOfFieldsOnTheStack> bitsOnTheStack;
    Vector<uint64_t> bitsOfWideRun;
    uint64_t* bitsOfRun = bitsOnTheStack.data();
    if (m_plan.fieldStores.size() > numberOfFieldsOnTheStack) [[unlikely]] {
        bitsOfWideRun.grow(m_plan.fieldStores.size());
        bitsOfRun = bitsOfWideRun.mutableSpan().data();
    }
    uint64_t* bits = bitsOfRun;
    Extension* sourceExtension = m_plan.readsOutOfLineFields ? source.compiledObject->extension() : nullptr;
    using MovesFrom = AssignmentPlan::FieldStore::MovesFrom;
    for (AssignmentPlan::FieldStore& store : m_plan.fieldStores) {
        if (store.copiesBitsUntested) {
            const void* from = slotIn(*source.compiledObject, sourceExtension, *store.from);
            if (!from)
                return false;
            *bits = loadSlotBits(store.width, from);
            if (store.hasAbsent && *bits == store.absentBits)
                return false;
            ++bits;
            m_didStoreCell |= store.holdsCell;
            continue;
        }
        if (store.movesFrom != MovesFrom::Nowhere) {
            const uint8_t* place;
            if (store.movesFrom == MovesFrom::Cell)
                place = reinterpret_cast<const uint8_t*>(source.object);
            else if (store.movesFrom == MovesFrom::InlineProperties)
                place = reinterpret_cast<const uint8_t*>(source.propertyStorage->inlineStorageUnsafe());
            else
                place = reinterpret_cast<const uint8_t*>(source.propertyStorage->outOfLineStorage());
            place += store.bytesInSource;
            bool didMove = (store.movesFrom == MovesFrom::Cell || *reinterpret_cast<const uint64_t*>(place)) && tryMove(m_state.instance.cellCageBase, store.move, place, [&](uint64_t moved) ALWAYS_INLINE_LAMBDA {
                *bits = moved;
            }, [] {
                return false;
            });
            if (didMove) [[likely]] {
                ++bits;
                m_didStoreCell |= store.holdsCell;
                continue;
            }
        }
        JSValue value = source.read(store);
        if (store.writesInt32AsItIs && value.isInt32() && value.asInt32() >= store.leastInt32 && value.asInt32() <= store.greatestInt32) {
            *bits++ = static_cast<uint64_t>(static_cast<int64_t>(value.asInt32()));
            continue;
        }
        if (store.leavesUndefinedAndNullToOtherWay && value && value.isUndefinedOrNull())
            return false;
        if (value && !isAccepted(store, value)) {
            std::optional<AcceptedValue> accepted = m_state.objects->layouts.accept(*store.to, value);
            value = accepted ? accepted->value() : JSValue();
        }
        *bits = 0;
        if (!value || !tryWriteSlot(m_state.instance, store.to->slot.representation(), bits++, value))
            return false;
        m_didStoreCell |= store.holdsCell && value.isCell();
    }
    if (!m_plan.extraPropertyStores.isEmpty() && !storeExtraProperties(m_vm, m_target, m_plan, m_extra, source))
        return false;
    Extension* targetExtension = nullptr;
    if (m_plan.granuleBitsFilled) {
        m_target.ensureExtensionGranules(m_vm, m_plan.granuleBitsFilled);
        targetExtension = m_target.extension();
    }
    bits = bitsOfRun;
    for (const AssignmentPlan::FieldStore& store : m_plan.fieldStores) {
        void* to = slotIn(m_target, targetExtension, *store.to);
        RELEASE_ASSERT(to);
        if constexpr (countEmptySlots == CountEmptySlots::Yes)
            m_emptySlotsFilled += !store.hasAbsent || loadSlotBits(store.width, to) == store.absentBits;
        storeBitsOfWidth(store.width, static_cast<uint8_t*>(to), *bits++);
    }
    return true;
}

bool FieldStoreRun::tryRunAsBefore(const AssignmentSource& source)
{
    ASSERT(m_plan.orderWordBeforeRun == m_target.orderWord());
    if (!tryStoreEveryProperty<CountEmptySlots::No>(source)) [[unlikely]]
        return false;
    m_target.at<uint32_t>(offsetOfOrderWord) = m_plan.orderWordAfterRun;
    if (m_didStoreCell)
        m_vm.writeBarrier(&m_target);
    return true;
}

static uint32_t countExtraKeysOf(VM& vm, State& state, const Object& object)
{
    if (std::optional<uint32_t> number = object.numberOfExtraKeysByStructure())
        return *number;
    uint32_t number = 0;
    if (JSObject* extra = object.extraProperties()) {
        extra->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            number += isExtraKey(state, object, entry.key());
            return true;
        });
    }
    return number;
}

bool FieldStoreRun::tryRunListingOnce(const AssignmentSource& source)
{
    const LayoutData& layout = m_target.layout();
    uint32_t orderWord = m_target.orderWord();
    if (isOrderStoredOutOfLine(orderWord) || orderWord & (orderWordHasFieldOutsideSlot | orderWordIntegrityBits))
        return false;
    if (!std::ranges::all_of(m_plan.fieldStores, &AssignmentPlan::FieldStore::isDirect))
        return false;
    Vector<uint16_t, 32> keys;
    keys.append(m_target.keyOrder(m_state));
    size_t numberOfKeysBefore = keys.size();
    ExtraKeyCounter positioned(layout.numberOfFields);
    BitVector isListed;
    for (uint16_t key : keys) {
        if (key >= layout.numberOfFields)
            positioned.add(key);
        else
            isListed.set(key);
    }
    std::optional<uint32_t> numberOfExtraKeysBefore;
    uint32_t numberOfExtraKeysAdded = 0;
    for (uint16_t number : m_plan.storesInSourceOrder) {
        size_t index = number & ~AssignmentPlan::isExtraPropertyStore;
        if (number & AssignmentPlan::isExtraPropertyStore) {
            const AssignmentPlan::ExtraPropertyStore& store = m_plan.extraPropertyStores[index];
            numberOfExtraKeysAdded += store.addsName && !PropertyName(store.name).isPrivateName();
            continue;
        }
        uint16_t key = layout.indexOf(*m_plan.fieldStores[index].to);
        if (isListed.set(key))
            continue;
        if (!numberOfExtraKeysBefore)
            numberOfExtraKeysBefore = countExtraKeysOf(m_vm, m_state, m_target);
        positioned.appendEntriesCountingTo(*numberOfExtraKeysBefore + numberOfExtraKeysAdded, keys);
        keys.append(key);
    }
    if (!tryStoreEveryProperty<CountEmptySlots::Yes>(source))
        return false;
    if (keys.size() != numberOfKeysBefore)
        m_target.setKeyOrder(m_vm, m_state, keys.span());
    if (m_didStoreCell)
        m_vm.writeBarrier(&m_target);
    return true;
}

static bool tryFillEmptySlotsAsBefore(VM& vm, Object& target, State& state, const AssignmentPlan& plan, const Object& source)
{
    ASSERT(plan.storesOnlyFieldsThatFit && plan.orderWordBeforeFillingAll == target.orderWord() && plan.readsFrom(source.structureID(), source.orderWord(), plan.leavesElementsOut ? nullptr : source.extraProperties()));
    Extension* targetExtension = nullptr;
    if (plan.granuleBitsFilled) {
        target.ensureExtensionGranules(vm, plan.granuleBitsFilled);
        targetExtension = target.extension();
    }
    Extension* sourceExtension = plan.readsOutOfLineFields ? source.extension() : nullptr;
    std::span<const AssignmentPlan::FieldStore> stores = plan.fieldStores.span();
    size_t numberStored = 0;
    for (const AssignmentPlan::FieldStore& store : stores) {
        void* to = slotIn(target, targetExtension, *store.to);
        if (store.hasAbsent && loadSlotBits(store.width, to) != store.absentBits)
            break;
        if (store.copiesBits) {
            const void* from = slotIn(source, sourceExtension, *store.from);
            if (!from)
                break;
            uint64_t bits = loadSlotBits(store.width, from);
            if (store.hasAbsent && bits == store.absentBits)
                break;
            storeBitsOfWidth(store.width, static_cast<uint8_t*>(to), bits);
        } else {
            JSValue value = source.readInline(*store.from);
            if (!value || !tryWriteSlot(state.instance, store.to->slot.representation(), to, value))
                break;
        }
        ++numberStored;
    }
    if (numberStored != stores.size()) [[unlikely]] {
        for (const AssignmentPlan::FieldStore& store : stores.first(numberStored)) {
            if (store.hasAbsent)
                target.clear(*store.to);
        }
        return false;
    }
    target.at<uint32_t>(offsetOfOrderWord) = plan.orderWordAfterFillingAll;
    if (plan.mayStoreCells)
        vm.writeBarrier(&target);
    return true;
}

static std::optional<AssignmentSource> assignmentSourceFor(JSObject* source, std::span<UniquedStringImpl* const> excludedNames, bool defines)
{
    if (!isCompiledObject(source)) {
        if (source->type() != FinalObjectType || source->canHaveExistingOwnIndexedProperties())
            return std::nullopt;
        return AssignmentSource { source, nullptr, source, 0, excludedNames, defines };
    }
    auto* compiledSource = uncheckedDowncast<Object>(source);
    uint32_t orderWord = compiledSource->orderWord();
    if (isOrderStoredOutOfLine(orderWord))
        return std::nullopt;
    return AssignmentSource { source, compiledSource, compiledSource->extraProperties(), orderWord, excludedNames, defines, compiledSource->extension(), &compiledSource->instance() };
}

static ALWAYS_INLINE void assignByPlan(VM&, Object& target, State&, AssignmentPlan&, JSObject* extra, const AssignmentSource&);

static ALWAYS_INLINE uint64_t granuleBitsOf(const Extension* extensionOrNull) { return extensionOrNull ? extensionOrNull->granuleBits() : 0; }

static uint32_t offsetInExtensionWith(uint64_t granuleBits, uint32_t offset)
{
    ASSERT(granuleBits & granuleBitOf(offset));
    return offsetOfFirstExtensionSlot + std::popcount(granuleBits & (granuleBitOf(offset) - 1)) * extensionGranuleSize + offsetInGranule(offset);
}

static MappedCopy::MoveKind moveKindBetweenFormsOfIntegers(SlotRepresentation from, SlotRepresentation to)
{
    using Kind = MappedCopy::MoveKind;
    ASSERT(from != to);
    if (from == SlotRepresentation::OptionalInt32) {
        if (to == SlotRepresentation::JSValue)
            return Kind::BoxedOptionalInt32;
        if (to == SlotRepresentation::OptionalDouble)
            return Kind::OptionalInt32AsDouble;
        if (to == SlotRepresentation::OptionalInt64)
            return Kind::OptionalInt32AsOptionalInt64;
    }
    if (from == SlotRepresentation::OptionalInt64) {
        if (to == SlotRepresentation::JSValue)
            return Kind::BoxedOptionalInt64;
        if (to == SlotRepresentation::OptionalDouble)
            return Kind::OptionalInt64AsDouble;
        if (to == SlotRepresentation::OptionalInt32)
            return Kind::OptionalInt64AsOptionalInt32;
    }
    return Kind::OtherForm;
}

static MappedCopy::MoveKind moveKindUnlessMarker(SlotRepresentation from, SlotRepresentation to)
{
    using Kind = MappedCopy::MoveKind;
    if (from != to)
        return moveKindBetweenFormsOfIntegers(from, to);
    if (from == SlotRepresentation::OptionalInt32)
        return Kind::Int32AsOptionalInt32;
    if (from == SlotRepresentation::OptionalInt64)
        return Kind::OptionalInt64UnlessMarker;
    if (from == SlotRepresentation::OptionalDouble)
        return Kind::OptionalDoubleUnlessMarker;
    if (from == SlotRepresentation::OptionalBoolean)
        return Kind::OptionalBooleanUnlessMarker;
    if (from == SlotRepresentation::JSValue)
        return Kind::JSValueUnlessMarker;
    return Kind::OtherForm;
}

MappedCopy::MoveKind moveKindFor(SlotRepresentation from, SlotRepresentation to, SlotFit fit)
{
    using Kind = MappedCopy::MoveKind;
    if (fit == SlotFit::IsTested)
        return Kind::Tested;
    if (fit == SlotFit::UnlessUndefinedOrNull)
        return moveKindUnlessMarker(from, to);
    auto isNarrowReference = [](SlotRepresentation representation) {
        return representation == SlotRepresentation::Cell32 || representation == SlotRepresentation::OptionalCell32;
    };
    auto isDouble = [](SlotRepresentation representation) {
        return representation == SlotRepresentation::Double || representation == SlotRepresentation::OptionalDouble;
    };
    if (from != to) {
        static_assert(nullCell32 == JSValue::ValueNull && undefinedCell32 == JSValue::ValueUndefined);
        if (isNarrowReference(from) && isNarrowReference(to))
            return Kind::HalfWordUnlessZero;
        if (isNarrowReference(from) && (to == SlotRepresentation::JSValue || to == SlotRepresentation::Cell))
            return Kind::WideNarrowReference;
        if ((from == SlotRepresentation::JSValue || from == SlotRepresentation::Cell) && isNarrowReference(to))
            return Kind::NarrowWideReference;
        if (from == SlotRepresentation::Int32 && to == SlotRepresentation::JSValue)
            return Kind::BoxedInt32;
        if (from == SlotRepresentation::Int32 && to == SlotRepresentation::OptionalInt32)
            return Kind::Int32AsOptionalInt32;
        if (from == SlotRepresentation::Int32 && isDouble(to))
            return Kind::Int32AsDouble;
        if (from == SlotRepresentation::Int32 && (to == SlotRepresentation::Int64 || to == SlotRepresentation::OptionalInt64))
            return Kind::Int32AsInt64;
        return moveKindBetweenFormsOfIntegers(from, to);
    }
    if (from == SlotRepresentation::OptionalInt32)
        return Kind::OptionalInt32;
    if (from == SlotRepresentation::OptionalInt64)
        return Kind::OptionalInt64;
    if (from == SlotRepresentation::OptionalDouble)
        return Kind::OptionalDouble;
    const SlotTraits traits = traitsOf(from);
    if (traits.absentBits)
        return Kind::OtherForm;
    switch (traits.width) {
    case 8:
        return traits.hasAbsent ? Kind::WordUnlessZero : Kind::Word;
    case 4:
        return traits.hasAbsent ? Kind::HalfWordUnlessZero : Kind::HalfWord;
    case 2:
        RELEASE_ASSERT(!traits.hasAbsent);
        return Kind::QuarterWord;
    case 1:
        return traits.hasAbsent ? Kind::ByteUnlessZero : Kind::Byte;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const MappedCopy*& placeOfCopyWithoutFrame(State& state, const MappedCopy& copy)
{
    return state.instance.copiesWithoutFrame()[AllocationWords::placeOfCopyWithoutFrame(copy.targetWords, copy.sourceWords, copy.excludedRunNumberPlusOne)];
}

static bool tryDescribeForStub(MappedCopy::Move& move)
{
    using Kind = MappedCopy::MoveKind;
    using Run = MappedCopy::RunOfStub;
    auto isMadeIn = [&](Run run, uint64_t firstBitsRefused, uint64_t numberOfBitsRefused) {
        move.runOfStub = run;
        move.firstBitsRefusedByStub = firstBitsRefused;
        move.numberOfBitsRefusedByStub = numberOfBitsRefused;
        return true;
    };
    switch (move.kind) {
    case Kind::HalfWord:
        return isMadeIn(Run::HalfWords, 0, 0);
    case Kind::HalfWordUnlessZero:
        return isMadeIn(Run::HalfWords, 0, 1);
    case Kind::OptionalInt32:
        return isMadeIn(Run::HalfWords, traitsOf(SlotRepresentation::OptionalInt32).absentBits, 1);
    case Kind::Int32AsOptionalInt32:
        return isMadeIn(Run::HalfWords, static_cast<uint32_t>(INT32_MIN), static_cast<int64_t>(minInlineInt32) - INT32_MIN);
    case Kind::NarrowWideReference:
        return isMadeIn(Run::NarrowedReferences, 0, 0);
    case Kind::Word:
        return isMadeIn(Run::Words, 0, 0);
    case Kind::WordUnlessZero:
        return isMadeIn(Run::Words, 0, 1);
    case Kind::OptionalInt64:
        return isMadeIn(Run::Words, traitsOf(SlotRepresentation::OptionalInt64).absentBits, 1);
    case Kind::OptionalDouble:
        return isMadeIn(Run::Words, traitsOf(SlotRepresentation::OptionalDouble).absentBits, 1);
    case Kind::OptionalInt64UnlessMarker:
        return isMadeIn(Run::Words, undefinedInteger, absentInteger - undefinedInteger + 1);
    case Kind::OptionalDoubleUnlessMarker:
        return isMadeIn(Run::Words, undefinedDouble, absentDouble - undefinedDouble + 1);
    case Kind::JSValueUnlessMarker:
        return isMadeIn(Run::Words, JSValue::ValueEmpty, JSValue::ValueUndefined + 1);
    case Kind::Byte:
        return isMadeIn(Run::Bytes, 0, 0);
    case Kind::ByteUnlessZero:
        return isMadeIn(Run::Bytes, 0, 1);
    case Kind::OptionalBooleanUnlessMarker:
        return isMadeIn(Run::Bytes, JSValue::ValueTrue + 1, UINT32_MAX - 1);
    case Kind::OptionalInt32AsDouble:
        return isMadeIn(Run::Int32sAsDoubles, static_cast<uint32_t>(INT32_MIN), static_cast<int64_t>(minInlineInt32) - INT32_MIN);
    case Kind::Int32AsDouble:
        return isMadeIn(Run::Int32sAsDoubles, 0, 0);
    case Kind::BoxedOptionalInt32:
        return isMadeIn(Run::BoxedInt32s, static_cast<uint32_t>(INT32_MIN), static_cast<int64_t>(minInlineInt32) - INT32_MIN);
    case Kind::BoxedInt32:
        return isMadeIn(Run::BoxedInt32s, 0, 0);
    case Kind::WideNarrowReference:
        return isMadeIn(Run::WidenedReferences, 0, 1);
    case Kind::QuarterWord:
    case Kind::OptionalInt32AsOptionalInt64:
    case Kind::Int32AsInt64:
    case Kind::BoxedOptionalInt64:
    case Kind::OptionalInt64AsOptionalInt32:
    case Kind::OptionalInt64AsDouble:
    case Kind::BoxedAsInt32:
    case Kind::BoxedAsInt64:
    case Kind::BoxedAsDouble:
    case Kind::BoxedAsNarrowString:
    case Kind::BoxedAsJSValue:
    case Kind::OtherForm:
    case Kind::Tested:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void describeMovesForStub(MappedCopy& copy)
{
    if (copy.granuleBitsOfSource || copy.granuleBitsFilled)
        return;
    for (MappedCopy::Move& move : copy.moves) {
        if (!tryDescribeForStub(move))
            return;
    }
    Vector<MappedCopy::Move> movesByRun;
    movesByRun.reserveInitialCapacity(copy.moves.size());
    for (unsigned run = 0; run < MappedCopy::numberOfRunsOfStub; ++run) {
        for (const MappedCopy::Move& move : copy.moves) {
            if (static_cast<unsigned>(move.runOfStub) == run)
                movesByRun.append(move);
        }
    }
    copy.moves = WTF::move(movesByRun);
    for (const MappedCopy::Move& move : copy.moves) {
        MappedCopy::Run& run = copy.runsOfStub[static_cast<size_t>(move.runOfStub)];
        run.end = &move + 1;
        run.negativeSize -= sizeof(MappedCopy::Move);
    }
    auto hasMovesBehind = [&](MappedCopy::RunOfStub run) {
        return std::ranges::any_of(std::span { copy.runsOfStub }.subspan(static_cast<size_t>(run) + 1), &MappedCopy::Run::negativeSize);
    };
    copy.hasRunsBehindSecondForStub = hasMovesBehind(MappedCopy::RunOfStub::NarrowedReferences);
    copy.hasRunsBehindFourthForStub = hasMovesBehind(MappedCopy::RunOfStub::Bytes);
    copy.structureOfSourceForStub = copy.structureOfSource;
}

static bool tryDescribeNewExtension(VM& vm, MappedCopy& copy, const Object& target)
{
    SlotData word = *target.compiledStructure().extensionSlot();
    copy.offsetOfExtensionWordInTarget = word.offset;
    copy.extensionWordOfTargetIsWide = word.representation() == SlotRepresentation::Cell;
    Allocator allocator = vm.heap.cagedAuxiliarySpace<SubspaceAccess::OnMainThread>()->allocatorFor(extensionCellSizeFor(std::popcount(copy.granuleBitsFilled)), AllocatorForMode::EnsureAllocator);
    if (!allocator)
        return false;
    copy.allocatorOfNewExtension = allocator.localAllocator();
    copy.sizeOfNewExtension = copy.allocatorOfNewExtension->cellSize();
    return true;
}

static bool takesNoFrame(VM& vm, MappedCopy& copy, const Object& target, const Object& source)
{
    for (const MappedCopy::Move& move : copy.moves) {
        if (move.kind == MappedCopy::MoveKind::OtherForm || move.kind == MappedCopy::MoveKind::Tested)
            return false;
    }
    if (copy.granuleBitsOfSource) {
        SlotData word = *source.compiledStructure().extensionSlot();
        copy.offsetOfExtensionWordInSource = word.offset;
        copy.extensionWordOfSourceIsWide = word.representation() == SlotRepresentation::Cell;
    }
    if (copy.granuleBitsFilled && !tryDescribeNewExtension(vm, copy, target))
        return false;
    copy.structureOfTarget = target.structureID();
    copy.structureOfSource = source.structureID();
    describeMovesForStub(copy);
    return true;
}

static NEVER_INLINE void offerCopyWithoutFrame(VM& vm, State& state, MappedCopy& copy, const Object& target, const Object& source)
{
    if (copy.mayTakeNoFrame) {
        copy.mayTakeNoFrame = false;
        copy.takesNoFrame = takesNoFrame(vm, copy, target, source);
    }
    if (copy.takesNoFrame)
        placeOfCopyWithoutFrame(state, copy) = &copy;
}

bool clearFieldsMovedInVain(const MappedCopy& copy, const MappedCopy::Move* end, JSCell* target)
{
    if (copy.numberOfCopiesInVain < UINT8_MAX)
        ++copy.numberOfCopiesInVain;
    for (const MappedCopy::Move* move = copy.moves.begin(); move != end; ++move) {
        if (!move->writesExtension && !move->overwritesField)
            clearSlot(move->representationOfTarget, reinterpret_cast<uint8_t*>(target) + move->to);
    }
    return false;
}

static NEVER_INLINE bool tryDescribeMappedCopy(VM& vm, State& state, MappedCopy& copy, const Object& target, const Object& source, uint32_t excludedRunNumberPlusOne)
{
    AssignmentPlanCache& plans = state.objects->caches->assignmentPlans;
    const LayoutData& targetLayout = target.layout();
    const LayoutData& sourceLayout = source.layout();
    FieldMap* map = plans.fieldMapFor(targetLayout, sourceLayout, excludedRunNumberPlusOne);
    if (!map)
        return false;
    if (const MappedCopy*& place = placeOfCopyWithoutFrame(state, copy); place == &copy)
        place = nullptr;
    copy = { };
    copy.targetWords = target.orderAndShapeWordsWithMark();
    copy.sourceWords = source.orderAndShapeWordsWithMark();
    copy.excludedRunNumberPlusOne = excludedRunNumberPlusOne;
    copy.state = MappedCopy::State::IsNotServed;
    uint32_t orderWordOfSource = source.orderWord();
    std::span<const uint16_t> entries = state.objects->keyOrders.entriesListedBy(sourceLayout, orderWordOfSource);
    Vector<uint16_t, 32> keysAfter(target.keyOrder(state));
    BitVector isListed;
    for (uint16_t key : keysAfter) {
        if (key >= targetLayout.numberOfFields)
            return false;
        isListed.set(key);
    }
    copy.granuleBitsOfSource = sourceLayout.hasExtension() ? granuleBitsOf(source.extension()) : 0;
    copy.granuleBitsOfTargetBefore = targetLayout.hasExtension() ? granuleBitsOf(target.extension()) : 0;
    Vector<MappedCopy::Move> moves;
    moves.reserveInitialCapacity(entries.size());
    for (uint16_t index : entries) {
        if (index >= sourceLayout.numberOfFields)
            return false;
        FieldMap::Entry& entry = map->entries[index];
        if (entry.state == FieldMap::isUnknown)
            entry = plans.describeMappedField(vm, targetLayout, sourceLayout.fields()[index], excludedRunNumberPlusOne);
        if (entry.state == FieldMap::isNotServed)
            return false;
        if (entry.state == FieldMap::isLeftOut)
            continue;
        uint16_t key = entry.fieldOfTarget();
        const FieldData& from = sourceLayout.fields()[index];
        const FieldData& to = targetLayout.fields()[key];
        if (from.isOutOfLine() && !(copy.granuleBitsOfSource & granuleBitOf(from.slot.offset)))
            return false;
        moves.append(MappedCopy::Move { from.slot.offset, to.slot.offset, entry.kind, static_cast<uint8_t>(widthOf(to.slot.representation())),
            from.isOutOfLine(), to.isOutOfLine(), from.slot.representation(), to.slot.representation(), entry.acceptedKinds, key, false, !!(entry.state & FieldMap::testsUndefinedAndNull) });
        copy.mayStoreCells |= traitsOf(to.slot.representation()).holdsCell;
        if (to.isOutOfLine())
            copy.granuleBitsFilled |= granuleBitOf(to.slot.offset);
        moves.last().overwritesField = isListed.set(key);
        if (!moves.last().overwritesField)
            keysAfter.append(key);
    }
    for (MappedCopy::Move& move : moves) {
        if (move.readsExtension)
            move.from = offsetInExtensionWith(copy.granuleBitsOfSource, move.from);
        if (move.writesExtension)
            move.to = offsetInExtensionWith(copy.granuleBitsOfTargetBefore | copy.granuleBitsFilled, move.to);
    }
    copy.orderWordAfter = target.orderWord();
    if (keysAfter.size() != countOf(copy.orderWordAfter)) {
        std::optional<uint32_t> numbered = state.objects->keyOrders.orderStartingWith(targetLayout, keysAfter.span());
        if (!numbered)
            return false;
        copy.orderWordAfter = *numbered;
    }
    copy.moves = WTF::move(moves);
    copy.state = MappedCopy::State::IsServed;
    copy.mayTakeNoFrame = !copy.granuleBitsOfTargetBefore;
    return true;
}

static NEVER_INLINE bool tryReadInOtherWay(State& state, const MappedCopy::Move& move, std::span<const FieldData> fieldsOfTarget, const uint8_t* slot, uint64_t& bits)
{
    JSValue value = readSlot(move.representationOfSource, slot, state.instance);
    if (!value)
        return false;
    bool isTested = move.kind == MappedCopy::MoveKind::Tested ? !OptionSet<ValueKind>::fromRaw(move.acceptedKinds).containsAny(valueKindOf(value)) : move.testsUndefinedAndNull && value.isUndefinedOrNull();
    if (isTested) {
        std::optional<AcceptedValue> accepted = state.objects->layouts.accept(fieldsOfTarget[move.fieldOfTarget], value);
        if (!accepted)
            return false;
        value = accepted->value();
    }
    bits = 0;
    return tryWriteSlot(state.instance, move.representationOfTarget, &bits, value);
}

static ALWAYS_INLINE RepeatedCopy tryCopyByFieldMap(VM& vm, State& state, Object& target, const Object& source, uint32_t excludedRunNumberPlusOne)
{
    uint32_t orderWordOfTarget = target.orderWord();
    uint32_t orderWordOfSource = source.orderWord();
    if ((orderWordOfTarget | orderWordOfSource) & ~orderAndCountMask || isOrderStoredOutOfLine(orderWordOfTarget) || isOrderStoredOutOfLine(orderWordOfSource))
        return RepeatedCopy::IsNotDone;
    JSGlobalObject* realm = state.instance.globalObject;
    if (target.structure()->realm() != realm || source.structure()->realm() != realm)
        return RepeatedCopy::IsNotDone;
    uint64_t targetWords = target.orderAndShapeWordsWithMark();
    uint64_t sourceWords = source.orderAndShapeWordsWithMark();
    MappedCopy& copy = state.objects->caches->assignmentPlans.mappedCopyAt(targetWords, sourceWords, excludedRunNumberPlusOne);
    bool isFirstCopy = !copy.isFor(targetWords, sourceWords, excludedRunNumberPlusOne);
    if (isFirstCopy && !tryDescribeMappedCopy(vm, state, copy, target, source, excludedRunNumberPlusOne))
        return RepeatedCopy::IsNotDone;
    if (copy.state != MappedCopy::State::IsServed)
        return RepeatedCopy::IsNotDone;

    const Extension* extensionOfSource = copy.granuleBitsOfSource ? source.extension() : nullptr;
    if (granuleBitsOf(extensionOfSource) != copy.granuleBitsOfSource)
        return RepeatedCopy::IsNotDone;
    Extension* extensionOfTargetBefore = copy.granuleBitsOfTargetBefore | copy.granuleBitsFilled ? target.extension() : nullptr;
    if (granuleBitsOf(extensionOfTargetBefore) != copy.granuleBitsOfTargetBefore)
        return RepeatedCopy::IsNotDone;

    DeferGC deferGC(vm);
    constexpr size_t numberOfValuesOnTheStack = 64;
    std::array<uint64_t, numberOfValuesOnTheStack> valuesOnTheStack;
    Vector<uint64_t> valuesOfWideCopy;
    uint64_t* values = valuesOnTheStack.data();
    if (copy.moves.size() > numberOfValuesOnTheStack) [[unlikely]] {
        valuesOfWideCopy.grow(copy.moves.size());
        values = valuesOfWideCopy.mutableSpan().data();
    }
    uint64_t* value = values;
    const uint8_t* cellOfSource = &source.at<uint8_t>(0);
    std::span<const FieldData> fieldsOfTarget = target.layout().fields();
    for (const MappedCopy::Move& move : copy.moves) {
        const uint8_t* slot = (move.readsExtension ? reinterpret_cast<const uint8_t*>(extensionOfSource) : cellOfSource) + move.from;
        uint64_t& bits = *value++;
        bool didRead = tryMove(state.instance.cellCageBase, move, slot, [&](uint64_t read) ALWAYS_INLINE_LAMBDA {
            bits = read;
        }, [&]() ALWAYS_INLINE_LAMBDA {
            return tryReadInOtherWay(state, move, fieldsOfTarget, slot, bits);
        });
        if (!didRead)
            return RepeatedCopy::IsNotDone;
    }

    Extension* newExtension = copy.granuleBitsFilled && !extensionOfTargetBefore ? Extension::create(vm, copy.granuleBitsFilled) : nullptr;
    Extension* extensionOfTarget = newExtension;
    if (copy.granuleBitsFilled && !newExtension) {
        target.ensureExtensionGranules(vm, copy.granuleBitsFilled);
        extensionOfTarget = target.extension();
    }
    uint8_t* cellOfTarget = &target.at<uint8_t>(0);
    value = values;
    for (const MappedCopy::Move& move : copy.moves)
        storeBitsOfWidth(move.widthInTarget, (move.writesExtension ? reinterpret_cast<uint8_t*>(extensionOfTarget) : cellOfTarget) + move.to, *value++);
    if (newExtension)
        target.setExtension(vm, newExtension);
    target.at<uint32_t>(offsetOfOrderWord) = copy.orderWordAfter;
    if (copy.mayStoreCells)
        vm.writeBarrier(&target);
    didChangeOwnProperties(vm, target, VM::StructureChainIntegrityEvent::Add, nullptr);
    if ((copy.takesNoFrame || copy.mayTakeNoFrame) && !isFirstCopy) [[unlikely]]
        offerCopyWithoutFrame(vm, state, copy, target, source);
    return isFirstCopy ? RepeatedCopy::ByMapForTheFirstTime : RepeatedCopy::ByMap;
}

static ALWAYS_INLINE RepeatedCopy tryRunPlanAsBefore(VM& vm, State& state, Object& to, AssignmentPlan& plan, const AssignmentSource& source, uint32_t excludedRunNumberPlusOne)
{
    if (!plan.runsAsBeforeOn(to, source, excludedRunNumberPlusOne))
        return RepeatedCopy::IsNotDone;
    DeferGC deferGC(vm);
    FieldStoreRun run(vm, to, state, plan, nullptr);
    if (!run.tryRunAsBefore(source))
        return RepeatedCopy::IsNotDone;
    if (to.structure()->mayBePrototype()) [[unlikely]]
        didChangeOwnProperties(vm, to, VM::StructureChainIntegrityEvent::Add, nullptr);
    return RepeatedCopy::AsThePlanDidBefore;
}

static RepeatedCopy runPlanAsBeforeAndOfferCopy(VM&, State&, Object& to, AssignmentPlan&, const AssignmentSource&, uint32_t excludedRunNumberPlusOne);

static NEVER_INLINE RepeatedCopy tryRunPlanMadeBefore(VM& vm, State& state, Object& to, const AssignmentSource& source)
{
    if (to.orderWord() & orderWordIntegrityBits)
        return RepeatedCopy::IsNotDone;
    AssignmentPlanCache& plans = state.objects->caches->assignmentPlans;
    JSObject* extra = to.extraProperties();
    AssignmentPlan* plan = plans.planMadeFor(to.structureID(), extra ? extra->structureID() : StructureID(), source);
    if (!plan || !plan->isUsable || plan->orderWordBeforeRun != to.orderWord())
        return RepeatedCopy::IsNotDone;
    DeferGC deferGC(vm);
    FieldStoreRun run(vm, to, state, *plan, extra);
    if (!run.tryRunAsBefore(source))
        return RepeatedCopy::IsNotDone;
    plans.didRunAsBefore(*plan);
    didChangeOwnProperties(vm, to, VM::StructureChainIntegrityEvent::Add, nullptr);
    return RepeatedCopy::AsThePlanDidBefore;
}

static ALWAYS_INLINE RepeatedCopy tryRunRecentPlanOrPlanMadeBefore(VM& vm, State& state, Object& to, AssignmentPlan* recentOrNull, const AssignmentSource& source, uint32_t excludedRunNumberPlusOne)
{
    if (recentOrNull) [[likely]] {
        RepeatedCopy way = recentOrNull->mayCopyWithoutFrame ? runPlanAsBeforeAndOfferCopy(vm, state, to, *recentOrNull, source, excludedRunNumberPlusOne) : tryRunPlanAsBefore(vm, state, to, *recentOrNull, source, excludedRunNumberPlusOne);
        if (way != RepeatedCopy::IsNotDone || recentOrNull->runsAsBeforeOn(to, source, excludedRunNumberPlusOne)) [[likely]]
            return way;
    }
    return excludedRunNumberPlusOne ? RepeatedCopy::IsNotDone : tryRunPlanMadeBefore(vm, state, to, source);
}

static NEVER_INLINE RepeatedCopy tryRepeatAssignmentByRecentPlan(VM& vm, State& state, Object& to, const Object& from, uint32_t excludedRunNumberPlusOne)
{
    if (&to == &from)
        return RepeatedCopy::IsNotDone;
    JSObject* extraPropertiesOfSource = from.extraProperties();
    AssignmentPlan* plan = state.objects->caches->assignmentPlans.recentPlan(to.structureID(), from.structureID(), extraPropertiesOfSource ? extraPropertiesOfSource->structureID() : StructureID(), from.orderWord(), excludedRunNumberPlusOne);
    if (!plan || !plan->fillsEmptySlotsOf(to, from, excludedRunNumberPlusOne)) {
        AssignmentSource source { const_cast<Object*>(&from), &from, extraPropertiesOfSource, from.orderWord(), { }, true, from.extension(), &from.instance() };
        return tryRunRecentPlanOrPlanMadeBefore(vm, state, to, plan, source, excludedRunNumberPlusOne);
    }
    DeferGC deferGC(vm);
    if (!tryFillEmptySlotsAsBefore(vm, to, state, *plan, from))
        return RepeatedCopy::IsNotDone;
    didChangeOwnProperties(vm, to, VM::StructureChainIntegrityEvent::Add, nullptr);
    return RepeatedCopy::AsThePlanDidBefore;
}

static NEVER_INLINE RepeatedCopy tryRepeatAssignmentBetweenLayouts(VM& vm, State& state, Object& to, const Object& from, uint32_t excludedRunNumberPlusOne)
{
    if (&to != &from) {
        if (RepeatedCopy way = tryCopyByFieldMap(vm, state, to, from, excludedRunNumberPlusOne); way != RepeatedCopy::IsNotDone)
            return way;
    }
    return tryRepeatAssignmentByRecentPlan(vm, state, to, from, excludedRunNumberPlusOne);
}

std::optional<MappedCopy::MoveKind> moveKindOutOfOrdinaryObject(const RuntimeLayoutTable& layouts, const AssignmentPlan::FieldStore& store)
{
    using Kind = MappedCopy::MoveKind;
    const FieldData& to = *store.to;
    if (store.from || !store.isDirect || to.holdsRawValueFromBirth || to.flags & fieldAttributeFlags)
        return std::nullopt;
    switch (to.slot.representation()) {
    case SlotRepresentation::Int32:
    case SlotRepresentation::OptionalInt32:
        return store.writesInt32AsItIs ? std::optional(Kind::BoxedAsInt32) : std::nullopt;
    case SlotRepresentation::Int64:
    case SlotRepresentation::OptionalInt64:
        return store.writesInt32AsItIs ? std::optional(Kind::BoxedAsInt64) : std::nullopt;
    case SlotRepresentation::Double:
    case SlotRepresentation::OptionalDouble:
        return Kind::BoxedAsDouble;
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
        return store.acceptedKinds.contains(ValueKind::String) ? std::optional(Kind::BoxedAsNarrowString) : std::nullopt;
    case SlotRepresentation::JSValue:
        return acceptsEveryValue(layouts.typeOf(to)) && !store.leavesUndefinedAndNullToOtherWay ? Kind::JSValueUnlessMarker : Kind::BoxedAsJSValue;
    default:
        return std::nullopt;
    }
}

static bool tryDescribeCopyOutOfOrdinaryObject(VM& vm, State& state, const AssignmentPlan& plan, MappedCopy& copy, const Object& target, uint32_t excludedRunNumberPlusOne)
{
    if (plan.orderWordBeforeRun != unorderedOrderWord || !plan.extraPropertyStores.isEmpty() || plan.orderWordAfterRun & ~orderAndCountMask || isOrderStoredOutOfLine(plan.orderWordAfterRun))
        return false;
    copy.moves.reserveInitialCapacity(plan.fieldStores.size());
    for (const AssignmentPlan::FieldStore& store : plan.fieldStores) {
        std::optional<MappedCopy::MoveKind> kind = moveKindOutOfOrdinaryObject(state.objects->layouts, store);
        if (!kind || !isInlineOffset(store.sourceOffset))
            return false;
        const FieldData& to = *store.to;
        SlotRepresentation representation = to.slot.representation();
        uint32_t offsetInTarget = to.isOutOfLine() ? offsetInExtensionWith(plan.granuleBitsFilled, to.slot.offset) : to.slot.offset;
        MappedCopy::Move move { static_cast<uint32_t>(JSObject::offsetOfInlineStorage() + store.sourceOffset * sizeof(EncodedJSValue)), offsetInTarget, *kind, static_cast<uint8_t>(widthOf(representation)),
            false, to.isOutOfLine(), SlotRepresentation::JSValue, representation, store.acceptedKinds.toRaw(), target.layout().indexOf(to) };
        move.leastInt32 = store.leastInt32;
        move.greatestInt32 = store.greatestInt32;
        copy.moves.append(move);
        copy.mayStoreCells |= traitsOf(representation).holdsCell;
    }
    copy.granuleBitsFilled = plan.granuleBitsFilled;
    if (copy.granuleBitsFilled && !tryDescribeNewExtension(vm, copy, target))
        return false;
    copy.targetWords = static_cast<uint64_t>(target.shapeWordWithMark()) << 32 | unorderedOrderWord;
    copy.sourceWords = 0;
    copy.readsOrdinaryObject = true;
    copy.granuleBitsOfSource = UINT64_MAX;
    copy.excludedRunNumberPlusOne = excludedRunNumberPlusOne;
    copy.orderWordAfter = plan.orderWordAfterRun;
    copy.structureOfTarget = plan.target;
    copy.structureOfSource = plan.source;
    copy.takesNoFrame = true;
    copy.placeOfOwnedCopy = &placeOfCopyWithoutFrame(state, copy);
    return true;
}

static bool tryDescribeCopyFollowingPlan(VM& vm, State& state, const AssignmentPlan& plan, MappedCopy& copy, const Object& target, const AssignmentSource& source, uint32_t excludedRunNumberPlusOne)
{
    using Place = MappedCopy::Place;
    using MovesFrom = AssignmentPlan::FieldStore::MovesFrom;
    copy.followedPlan = makeUnique<MappedCopy::FollowedPlan>();
    MappedCopy::FollowedPlan& followed = *copy.followedPlan;
    if (source.compiledObject && &source.compiledObject->state() != &state)
        return false;
    uint32_t flagsAfter = plan.orderWordAfterRun & ~orderAndCountMask;
    bool makesSecondObject = !plan.extraPropertyStores.isEmpty();
    if (plan.orderWordBeforeRun != unorderedOrderWord || plan.granuleBitsFilled || plan.readsOutOfLineFields || isOrderStoredOutOfLine(plan.orderWordAfterRun) || flagsAfter != (makesSecondObject ? orderWordHasExtraProperties : 0))
        return false;
    static_assert(static_cast<unsigned>(MovesFrom::Cell) == static_cast<unsigned>(Place::Cell) + 1 && static_cast<unsigned>(MovesFrom::InlineProperties) == static_cast<unsigned>(Place::InlineProperties) + 1
        && static_cast<unsigned>(MovesFrom::OutsideProperties) == static_cast<unsigned>(Place::OutsideProperties) + 1);
    for (AssignmentPlan::FieldStore store : plan.fieldStores) {
        if (store.copiesBitsUntested)
            state.objects->caches->assignmentPlans.describeMove(vm, store, target.layout(), true);
        if (store.movesFrom == MovesFrom::Nowhere || store.to->isOutOfLine())
            return false;
        MappedCopy::Move move = store.move;
        move.from = static_cast<uint32_t>(store.bytesInSource);
        move.to = store.to->slot.offset;
        move.placeInSource = static_cast<Place>(static_cast<unsigned>(store.movesFrom) - 1);
        copy.moves.append(move);
    }
    auto bytesOf = [](PropertyOffset offset) {
        return static_cast<int32_t>((isInlineOffset(offset) ? offset : offsetInOutOfLineStorage(offset)) * sizeof(EncodedJSValue));
    };
    auto placeOf = [](PropertyOffset offset) {
        return isInlineOffset(offset) ? Place::InlineProperties : Place::OutsideProperties;
    };
    for (const AssignmentPlan::ExtraPropertyStore& store : plan.extraPropertyStores) {
        if (!store.from) {
            followed.wordMoves.append({ bytesOf(store.sourceOffset), bytesOf(store.offset), placeOf(store.sourceOffset), placeOf(store.offset) });
            continue;
        }
        if (store.from->isOutOfLine())
            return false;
        SlotRepresentation representation = store.from->slot.representation();
        MappedCopy::Move move { store.from->slot.offset, static_cast<uint32_t>(bytesOf(store.offset)), moveKindFor(representation, SlotRepresentation::JSValue, SlotFit::Always), sizeof(EncodedJSValue), false, true, representation, SlotRepresentation::JSValue, 0, 0 };
        if (move.kind == MappedCopy::MoveKind::OtherForm || move.kind == MappedCopy::MoveKind::Tested)
            return false;
        move.placeInTarget = placeOf(store.offset);
        copy.moves.append(move);
    }
    copy.moves.shrinkToFit();
    followed.wordMoves.shrinkToFit();
    if (source.compiledObject && source.propertyStorage) {
        std::optional<SlotData> word = source.compiledObject->compiledStructure().extraPropertiesSlot();
        if (!word)
            return false;
        followed.readsSecondObjectOfSource = true;
        followed.offsetOfSecondObjectWordInSource = word->offset;
        followed.secondObjectWordOfSourceIsWide = word->representation() == SlotRepresentation::Cell;
        followed.structureOfSecondObjectOfSource = plan.sourcePropertyStorage;
    }
    if (makesSecondObject) {
        std::optional<SlotData> word = target.compiledStructure().extraPropertiesSlot();
        Structure* after = plan.extraPropertiesAfter;
        if (!word || !plan.targetAfter || after->inlineCapacity() != extraPropertiesInlineCapacity || after->isDictionary() || plan.extraPropertyStores.size() != static_cast<size_t>(after->inlineSize() + after->outOfLineSize()))
            return false;
        if (cellHeaderFor(plan.targetAfter) >> 32 != cellHeaderFor(plan.target.decode()) >> 32)
            return false;
        followed.offsetOfSecondObjectWordInTarget = word->offset;
        followed.secondObjectWordOfTargetIsWide = word->representation() == SlotRepresentation::Cell;
        followed.structureOfTargetAfter = plan.targetAfter->id();
        followed.headerOfNewSecondObject = cellHeaderFor(after);
        Allocator allocator = subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(extraPropertiesInlineCapacity), AllocatorForMode::EnsureAllocator);
        if (!allocator)
            return false;
        followed.allocatorOfNewSecondObject = allocator.localAllocator();
        followed.sizeOfNewSecondObject = followed.allocatorOfNewSecondObject->cellSize();
        if (size_t capacity = after->outOfLineCapacity()) {
            followed.bytesOfNewStorage = static_cast<uint32_t>(capacity * sizeof(EncodedJSValue));
            followed.bytesOfNewStorageLeftEmpty = static_cast<uint32_t>((capacity - after->outOfLineSize()) * sizeof(EncodedJSValue));
            Allocator allocatorOfStorage = vm.auxiliarySpace().allocatorFor(followed.bytesOfNewStorage, AllocatorForMode::EnsureAllocator);
            if (!allocatorOfStorage)
                return false;
            followed.allocatorOfNewStorage = allocatorOfStorage.localAllocator();
            followed.sizeOfNewStorage = followed.allocatorOfNewStorage->cellSize();
        }
    }
    copy.readsOrdinaryObject = !source.compiledObject;
    copy.excludedRunNumberPlusOne = excludedRunNumberPlusOne;
    copy.orderWordAfter = plan.orderWordAfterRun;
    copy.structureOfTarget = plan.target;
    copy.structureOfSource = plan.source;
    followed.targetWords = static_cast<uint64_t>(target.shapeWordWithMark()) << 32 | unorderedOrderWord;
    if (source.compiledObject) {
        copy.sourceWords = source.compiledObject->orderAndShapeWordsWithMark();
        copy.targetWords = followed.targetWords;
        copy.placeOfOwnedCopy = &placeOfCopyWithoutFrame(state, copy);
        copy.takesNoFrame = true;
    }
    copy.targetWords = UINT64_MAX;
    return true;
}

static ALWAYS_INLINE bool tryCopyFollowingPlan(Instance& instance, const MappedCopy& copy, JSCell* target, const JSCell* source)
{
    using Place = MappedCopy::Place;
    constexpr unsigned placeMask = MappedCopy::numberOfPlaces - 1;
    static_assert(hasOneBitSet(MappedCopy::numberOfPlaces));
    constexpr unsigned inlineProperties = static_cast<unsigned>(Place::InlineProperties);
    constexpr unsigned outsideProperties = static_cast<unsigned>(Place::OutsideProperties);
    if (static_cast<uint8_t>(target->cellState()) <= instance.barrierThreshold || target->structure()->mayBePrototype())
        return false;
    uint8_t* cellOfTarget = reinterpret_cast<uint8_t*>(target);
    const uint8_t* cellOfSource = reinterpret_cast<const uint8_t*>(source);
    const uint8_t* placesOfSource[MappedCopy::numberOfPlaces] { cellOfSource, nullptr, nullptr, nullptr };
    const JSObject* properties = copy.readsOrdinaryObject ? static_cast<const JSObject*>(source) : nullptr;
    const MappedCopy::FollowedPlan* plan = copy.followedPlan.get();
    if (plan && plan->readsSecondObjectOfSource) {
        const MappedCopy::FollowedPlan& followed = *plan;
        const uint8_t* word = cellOfSource + followed.offsetOfSecondObjectWordInSource;
        properties = followed.secondObjectWordOfSourceIsWide ? *reinterpret_cast<const JSObject* const*>(word) : static_cast<const JSObject*>(MarkedBlockCage::decode(instance.cellCageBase, *reinterpret_cast<const uint32_t*>(word)));
        if (properties->structureID() != followed.structureOfSecondObjectOfSource)
            return false;
    }
    if (properties) {
        placesOfSource[inlineProperties] = reinterpret_cast<const uint8_t*>(properties->inlineStorageUnsafe());
        placesOfSource[outsideProperties] = reinterpret_cast<const uint8_t*>(properties->butterfly()) - sizeof(IndexingHeader);
    }
    uint8_t* placesOfTarget[MappedCopy::numberOfPlaces] { cellOfTarget, nullptr, nullptr, nullptr };
    uint8_t* secondObject = nullptr;
    uint8_t* storage = nullptr;
    if (plan && plan->structureOfTargetAfter) {
        const MappedCopy::FollowedPlan& followed = *plan;
        auto allocate = [](LocalAllocator* allocator, uint32_t size) ALWAYS_INLINE_LAMBDA {
            FreeList& freeList = *reinterpret_cast<FreeList*>(reinterpret_cast<uint8_t*>(allocator) + LocalAllocator::offsetOfFreeList());
            return reinterpret_cast<uint8_t*>(freeList.allocateWithCellSize([]() -> HeapCell* {
                return nullptr;
            }, size));
        };
        if (followed.allocatorOfNewStorage) {
            storage = allocate(followed.allocatorOfNewStorage, followed.sizeOfNewStorage);
            if (!storage)
                return false;
        }
        secondObject = allocate(followed.allocatorOfNewSecondObject, followed.sizeOfNewSecondObject);
        if (!secondObject)
            return false;
        for (uint32_t offset = 0; offset < followed.bytesOfNewStorageLeftEmpty; offset += sizeof(EncodedJSValue)) {
            *reinterpret_cast<uint64_t*>(storage + offset) = 0;
            WTF::compilerFence();
        }
        static_assert(extraPropertiesInlineCapacity == 2 && JSObject::offsetOfInlineStorage() == 2 * sizeof(uint64_t) && JSObject::butterflyOffset() == sizeof(uint64_t));
        uint64_t* words = reinterpret_cast<uint64_t*>(secondObject);
        words[0] = followed.headerOfNewSecondObject;
        words[1] = storage ? std::bit_cast<uint64_t>(storage + followed.bytesOfNewStorage + sizeof(IndexingHeader)) : 0;
        words[2] = 0;
        words[3] = 0;
        placesOfTarget[inlineProperties] = secondObject + JSObject::offsetOfInlineStorage();
        placesOfTarget[outsideProperties] = storage + followed.bytesOfNewStorage;
    }
    // The collector may find the new cell on the stack.
    auto fail = [&](const MappedCopy::Move* end) ALWAYS_INLINE_LAMBDA {
        if (storage)
            zeroSpan(std::span { storage, plan->bytesOfNewStorage });
        return clearFieldsMovedInVain(copy, end, target);
    };
    uintptr_t cellCageBase = instance.cellCageBase;
    for (const MappedCopy::Move& move : copy.moves) {
        uint8_t* slotOfTarget = placesOfTarget[static_cast<unsigned>(move.placeInTarget) & placeMask] + static_cast<int32_t>(move.to);
        const uint8_t* slotOfSource = placesOfSource[static_cast<unsigned>(move.placeInSource) & placeMask] + static_cast<int32_t>(move.from);
        bool didMove = (move.placeInSource == Place::Cell || *reinterpret_cast<const uint64_t*>(slotOfSource)) && tryMove(cellCageBase, move, slotOfSource, [&]<typename T>(T bits) ALWAYS_INLINE_LAMBDA {
            *reinterpret_cast<T*>(slotOfTarget) = bits;
        }, [] {
            return false;
        });
        if (!didMove) [[unlikely]]
            return fail(&move);
    }
    if (!secondObject) {
        *reinterpret_cast<uint32_t*>(cellOfTarget + offsetOfOrderWord) = copy.orderWordAfter;
        return true;
    }
    const MappedCopy::FollowedPlan& followed = *plan;
    for (const MappedCopy::WordMove& move : followed.wordMoves) {
        uint64_t word = *reinterpret_cast<const uint64_t*>(placesOfSource[static_cast<unsigned>(move.placeInSource) & placeMask] + move.from);
        if (!word) [[unlikely]]
            return fail(copy.moves.end());
        *reinterpret_cast<uint64_t*>(placesOfTarget[static_cast<unsigned>(move.placeInTarget) & placeMask] + move.to) = word;
    }
    uint8_t* word = cellOfTarget + followed.offsetOfSecondObjectWordInTarget;
    if (followed.secondObjectWordOfTargetIsWide) [[unlikely]]
        *reinterpret_cast<uint8_t**>(word) = secondObject;
    else {
        RELEASE_ASSERT(std::bit_cast<uintptr_t>(secondObject) - cellCageBase < MarkedBlockCage::maxSize);
        *reinterpret_cast<uint32_t*>(word) = static_cast<uint32_t>(std::bit_cast<uintptr_t>(secondObject) >> MarkedBlockCage::shift);
    }
    *reinterpret_cast<uint32_t*>(cellOfTarget + offsetOfOrderWord) = copy.orderWordAfter;
    *reinterpret_cast<StructureID*>(cellOfTarget + JSCell::structureIDOffset()) = followed.structureOfTargetAfter;
    return true;
}

bool tryCopyFollowingPlanWithoutFrame(Instance& instance, JSCell* target, EncodedJSValue encodedSource, uint32_t excludedRunNumberPlusOne)
{
    JSValue sourceValue = JSValue::decode(encodedSource);
    if (!sourceValue.isCell())
        return false;
    const JSCell* source = sourceValue.asCell();
    uint64_t targetWords = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(target) + offsetOfOrderWord);
    uint64_t sourceWords = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(source) + offsetOfOrderWord);
    const MappedCopy* copy = instance.copiesWithoutFrame()[AllocationWords::placeOfCopyWithoutFrame(targetWords, sourceWords, excludedRunNumberPlusOne)];
    if (!copy || !copy->followedPlan || copy->followedPlan->targetWords != targetWords || copy->sourceWords != sourceWords || copy->excludedRunNumberPlusOne != excludedRunNumberPlusOne)
        return false;
    if (copy->structureOfTarget != target->structureID() || copy->structureOfSource != source->structureID())
        return false;
    return tryCopyFollowingPlan(instance, *copy, target, source);
}

static NEVER_INLINE void offerCopyWithoutFrame(VM& vm, State& state, AssignmentPlan& plan, const Object& target, const AssignmentSource& source, uint32_t excludedRunNumberPlusOne)
{
    static constexpr uint8_t maxNumberOfCopiesInVain = 8;
    if (!plan.copyWithoutFrame) {
        auto copy = makeUniqueWithoutFastMallocCheck<MappedCopy>();
        plan.mayCopyWithoutFrame = !source.compiledObject && !source.object->butterfly() && tryDescribeCopyOutOfOrdinaryObject(vm, state, plan, *copy, target, excludedRunNumberPlusOne);
        if (!plan.mayCopyWithoutFrame) {
            *copy = { };
            plan.mayCopyWithoutFrame = tryDescribeCopyFollowingPlan(vm, state, plan, *copy, target, source, excludedRunNumberPlusOne);
        }
        if (!plan.mayCopyWithoutFrame)
            return;
        copy->state = MappedCopy::State::IsServed;
        plan.copyWithoutFrame = WTF::move(copy);
    }
    MappedCopy& copy = *plan.copyWithoutFrame;
    if (!copy.placeOfOwnedCopy) {
        if (copy.numberOfCopiesInVain > maxNumberOfCopiesInVain) {
            plan.mayCopyWithoutFrame = false;
            plan.copyWithoutFrame = nullptr;
        }
        return;
    }
    const MappedCopy*& place = *copy.placeOfOwnedCopy;
    if (!place) {
        copy.numberOfCopiesInVain = 0;
        copy.numberOfTurnsLost = 0;
    } else if (place != &copy)
        ++copy.numberOfTurnsLost;
    if (copy.numberOfCopiesInVain > maxNumberOfCopiesInVain || copy.numberOfTurnsLost > maxNumberOfCopiesInVain) {
        if (copy.numberOfCopiesInVain <= maxNumberOfCopiesInVain && !copy.granuleBitsFilled) {
            copy.placeOfOwnedCopy = nullptr;
            return;
        }
        plan.mayCopyWithoutFrame = false;
        plan.copyWithoutFrame = nullptr;
        return;
    }
    place = &copy;
}

static NEVER_INLINE RepeatedCopy runPlanAsBeforeAndOfferCopy(VM& vm, State& state, Object& to, AssignmentPlan& plan, const AssignmentSource& source, uint32_t excludedRunNumberPlusOne)
{
    auto isForTarget = [&](const MappedCopy& copy) {
        return (copy.followedPlan ? copy.followedPlan->targetWords : copy.targetWords) == to.orderAndShapeWordsWithMark();
    };
    if (const MappedCopy* copy = plan.copyWithoutFrame.get(); copy && !copy->granuleBitsFilled && isForTarget(*copy) && plan.runsAsBeforeOn(to, source, excludedRunNumberPlusOne) && tryCopyFollowingPlan(state.instance, *copy, &to, source.object)) {
        if (copy->placeOfOwnedCopy && *copy->placeOfOwnedCopy != copy) [[unlikely]]
            offerCopyWithoutFrame(vm, state, plan, to, source, excludedRunNumberPlusOne);
        return RepeatedCopy::AsThePlanDidBefore;
    }
    static constexpr uint8_t maxNumberOfTimesAskedInVain = 4;
    RepeatedCopy way = tryRunPlanAsBefore(vm, state, to, plan, source, excludedRunNumberPlusOne);
    if (way != RepeatedCopy::IsNotDone) {
        plan.numberOfTimesAskedInVain = 0;
        offerCopyWithoutFrame(vm, state, plan, to, source, excludedRunNumberPlusOne);
    } else if (!plan.copyWithoutFrame && ++plan.numberOfTimesAskedInVain > maxNumberOfTimesAskedInVain)
        plan.mayCopyWithoutFrame = false;
    return way;
}

static NEVER_INLINE RepeatedCopy tryRepeatAssignmentFromOrdinaryObject(VM& vm, State& state, Object& to, JSValue source, uint32_t excludedRunNumberPlusOne)
{
    if (!source.isCell() || source.asCell()->type() != FinalObjectType)
        return RepeatedCopy::IsNotDone;
    JSObject* from = asObject(source.asCell());
    if (from->canHaveExistingOwnIndexedProperties())
        return RepeatedCopy::IsNotDone;
    AssignmentPlan* plan = state.objects->caches->assignmentPlans.recentPlan(to.structureID(), from->structureID(), from->structureID(), 0, excludedRunNumberPlusOne);
    return tryRunRecentPlanOrPlanMadeBefore(vm, state, to, plan, { from, nullptr, from, 0, { }, true }, excludedRunNumberPlusOne);
}

RepeatedCopy tryRepeatAssignment(VM& vm, State& state, JSObject* target, JSValue source, uint32_t excludedRunNumberPlusOne)
{
    ASSERT(isCompiledObject(target));
    Object* to = uncheckedDowncast<Object>(target);
    if (&to->state() != &state) [[unlikely]]
        return RepeatedCopy::IsNotDone;
    Object* from = compiledObjectOrNull(source);
    if (!from)
        return tryRepeatAssignmentFromOrdinaryObject(vm, state, *to, source, excludedRunNumberPlusOne);
    static_assert(!static_cast<uint8_t>(RepeatedCopy::IsNotDone) && static_cast<uint8_t>(RepeatedCopy::ByWordsOrMoves) == 1);
    if (!excludedRunNumberPlusOne && to->shapeWord() == from->shapeWord())
        return static_cast<RepeatedCopy>(to->tryCopyFieldsOf(vm, *from));
    return tryRepeatAssignmentBetweenLayouts(vm, state, *to, *from, excludedRunNumberPlusOne);
}

static constexpr size_t minNumberOfNamesPutTogether = 3;

static bool isPutAmongExtraProperties(VM& vm, State& state, Object& target, UniquedStringImpl* name)
{
    ASSERT(!parseIndex(PropertyName(name)));
    if (target.fieldNamed(state, name))
        return false;
    JSObject* extra = target.extraProperties();
    if (!extra)
        return true;
    unsigned attributes = 0;
    return !isValidOffset(extra->structure()->get(vm, name, attributes)) || !attributes;
}

enum class ExistingAttributes : bool { AreReset, AreKept };

template<ExistingAttributes existingAttributes>
static bool putPropertiesInOrder(VM& vm, Object& target, std::span<UniquedStringImpl*> names, std::span<const EncodedJSValue> values)
{
    ASSERT(names.size() == values.size() && !(target.orderWord() & orderWordIntegrityBits));
    State& state = target.state();
    for (size_t index = 0; index < names.size();) {
        size_t end = index;
        while (end < names.size() && isPutAmongExtraProperties(vm, state, target, names[end]))
            ++end;
        if (end - index < minNumberOfNamesPutTogether) {
            end = std::max(end, index + 1);
            for (; index < end; ++index) {
                JSValue value = JSValue::decode(values[index]);
                if (!(existingAttributes == ExistingAttributes::AreKept ? target.storeDirectFromNativeCode(vm, names[index], value) : target.putDirectFromNativeCode(vm, names[index], value, 0)))
                    return false;
            }
            continue;
        }
        JSObject* extra = target.ensureExtraProperties(vm);
        RELEASE_ASSERT(extra);
        for (UniquedStringImpl* name : names.subspan(index, end - index)) {
            if (!extra->getDirect(vm, name))
                target.willAddExtraProperty(vm, name);
        }
        extra->putOwnDataPropertyBatching(vm, names.subspan(index).data(), values.subspan(index).data(), static_cast<unsigned>(end - index));
        target.didChangeExtraProperties(vm);
        didChangeOwnProperties(vm, target, VM::StructureChainIntegrityEvent::Change, nullptr);
        index = end;
    }
    return true;
}

bool definePropertiesInOrder(VM& vm, Object& target, std::span<UniquedStringImpl*> names, std::span<const EncodedJSValue> values)
{
    return putPropertiesInOrder<ExistingAttributes::AreReset>(vm, target, names, values);
}

bool storePropertiesInOrder(VM& vm, Object& target, std::span<UniquedStringImpl*> names, std::span<const EncodedJSValue> values)
{
    return putPropertiesInOrder<ExistingAttributes::AreKept>(vm, target, names, values);
}

static bool tryAssignOrDefinePropertiesOf(VM& vm, Object& target, JSObject* source, std::span<UniquedStringImpl* const> excludedNames, bool defines)
{
    if (target.orderWord() & orderWordIntegrityBits || source == &target)
        return false;
    std::optional<AssignmentSource> assignmentSource = assignmentSourceFor(source, excludedNames, defines);
    if (!assignmentSource)
        return false;
    DeferGC deferGC(vm);
    JSObject* extra = target.extraProperties();
    State& state = target.state();
    AssignmentPlanCache& plans = state.objects->caches->assignmentPlans;
    AssignmentPlan* plan = extra || !excludedNames.empty() ? nullptr : plans.recentPlan(target.structureID(), source->structureID(), assignmentSource->propertyStorageStructureID(), assignmentSource->orderWord, 0);
    if (!plan || !plan->isFor(target.structureID(), StructureID(), *assignmentSource))
        plan = plans.planFor(vm, &target, extra, *assignmentSource);
    if (!defines && plan->isUsable && plan->storesNameAfterSymbol) {
        if (!PropertyWalk::wouldPerformFastPut(vm, &target))
            return false;
        plan = plans.planFor(vm, &target, extra, *assignmentSource);
    }
    if (!plan->isUsable)
        return false;
    assignByPlan(vm, target, state, *plan, extra, *assignmentSource);
    return true;
}

bool tryAssignOrdinaryObjectAsBefore(VM& vm, State& state, Object& target, JSObject* source)
{
    if (source->type() != FinalObjectType || source->canHaveExistingOwnIndexedProperties())
        return false;
    AssignmentPlan* plan = state.objects->caches->assignmentPlans.recentPlan(target.structureID(), source->structureID(), source->structureID(), 0, 0);
    if (!plan)
        return false;
    StructureID ofSource = source->structureID();
    bool isThePlan = plan->target == target.structureID() && plan->source == ofSource && !plan->sourceOrderWord && plan->sourcePropertyStorage == ofSource && !plan->extraPropertiesBefore
        && plan->isUsable && !plan->servesDefinitionsOnly && !plan->storesNameAfterSymbol && !plan->excludedList && plan->excludedNames.isEmpty() && plan->orderWordBeforeRun == target.orderWord();
    if (!isThePlan)
        return false;
    DeferGC deferGC(vm);
    if (plan->fieldStores.isEmpty() && !plan->extraPropertyStores.isEmpty()) {
        if (!storeNewExtraPropertiesFromOrdinaryObject(vm, target, *plan, source))
            return false;
        target.at<uint32_t>(offsetOfOrderWord) = plan->orderWordAfterRun;
    } else {
        AssignmentSource assignmentSource { source, nullptr, source, 0, { }, false };
        FieldStoreRun run(vm, target, state, *plan, nullptr);
        if (!run.tryRunAsBefore(assignmentSource))
            return false;
    }
    if (target.structure()->mayBePrototype()) [[unlikely]]
        didChangeOwnProperties(vm, target, VM::StructureChainIntegrityEvent::Add, nullptr);
    return true;
}

template<bool defines>
static ALWAYS_INLINE bool tryCopyFieldsLeavingElementsOut(VM& vm, Object& target, Object& source)
{
    uint32_t orderWord = source.orderWord();
    if (target.orderWord() & orderWordIntegrityBits || &source == &target || isOrderStoredOutOfLine(orderWord) || &source.state() != &target.state())
        return false;
    AssignmentSource assignmentSource { &source, &source, nullptr, orderWord, { }, defines, source.extension(), &source.instance() };
    DeferGC deferGC(vm);
    State& state = target.state();
    JSObject* extra = target.extraProperties();
    AssignmentPlan* plan = state.objects->caches->assignmentPlans.planFor(vm, &target, extra, assignmentSource);
    if (!plan->isUsable || (!defines && plan->storesNameAfterSymbol))
        return false;
    plan->leavesElementsOut = true;
    assignByPlan(vm, target, state, *plan, extra, assignmentSource);
    return true;
}

bool tryDefineFieldsLeavingElementsOut(VM& vm, Object& target, Object& source)
{
    return tryCopyFieldsLeavingElementsOut<true>(vm, target, source);
}

bool tryAssignFieldsLeavingElementsOut(VM& vm, Object& target, Object& source)
{
    return tryCopyFieldsLeavingElementsOut<false>(vm, target, source);
}

bool tryAssignPropertiesOf(VM& vm, Object& target, JSObject* source)
{
    return tryAssignOrDefinePropertiesOf(vm, target, source, { }, false);
}

bool tryDefinePropertiesOf(VM& vm, Object& target, JSObject* source, std::span<UniquedStringImpl* const> excludedNames)
{
    return tryAssignOrDefinePropertiesOf(vm, target, source, excludedNames, true);
}

static bool hasPlainPropertyStorage(Structure* target)
{
    return !target->isCompiledStructure()
        && !target->mayHaveImmutableProperties()
        && !target->mayBePrototype()
        && !target->isWatchingReplacement()
        && !target->isDictionary()
        && target->isStructureExtensible()
        && !target->hasPolyProto();
}

bool takesPlainPropertiesDirectly(Structure* target)
{
    return target->typeInfo().type() == FinalObjectType && hasPlainPropertyStorage(target);
}

static bool takesPlainPropertiesDirectly(const Closure& closure)
{
    FunctionKindData kind = closure.kind();
    Structure* structure = closure.structure();
    return !kind.isConstructor && !kind.isInRuntimeModule && !kind.closuresGetOnlyListedNames
        && hasPlainPropertyStorage(structure) && !structure->hasFrozenElementsInPlace() && !structure->hasNonReifiedStaticProperties();
}

static bool tryCopyByPlan(VM& vm, State& state, JSObject* target, JSObject* source, std::span<UniquedStringImpl* const> excludedNames, bool defines)
{
    ASSERT(!isCompiledObject(target));
    if (source == target || hasIndexedProperties(target->indexingType()))
        return false;
    std::optional<AssignmentSource> assignmentSource = assignmentSourceFor(source, excludedNames, defines);
    if (!assignmentSource)
        return false;
    if (assignmentSource->compiledObject && &assignmentSource->compiledObject->state() != &state)
        return false;
    DeferGC deferGC(vm);
    AssignmentPlan* plan = state.objects->caches->assignmentPlans.planFromSecondCopyOn(vm, target, nullptr, *assignmentSource);
    if (!plan || !plan->isUsable || (!defines && plan->storesNameAfterSymbol))
        return false;
    Vector<EncodedJSValue, 16> values;
    for (auto& store : plan->extraPropertyStores) {
        JSValue value = assignmentSource->read(store);
        if (!value)
            return false;
        values.append(JSValue::encode(value));
    }
    Structure* before = target->structure();
    Structure* after = plan->extraPropertiesAfter;
    if (before->outOfLineCapacity() != after->outOfLineCapacity())
        target->nukeStructureAndSetButterfly(vm, before->id(), target->allocateMoreOutOfLineStorage(vm, before->outOfLineCapacity(), after->outOfLineCapacity()));
    for (size_t i = 0; i < values.size(); ++i)
        target->locationForOffset(plan->extraPropertyStores[i].offset)->setWithoutWriteBarrier(JSValue::decode(values[i]));
    vm.writeBarrier(target);
    if (before != after || before->outOfLineCapacity() != after->outOfLineCapacity())
        target->setStructure(vm, after);
    return true;
}

bool tryDefinePropertiesOnOrdinaryObject(VM& vm, State& state, JSObject* target, JSObject* source, std::span<UniquedStringImpl* const> excludedNames)
{
    ASSERT(target->type() == FinalObjectType);
    return tryCopyByPlan(vm, state, target, source, excludedNames, true);
}

bool tryAssignToOrdinaryObject(VM& vm, State& state, JSObject* target, JSObject* source)
{
    bool takesProperties = target->type() == CompiledClosureType ? takesPlainPropertiesDirectly(*uncheckedDowncast<Closure>(target)) : takesPlainPropertiesDirectly(target->structure());
    return takesProperties && tryCopyByPlan(vm, state, target, source, { }, false);
}

JSFinalObject* tryCopyTwoObjects(VM& vm, State& state, JSGlobalObject* realm, JSValue first, JSValue second)
{
    if (compiledObjectOrNull(first))
        return nullptr;
    std::array<std::optional<AssignmentSource>, 2> sources;
    std::array<JSValue, 2> values { first, second };
    for (size_t index = 0; index < values.size(); ++index) {
        JSValue value = values[index];
        if (!value.isCell())
            continue;
        if (!value.isObject())
            return nullptr;
        sources[index] = assignmentSourceFor(asObject(value), { }, true);
        if (!sources[index] || (sources[index]->compiledObject && &sources[index]->compiledObject->state() != &state))
            return nullptr;
    }
    auto sourceOrNull = [&](size_t index) {
        return sources[index] ? &*sources[index] : nullptr;
    };
    DeferGC deferGC(vm);
    CopyOfTwoPlan* plan = state.objects->caches->assignmentPlans.planForCopyOfTwo(vm, realm, sourceOrNull(0), sourceOrNull(1));
    if (!plan || !plan->structure)
        return nullptr;
    if (plan->sources[1].fields.sourceHasDeferredClosures)
        const_cast<Object*>(sources[1]->compiledObject)->materializeDeferredClosures(vm);
    JSFinalObject* copy = JSFinalObject::create(vm, plan->structure);
    for (size_t index = 0; index < values.size(); ++index) {
        const CopyOfTwoPlan::Source& moved = plan->sources[index];
        if (!moved.fields.movesNothing() && !applyMovesToNewObject(vm, moved.fields, copy, sources[index]->object))
            return nullptr;
        if (!moved.properties.movesNothing() && !applyMovesToNewObject(vm, moved.properties, copy, sources[index]->propertyStorage))
            return nullptr;
    }
    return copy;
}

static NEVER_INLINE bool storePropertiesOfPlanOneByOne(VM& vm, Object& target, State& state, const AssignmentPlan& plan, const AssignmentSource& source)
{
    struct Property {
        const FieldData* field;
        UniquedStringImpl* name;
        JSValue value;
    };
    Vector<Property, 16> properties;
    for (uint16_t number : plan.storesInSourceOrder) {
        size_t index = number & ~AssignmentPlan::isExtraPropertyStore;
        if (number & AssignmentPlan::isExtraPropertyStore)
            properties.append({ nullptr, plan.extraPropertyStores[index].name, source.read(plan.extraPropertyStores[index]) });
        else
            properties.append({ plan.fieldStores[index].to, nullptr, source.read(plan.fieldStores[index]) });
    }
    Vector<UniquedStringImpl*, 16> namesToDefine;
    Vector<EncodedJSValue, 16> valuesToDefine;
    auto defineWhatWaits = [&] {
        if (namesToDefine.isEmpty())
            return;
        bool isDefined = definePropertiesInOrder(vm, target, namesToDefine.mutableSpan(), valuesToDefine.span());
        RELEASE_ASSERT(isDefined);
        namesToDefine.shrink(0);
        valuesToDefine.shrink(0);
    };
    Vector<const Property*, 4> valuesThatLeaveTheirSlots;
    for (const Property& property : properties) {
        if (!property.value)
            continue;
        if (!property.field) {
            namesToDefine.append(property.name);
            valuesToDefine.append(JSValue::encode(property.value));
            continue;
        }
        defineWhatWaits();
        const FieldData& field = *property.field;
        if (target.readInline(field) ? target.tryStore(vm, field, property.value) : tryCreateField(vm, state, target, field, property.value))
            continue;
        if (!target.isInKeyOrder(state, field))
            target.didCreateField(state, field, false);
        valuesThatLeaveTheirSlots.append(&property);
    }
    defineWhatWaits();
    for (const Property* property : valuesThatLeaveTheirSlots)
        target.moveFieldToExtraProperties(vm, *property->field, { property->value, 0 }, SlotExitReason::StoreOfOtherType);
    return valuesThatLeaveTheirSlots.isEmpty();
}

static ALWAYS_INLINE void assignByPlan(VM& vm, Object& target, State& state, AssignmentPlan& plan, JSObject* extra, const AssignmentSource& source)
{
    AssignmentPlanCache& plans = state.objects->caches->assignmentPlans;
    uint32_t orderWordBefore = target.orderWord();
    if (plan.orderWordBeforeFillingAll == orderWordBefore && tryFillEmptySlotsAsBefore(vm, target, state, plan, *source.compiledObject)) {
        plans.didRunAsBefore(plan);
        didChangeOwnProperties(vm, target, VM::StructureChainIntegrityEvent::Add, nullptr);
        return;
    }
    FieldStoreRun run(vm, target, state, plan, extra);
    if (plan.orderWordBeforeRun == orderWordBefore) {
        if (run.tryRunAsBefore(source)) [[likely]] {
            plans.didRunAsBefore(plan);
            didChangeOwnProperties(vm, target, VM::StructureChainIntegrityEvent::Add, nullptr);
            return;
        }
        plan.orderWordBeforeRun = std::nullopt;
    }
    static constexpr uint8_t maxNumberOfRunsInVain = 2;
    if (plan.numberOfRunsInVain > maxNumberOfRunsInVain || !run.tryRunListingOnce(source)) {
        uint8_t numberOfRunsInVain = plan.numberOfRunsInVain;
        bool everyValueKeptItsSlot = storePropertiesOfPlanOneByOne(vm, target, state, plan, source);
        plan.numberOfRunsInVain = everyValueKeptItsSlot ? 0 : static_cast<uint8_t>(std::min(numberOfRunsInVain, maxNumberOfRunsInVain) + 1);
        return;
    }
    if (plan.storesOnlyFieldsThatFit && !plan.fieldStores.isEmpty() && run.emptySlotsFilled() == plan.fieldStores.size() && !target.hasOutOfLineKeyOrder()) {
        plan.orderWordBeforeFillingAll = orderWordBefore;
        plan.orderWordAfterFillingAll = target.orderWord();
    } else if (!target.hasOutOfLineKeyOrder()) {
        plan.orderWordBeforeRun = orderWordBefore;
        plan.orderWordAfterRun = target.orderWord();
    }
    didChangeOwnProperties(vm, target, VM::StructureChainIntegrityEvent::Add, nullptr);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
