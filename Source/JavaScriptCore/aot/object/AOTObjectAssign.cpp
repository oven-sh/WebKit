/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTAssignmentPlans.h"
#include "AOTKeyOrderTable.h"
#include "AOTMappedCopyInlines.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTObjectModelCaches.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTPropertyWalk.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeValidation.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "DeferGC.h"
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
    // What lies behind the granules is zero, in the room that extensionCapacityFor() leaves. Nothing reads behind that room.
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
    if (layout.hasExtension())
        setExtension(vm, extension);
    setOrderAndCount(source.orderWord() & orderAndCountMask);
    vm.writeBarrier(this);
}

enum class CopyOfWords : bool { FillsObject, MakesObject };
static bool tryMakeClosuresForCopyOfWords(VM& vm, Object& source, CopyOfWords copy)
{
    const uint16_t flagsOfOtherFields = fieldIsPrivate | fieldIsBrand | fieldAttributeFlags | (copy == CopyOfWords::FillsObject ? fieldIsStoredOnce : 0);
    for (const FieldData& field : source.layout().fields()) {
        if (field.flags & flagsOfOtherFields)
            return false;
        if (field.slot.representation() == SlotRepresentation::DeferredClosure && source.at<uint32_t>(field.slot.offset) == uncreatedDeferredClosure)
            source.materializeDeferredClosure(vm, field);
    }
    return true;
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

// With fewer names a Structure without a table of its own serves the object and all its copies, once the object is flattened.
static bool isCopiedAsDictionary(Structure* structure)
{
    if (structure->typeInfo().type() != FinalObjectType || !structure->isDictionary() || structure->inlineSize() + structure->outOfLineSize() <= static_cast<unsigned>(Structure::s_maxTransitionLength))
        return false;
    if (!structure->canPerformFastPropertyEnumeration() || structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || structure->hasNonEnumerableProperties() || structure->hasNonConfigurableProperties())
        return false;
    // A branded Structure would hand the copy the private brand of the source.
    if (structure->variant() != Structure::StructureVariant::Normal)
        return false;
    return !structure->mayHaveImmutableProperties() && !structure->mayBePrototype() && !structure->didPreventExtensions() && !structure->hasPolyProto();
}

NEVER_INLINE bool flattenOnceUnlessCopiedAsDictionary(VM& vm, JSObject* source)
{
    return !isCopiedAsDictionary(source->structure()) && flattenOnce(vm, source);
}

// A dictionary has a Structure of its own. Its copy gets a copy of that Structure with its table, so no name is looked up.
static NEVER_INLINE Structure* tryCopyStructureOfDictionary(VM& vm, Structure* structure)
{
    return isCopiedAsDictionary(structure) ? Structure::toCacheableDictionaryTransition(vm, structure) : nullptr;
}

static JSFinalObject* tryCopyPlainExtraProperties(VM& vm, JSObject* extra)
{
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

// No two dictionaries have one Structure, so nothing is gained by naming it.
static ALWAYS_INLINE Structure* structureToName(JSObject* extraProperties)
{
    Structure* structure = extraProperties->structure();
    return structure->isDictionary() ? nullptr : structure;
}

ALWAYS_INLINE Object* Object::createCopyOfWords(VM& vm, const Object& source, Structure* structure)
{
    const LayoutData& layout = source.layout();
    Object* copy = new (NotNull, allocateCell<Object>(vm, layout.size)) Object(vm, structure);
    if (!layout.hasExtension()) {
        // Nothing is allocated before the last word is written, so nobody visits the cell while it holds what the cell before it held.
        copy->setPerCellBit(source.perCellBit());
        copy->at<uint32_t>(offsetOfOrderWord) = source.orderWord() & orderAndCountMask;
        copy->at<uint32_t>(offsetOfShapeWord) = shapeWordOf(layout.shape);
        gcSafeMemcpy(&copy->at<uint64_t>(offsetOfFirstField), &source.at<uint64_t>(offsetOfFirstField), sizeOfFieldWords(layout));
        copy->JSCell::finishCreation(vm);
        vm.writeBarrier(copy);
        return copy;
    }
    zeroSpan(std::span { reinterpret_cast<uint8_t*>(copy) + sizeof(JSCell), layout.size - sizeof(JSCell) });
    copy->setPerCellBit(source.perCellBit());
    copy->at<uint32_t>(offsetOfOrderWord) = unorderedOrderWord;
    copy->at<uint32_t>(offsetOfShapeWord) = shapeWordOf(layout.shape);
    copy->JSCell::finishCreation(vm);
    copy->copyFieldsOfSameLayout(vm, source);
    return copy;
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
    if (!structure.isCopiedByWords() && !tryMakeClosuresForCopyOfWords(vm, const_cast<Object&>(source), CopyOfWords::MakesObject))
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
    // The stub in front copies a source whose Structure names that of its extra properties. A source that is copied once is likely to be copied again.
    if (!structure.extraPropertiesStructureID() && named->extraPropertiesStructureID() == source.extraProperties()->structureID())
        const_cast<Object&>(source).JSCell::setStructure(vm, named);
    return copy;
}

Object* Object::tryCreateCopyOfFields(VM& vm, const Object& source)
{
    const CompiledStructure& structure = source.compiledStructure();
    const LayoutData& layout = source.layout();
    if (layout.kind() != LayoutKind::Record || layout.keysAreNeverEnumerated || source.hasKeyOrderOfItsOwn() || structure.mayBePrototype())
        return nullptr;
    if (source.hasFlagsInOrderWord() || !structure.flags().contains(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype)) [[unlikely]]
        return tryCreateCopyOfFieldsAndExtraProperties(vm, source);
    DeferGC deferGC(vm);
    if (!structure.isCopiedByWords() && !tryMakeClosuresForCopyOfWords(vm, const_cast<Object&>(source), CopyOfWords::MakesObject))
        return nullptr;
    return createCopyOfWords(vm, source, source.structure());
}

static void validateCopyOfExtraProperties(VM& vm, const Object& target, JSObject* extraProperties)
{
    if (!checksClaimsAboutObjects(vm))
        return;
    RuntimeLayoutTable& layouts = target.state().objects->layouts;
    bool addsName = false;
    extraProperties->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        addsName |= !layouts.field(vm, target.layout(), entry.key());
        return true;
    });
    if (addsName)
        validateMutation(vm, target, ObjectMutation::AddProperty);
}

// A Structure that holds a key order and belongs to no single object serves every object with that order.
static ALWAYS_INLINE bool holdsKeyOrderFor(const CompiledStructure& ordered, const CompiledStructure& unordered)
{
    return ordered.hasOutOfLineKeyOrder() && !ordered.hasKeyOrderOfOneObject() && !ordered.hasExtraProperties() && !ordered.mayBePrototype()
        && ordered.hasOtherPrototype() == unordered.hasOtherPrototype() && ordered.storedPrototype() == unordered.storedPrototype() && ordered.realm() == unordered.realm();
}

ALWAYS_INLINE bool Object::tryCopyWordsOfSameLayout(VM& vm, const Object& source)
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
    if (layout.kind() != LayoutKind::Record || layout.keysAreNeverEnumerated)
        return false;
    bool takesStructure = structureID() != source.structureID();
    if (takesStructure && !holdsKeyOrderFor(source.compiledStructure(), structure))
        return false;
    ASSERT(takesStructure == isOrderStoredOutOfLine(orderWordOfSource));
    // The only allocation comes first, while the target is still empty.
    std::optional<SlotData> extensionSlot = structure.extensionSlot();
    Extension* extension = extensionSlot ? createCopyOf(vm, reinterpret_cast<Extension*>(const_cast<uint8_t*>(extensionStorageOrNull(&source, *extensionSlot)))) : nullptr;
    gcSafeMemcpy(&at<uint64_t>(offsetOfFirstField), &source.at<uint64_t>(offsetOfFirstField), sizeOfFieldWords(layout));
    if (extension) {
        if (extensionSlot->representation() == SlotRepresentation::Cell)
            at<Extension*>(extensionSlot->offset) = extension;
        else
            at<uint32_t>(extensionSlot->offset) = MarkedBlockCage::encode(cageBaseOf(this), reinterpret_cast<uint8_t*>(extension) - offsetOfEmptyExtensionInCage);
    }
    if (takesStructure) {
        // Both describe cells with the same header.
        ASSERT(source.structure()->typeInfoBlob() == structure.typeInfoBlob());
        setStructureIDDirectly(source.structureID());
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
    if (hasOutOfLineKeyOrder() || source.hasOutOfLineKeyOrder() || source.layout().keysAreNeverEnumerated)
        return false;
    DeferGC deferGC(vm);
    bool mayCopyWords = shapeWord() == source.shapeWord() && !countOf(orderWord());
    JSObject* extraProperties = nullptr;
    if (source.hasExtraProperties()) [[unlikely]] {
        if (!mayCopyWords || source.orderWord() & orderWordIntegrityBits || !layout().extraPropertiesSlot() || isInFrame())
            return false;
        extraProperties = tryCopyPlainExtraProperties(vm, source.extraProperties());
        if (!extraProperties)
            return false;
    }
    if (mayCopyWords && !compiledStructure().hasOnlyPlainSlots()) [[unlikely]]
        mayCopyWords = tryMakeClosuresForCopyOfWords(vm, const_cast<Object&>(source), CopyOfWords::FillsObject);
    if (extraProperties && !mayCopyWords)
        return false;
    if (mayCopyWords) {
        CompiledStructure* structureWithExtraProperties = nullptr;
        if (extraProperties) {
            validateCopyOfExtraProperties(vm, *this, extraProperties);
            structureWithExtraProperties = state().objects->structures.withExtraProperties(vm, compiledStructure(), structureToName(extraProperties));
        }
        copyFieldsOfSameLayout(vm, source);
        if (extraProperties) {
            setExtraProperties(vm, extraProperties, structureWithExtraProperties);
            at<uint32_t>(offsetOfOrderWord) |= source.orderWord() & orderWordHasFieldOutsideSlot;
            if (source.perCellBit())
                setPerCellBit(true);
        }
        if (countOf(orderWord()) || extraProperties)
            didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Add, nullptr);
        return true;
    }
    if (!countOf(source.orderWord()))
        return true;
    return tryRepeatAssignmentBetweenLayouts(vm, state(), *this, source, 0) != RepeatedCopy::IsNotDone;
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
    Vector<AcceptedValue, 16> acceptedValues;
    for (size_t i = 0; i < names.size(); ++i) {
        const FieldData* field = layouts.field(vm, layout, names[i]);
        if (!field || field->flags & (fieldIsStoredOnce | fieldAttributeFlags) || field->slot.representation() == SlotRepresentation::DeferredClosure)
            return false;
        std::optional<AcceptedValue> accepted = acceptInSlot(*this, *field, JSValue::decode(values[i]));
        if (!accepted)
            return false;
        fields.append(field);
        acceptedValues.append(*accepted);
    }

    uint64_t granuleBits = 0;
    for (const FieldData* field : fields) {
        if (field->isOutOfLine())
            granuleBits |= granuleBitOf(field->slot.offset);
    }
    if (granuleBits)
        ensureExtensionGranules(vm, granuleBits);

    if (layout.keysAreNeverEnumerated) {
        bool didCreateFields = false;
        for (size_t i = 0; i < fields.size(); ++i) {
            didCreateFields |= !isInKeyOrder(state, *fields[i]);
            bool didStore = store(vm, *fields[i], acceptedValues[i]);
            RELEASE_ASSERT(didStore);
        }
        if (didCreateFields)
            didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Add, nullptr);
        return true;
    }

    KeyOrderStorage storage;
    Vector<uint16_t, 32> keys(keyOrder(state, storage));
    size_t numberOfKeysBefore = keys.size();
    BitVector isListed;
    for (uint16_t key : keys)
        isListed.set(key);
    for (size_t i = 0; i < fields.size(); ++i) {
        bool didStore = store(vm, *fields[i], acceptedValues[i]);
        RELEASE_ASSERT(didStore);
        uint16_t index = layout.indexOf(*fields[i]);
        if (!isListed.set(index))
            keys.append(index);
    }
    if (keys.size() == numberOfKeysBefore)
        return true;
    setKeyOrder(vm, state, keys.span());
    didChangeOwnProperties(vm, *this, VM::StructureChainIntegrityEvent::Add, nullptr);
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
    case CellTestKind::IsPristine:
    case CellTestKind::IsBuiltinCell:
    case CellTestKind::IsClosureOf:
    case CellTestKind::IsSymbol:
    case CellTestKind::SharingModeIs:
    case CellTestKind::IsNamespaceOf:
    case CellTestKind::CarriesBrandOfClass:
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

static ALWAYS_INLINE void storeSlotBits(unsigned width, void* slot, uint64_t bits)
{
    switch (width) {
    case 1:
        *static_cast<uint8_t*>(slot) = static_cast<uint8_t>(bits);
        return;
    case 2:
        *static_cast<uint16_t*>(slot) = static_cast<uint16_t>(bits);
        return;
    case 4:
        *static_cast<uint32_t*>(slot) = static_cast<uint32_t>(bits);
        return;
    case 8:
        *static_cast<uint64_t*>(slot) = bits;
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE void* slotIn(const Object& object, Extension* extensionOrNull, const FieldData& field)
{
    if (!field.isOutOfLine()) [[likely]]
        return &object.at<uint8_t>(field.slot.offset);
    return extensionOrNull ? extensionOrNull->slotOrNull(field.slot.offset) : nullptr;
}

// Every field's value is read and put into the form of its slot before anything of the target changes. False where a value is missing or does not go to its slot.
template<FieldStoreRun::CountEmptySlots countEmptySlots>
ALWAYS_INLINE bool FieldStoreRun::tryStoreEveryProperty(const AssignmentSource& source)
{
    // Extra properties that the run creates are seen by nobody before they are complete.
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
        JSValue value = source.read(store);
        if (store.writesInt32AsItIs && value.isInt32() && value.asInt32() >= store.leastInt32 && value.asInt32() <= store.greatestInt32) {
            *bits++ = static_cast<uint64_t>(static_cast<int64_t>(value.asInt32()));
            continue;
        }
        if (store.leavesUndefinedAndNullToOtherWay && value && value.isUndefinedOrNull())
            return false;
        if (value && !isAccepted(store, value)) {
            // A kind of which the field takes some values: numbers in a range, the strings of a list.
            std::optional<AcceptedValue> accepted = m_state.objects->layouts.accept(*store.to, value);
            value = accepted ? accepted->value() : JSValue();
        }
        *bits = 0;
        if (!value || !tryWriteSlot(*m_state.strings, store.to->slot.representation(), bits++, value))
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
        storeSlotBits(store.width, to, *bits++);
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

bool FieldStoreRun::tryRunListingOnce(const AssignmentSource& source)
{
    const LayoutData& layout = m_target.layout();
    uint32_t orderWord = m_target.orderWord();
    if (isOrderStoredOutOfLine(orderWord) || orderWord & (orderWordHasFieldOutsideSlot | orderWordIntegrityBits))
        return false;
    if (!std::ranges::all_of(m_plan.fieldStores, &AssignmentPlan::FieldStore::isDirect))
        return false;
    Object::KeyOrderStorage storage;
    Vector<uint16_t, 32> keys;
    if (!layout.keysAreNeverEnumerated)
        keys.append(m_target.keyOrder(m_state, storage));
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
    for (uint16_t number : layout.keysAreNeverEnumerated ? std::span<const uint16_t>() : m_plan.storesInSourceOrder.span()) {
        size_t index = number & ~AssignmentPlan::isExtraPropertyStore;
        if (number & AssignmentPlan::isExtraPropertyStore) {
            const AssignmentPlan::ExtraPropertyStore& store = m_plan.extraPropertyStores[index];
            numberOfExtraKeysAdded += store.addsName && !PropertyName(store.name).isPrivateName();
            continue;
        }
        uint16_t key = layout.indexOf(*m_plan.fieldStores[index].to);
        if (isListed.set(key))
            continue;
        if (!numberOfExtraKeysBefore) {
            numberOfExtraKeysBefore = m_target.numberOfExtraKeysByStructure();
            if (!numberOfExtraKeysBefore)
                numberOfExtraKeysBefore = m_target.extraKeyNames().size();
        }
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

// Fast path: every target slot is still empty, so copy each field and set the order word that the first run produced.
static bool tryFillEmptySlotsAsBefore(VM& vm, Object& target, State& state, const AssignmentPlan& plan, const Object& source)
{
    ASSERT(plan.storesOnlyFieldsThatFit && plan.orderWordBeforeFillingAll == target.orderWord() && plan.readsFrom(source.structureID(), source.orderWord(), source.extraProperties()));
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
            storeSlotBits(store.width, to, bits);
        } else {
            JSValue value = source.readInline(*store.from);
            if (!value || !tryWriteSlot(*state.strings, store.to->slot.representation(), to, value))
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
    // A copy only reads its source, so a frozen or sealed source can have a plan too. The order word is part of the plan's key.
    if (isOrderStoredOutOfLine(orderWord) || compiledSource->layout().keysAreNeverEnumerated)
        return std::nullopt;
    return AssignmentSource { source, compiledSource, compiledSource->extraProperties(), orderWord, excludedNames, defines, compiledSource->extension(), compiledSource->state().strings.get() };
}

static void assignByPlan(VM&, Object& target, State&, AssignmentPlan&, JSObject* extra, const AssignmentSource&);

static ALWAYS_INLINE uint64_t granuleBitsOf(const Extension* extensionOrNull) { return extensionOrNull ? extensionOrNull->granuleBits() : 0; }

static uint32_t offsetInExtensionWith(uint64_t granuleBits, uint32_t offset)
{
    ASSERT(granuleBits & granuleBitOf(offset));
    return offsetOfFirstExtensionSlot + std::popcount(granuleBits & (granuleBitOf(offset) - 1)) * extensionGranuleSize + offsetInGranule(offset);
}

// An integer keeps its value from one form to another. Each of these kinds takes the other way for a marker.
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

// A slot for literal strings holds the word of one or a reference to any other string. Both kinds take the other way for a marker.
static std::optional<MappedCopy::MoveKind> moveKindOfLiteralString(SlotRepresentation from, SlotRepresentation to)
{
    if (from == SlotRepresentation::LiteralString32 && (to == SlotRepresentation::Cell32 || to == SlotRepresentation::OptionalCell32))
        return MappedCopy::MoveKind::NarrowLiteralString;
    if (from == SlotRepresentation::LiteralString && (to == SlotRepresentation::JSValue || to == SlotRepresentation::Cell))
        return MappedCopy::MoveKind::WideLiteralString;
    return std::nullopt;
}

static MappedCopy::MoveKind moveKindUnlessMarker(SlotRepresentation from, SlotRepresentation to)
{
    using Kind = MappedCopy::MoveKind;
    if (auto kind = moveKindOfLiteralString(from, to))
        return *kind;
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
        if (auto kind = moveKindOfLiteralString(from, to))
            return *kind;
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
    switch (from) {
    case SlotRepresentation::OptionalInt32:
        return Kind::OptionalInt32;
    case SlotRepresentation::OptionalInt64:
        return Kind::OptionalInt64;
    case SlotRepresentation::OptionalDouble:
        return Kind::OptionalDouble;
    case SlotRepresentation::JSValue:
    case SlotRepresentation::Cell:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
    case SlotRepresentation::Int32:
    case SlotRepresentation::Uint32:
    case SlotRepresentation::Int64:
    case SlotRepresentation::Double:
    case SlotRepresentation::Float:
    case SlotRepresentation::Boolean:
    case SlotRepresentation::OptionalBoolean:
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
    case SlotRepresentation::DeferredClosure:
        break;
    }
    const SlotTraits traits = traitsOf(from);
    RELEASE_ASSERT(!traits.absentBits);
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
    // All but the two booleans: the stub counts in 32 bits.
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
    case Kind::NarrowLiteralString:
    case Kind::WideLiteralString:
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
    AOT::stableSort(copy.moves.mutableSpan(), { }, &MappedCopy::Move::runOfStub);
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

static bool takesNoFrame(VM& vm, State& state, MappedCopy& copy, const Object& target, const Object& source)
{
    if (state.objects->caches->assignmentPlans.countsCopies() || validatesRuntime(vm))
        return false;
    for (const MappedCopy::Move& move : copy.moves) {
        if (move.kind == MappedCopy::MoveKind::OtherForm || move.kind == MappedCopy::MoveKind::Tested)
            return false;
    }
    if (copy.granuleBitsOfSource) {
        SlotData word = *source.compiledStructure().extensionSlot();
        copy.offsetOfExtensionWordInSource = word.offset;
        copy.extensionWordOfSourceIsWide = word.representation() == SlotRepresentation::Cell;
    }
    if (copy.granuleBitsFilled) {
        SlotData word = *target.compiledStructure().extensionSlot();
        copy.offsetOfExtensionWordInTarget = word.offset;
        copy.extensionWordOfTargetIsWide = word.representation() == SlotRepresentation::Cell;
        Allocator allocator = vm.heap.cagedAuxiliarySpace<SubspaceAccess::OnMainThread>()->allocatorFor(extensionCellSizeFor(std::popcount(copy.granuleBitsFilled)), AllocatorForMode::EnsureAllocator);
        if (!allocator)
            return false;
        copy.allocatorOfNewExtension = allocator.localAllocator();
        copy.sizeOfNewExtension = copy.allocatorOfNewExtension->cellSize();
    }
    copy.structureOfTarget = target.structureID();
    copy.structureOfSource = source.structureID();
    describeMovesForStub(copy);
    return true;
}

static NEVER_INLINE void offerCopyWithoutFrame(VM& vm, State& state, MappedCopy& copy, const Object& target, const Object& source)
{
    if (copy.mayTakeNoFrame) {
        copy.mayTakeNoFrame = false;
        copy.takesNoFrame = takesNoFrame(vm, state, copy, target, source);
    }
    if (copy.takesNoFrame)
        placeOfCopyWithoutFrame(state, copy) = &copy;
}

bool clearFieldsMovedInVain(const MappedCopy& copy, const MappedCopy::Move* end, JSCell* target)
{
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
    copy.targetWords = target.orderAndShapeWords();
    copy.sourceWords = source.orderAndShapeWords();
    copy.excludedRunNumberPlusOne = excludedRunNumberPlusOne;
    copy.state = MappedCopy::State::IsNotServed;
    if (sourceLayout.keysAreNeverEnumerated)
        return false;
    uint32_t orderWordOfSource = source.orderWord();
    std::span<const uint16_t> entries = state.objects->keyOrders.entriesInOrder(sourceLayout, orderOf(orderWordOfSource)).first(countOf(orderWordOfSource));
    bool listsKeys = !targetLayout.keysAreNeverEnumerated;
    Vector<uint16_t, 32> keysAfter;
    if (listsKeys) {
        Object::KeyOrderStorage storage;
        keysAfter.append(target.keyOrder(state, storage));
    }
    BitVector isListed;
    for (uint16_t key : keysAfter) {
        if (key >= targetLayout.numberOfFields)
            return false;
        isListed.set(key);
    }
    copy.granuleBitsOfSource = sourceLayout.hasExtension() ? granuleBitsOf(source.extension()) : 0;
    copy.granuleBitsOfTargetBefore = targetLayout.hasExtension() ? granuleBitsOf(target.extension()) : 0;
    // Where nobody lists the keys, the order word says nothing of what the target holds.
    bool mayHoldFieldsUnknownHere = !listsKeys && target.orderWord() != unorderedOrderWord;
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
        moves.last().overwritesField = listsKeys && isListed.set(key);
        if (listsKeys && !moves.last().overwritesField)
            keysAfter.append(key);
    }
    for (MappedCopy::Move& move : moves) {
        if (move.readsExtension)
            move.from = offsetInExtensionWith(copy.granuleBitsOfSource, move.from);
        if (move.writesExtension)
            move.to = offsetInExtensionWith(copy.granuleBitsOfTargetBefore | copy.granuleBitsFilled, move.to);
    }
    copy.orderWordAfter = target.orderWord();
    if (listsKeys && keysAfter.size() != countOf(copy.orderWordAfter)) {
        std::optional<uint32_t> numbered = state.objects->keyOrders.orderStartingWith(targetLayout, keysAfter.span());
        if (!numbered)
            return false;
        copy.orderWordAfter = *numbered;
    }
    copy.moves = WTF::move(moves);
    copy.state = MappedCopy::State::IsServed;
    copy.mayTakeNoFrame = !mayHoldFieldsUnknownHere && !copy.granuleBitsOfTargetBefore;
    return true;
}

static NEVER_INLINE bool tryReadInOtherWay(State& state, const MappedCopy::Move& move, std::span<const FieldData> fieldsOfTarget, const uint8_t* slot, uint64_t& bits)
{
    JSValue value = readSlot(move.representationOfSource, slot, *state.strings);
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
    return tryWriteSlot(*state.strings, move.representationOfTarget, &bits, value);
}

static ALWAYS_INLINE RepeatedCopy tryCopyByFieldMap(VM& vm, State& state, Object& target, const Object& source, uint32_t excludedRunNumberPlusOne)
{
    uint32_t orderWordOfTarget = target.orderWord();
    uint32_t orderWordOfSource = source.orderWord();
    if ((orderWordOfTarget | orderWordOfSource) & ~orderAndCountMask || isOrderStoredOutOfLine(orderWordOfTarget) || isOrderStoredOutOfLine(orderWordOfSource))
        return RepeatedCopy::IsNotDone;
    // The numbers of orders are those of one realm.
    JSGlobalObject* realm = state.instance.globalObject;
    if (target.structure()->realm() != realm || source.structure()->realm() != realm)
        return RepeatedCopy::IsNotDone;
    uint64_t targetWords = target.orderAndShapeWords();
    uint64_t sourceWords = source.orderAndShapeWords();
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

    // What is read is held by nothing but the source until it is stored.
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
        }, state.instance.runtime.constants.literalStrings);
        if (!didRead)
            return RepeatedCopy::IsNotDone;
    }

    // An Extension that the copy makes is whole before the target has it.
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

static NEVER_INLINE RepeatedCopy tryRepeatAssignmentByRecentPlan(VM& vm, State& state, Object& to, const Object& from, uint32_t excludedRunNumberPlusOne)
{
    AssignmentPlan* plan = state.objects->caches->assignmentPlans.recentPlan(to.structureID(), from.structureID(), from.orderWord(), excludedRunNumberPlusOne);
    if (!plan || &to == &from)
        return RepeatedCopy::IsNotDone;
    if (!plan->fillsEmptySlotsOf(to, from, excludedRunNumberPlusOne))
        return tryRunPlanAsBefore(vm, state, to, *plan, { const_cast<Object*>(&from), &from, from.extraProperties(), from.orderWord(), { }, true, from.extension(), state.strings.get() }, excludedRunNumberPlusOne);
    DeferGC deferGC(vm);
    if (!tryFillEmptySlotsAsBefore(vm, to, state, *plan, from))
        return RepeatedCopy::IsNotDone;
    didChangeOwnProperties(vm, to, VM::StructureChainIntegrityEvent::Add, nullptr);
    return RepeatedCopy::AsThePlanDidBefore;
}

// Each half is a function of its own, so that the copy by a map saves no registers for the plan's.
static NEVER_INLINE RepeatedCopy tryRepeatAssignmentBetweenLayouts(VM& vm, State& state, Object& to, const Object& from, uint32_t excludedRunNumberPlusOne)
{
    if (&to != &from) {
        if (RepeatedCopy way = tryCopyByFieldMap(vm, state, to, from, excludedRunNumberPlusOne); way != RepeatedCopy::IsNotDone)
            return way;
    }
    return tryRepeatAssignmentByRecentPlan(vm, state, to, from, excludedRunNumberPlusOne);
}

static std::optional<MappedCopy::MoveKind> moveKindOutOfOrdinaryObject(const RuntimeLayoutTable& layouts, const AssignmentPlan::FieldStore& store)
{
    using Kind = MappedCopy::MoveKind;
    const FieldData& to = *store.to;
    if (store.from || !store.isDirect || to.isOutOfLine() || to.holdsRawValueFromBirth || to.flags & (fieldIsStoredOnce | fieldAttributeFlags | fieldMayExistWithoutStorage))
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
        return acceptsEveryValue(layouts.typeOf(to)) ? Kind::JSValueUnlessMarker : Kind::BoxedAsJSValue;
    default:
        return std::nullopt;
    }
}

// An ordinary object without storage outside its cell has zero where a compiled object has its order and shape words, so the stub and tryCopyByFieldMapWithoutFrame() find the copy by the words of the target and tell it by the Structure of the source.
static bool tryDescribeCopyOutOfOrdinaryObject(VM& vm, State& state, const AssignmentPlan& plan, MappedCopy& copy, const Object& target, uint32_t excludedRunNumberPlusOne)
{
    if (state.objects->caches->assignmentPlans.countsCopies() || validatesRuntime(vm))
        return false;
    if (plan.orderWordBeforeRun != unorderedOrderWord || !plan.extraPropertyStores.isEmpty() || plan.granuleBitsFilled || plan.orderWordAfterRun & ~orderAndCountMask || isOrderStoredOutOfLine(plan.orderWordAfterRun))
        return false;
    for (const AssignmentPlan::FieldStore& store : plan.fieldStores) {
        std::optional<MappedCopy::MoveKind> kind = moveKindOutOfOrdinaryObject(state.objects->layouts, store);
        if (!kind || !isInlineOffset(store.sourceOffset))
            return false;
        const FieldData& to = *store.to;
        SlotRepresentation representation = to.slot.representation();
        MappedCopy::Move move { static_cast<uint32_t>(JSObject::offsetOfInlineStorage() + store.sourceOffset * sizeof(EncodedJSValue)), to.slot.offset, *kind, static_cast<uint8_t>(widthOf(representation)),
            false, false, SlotRepresentation::JSValue, representation, store.acceptedKinds.toRaw(), target.layout().indexOf(to) };
        move.leastInt32 = store.leastInt32;
        move.greatestInt32 = store.greatestInt32;
        copy.moves.append(move);
        copy.mayStoreCells |= traitsOf(representation).holdsCell;
    }
    copy.targetWords = static_cast<uint64_t>(target.shapeWord()) << 32 | unorderedOrderWord;
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

// The plan has just run as before.
static NEVER_INLINE void offerCopyOutOfOrdinaryObject(VM& vm, State& state, AssignmentPlan& plan, const Object& target, JSObject* source, uint32_t excludedRunNumberPlusOne)
{
    AOT_THRESHOLD(uint8_t, maxNumberOfCopiesInVain, 8);
    if (!plan.copyWithoutFrame) {
        auto copy = makeUniqueWithoutFastMallocCheck<MappedCopy>();
        plan.mayCopyWithoutFrame = !source->butterfly() && tryDescribeCopyOutOfOrdinaryObject(vm, state, plan, *copy, target, excludedRunNumberPlusOne);
        if (!plan.mayCopyWithoutFrame)
            return;
        copy->state = MappedCopy::State::IsServed;
        plan.copyWithoutFrame = WTF::move(copy);
    }
    MappedCopy& copy = *plan.copyWithoutFrame;
    const MappedCopy*& place = *copy.placeOfOwnedCopy;
    if (place != &copy) {
        place = &copy;
        copy.numberOfCopiesInVain = 0;
        return;
    }
    // A value did not go to its slot as it is.
    if (++copy.numberOfCopiesInVain > maxNumberOfCopiesInVain) {
        plan.mayCopyWithoutFrame = false;
        plan.copyWithoutFrame = nullptr;
    }
}

static NEVER_INLINE RepeatedCopy runPlanAsBeforeAndOfferCopy(VM& vm, State& state, Object& to, AssignmentPlan& plan, JSObject* from, uint32_t excludedRunNumberPlusOne)
{
    RepeatedCopy way = tryRunPlanAsBefore(vm, state, to, plan, { from, nullptr, from, 0, { }, true }, excludedRunNumberPlusOne);
    if (way != RepeatedCopy::IsNotDone)
        offerCopyOutOfOrdinaryObject(vm, state, plan, to, from, excludedRunNumberPlusOne);
    return way;
}

static NEVER_INLINE RepeatedCopy tryRepeatAssignmentFromOrdinaryObject(VM& vm, State& state, Object& to, JSValue source, uint32_t excludedRunNumberPlusOne)
{
    if (!source.isCell() || source.asCell()->type() != FinalObjectType)
        return RepeatedCopy::IsNotDone;
    JSObject* from = asObject(source.asCell());
    if (from->canHaveExistingOwnIndexedProperties())
        return RepeatedCopy::IsNotDone;
    AssignmentPlan* plan = state.objects->caches->assignmentPlans.recentPlan(to.structureID(), from->structureID(), 0, excludedRunNumberPlusOne);
    if (!plan)
        return RepeatedCopy::IsNotDone;
    if (plan->mayCopyWithoutFrame) [[unlikely]]
        return runPlanAsBeforeAndOfferCopy(vm, state, to, *plan, from, excludedRunNumberPlusOne);
    return tryRunPlanAsBefore(vm, state, to, *plan, { from, nullptr, from, 0, { }, true }, excludedRunNumberPlusOne);
}

RepeatedCopy tryRepeatAssignment(VM& vm, State& state, JSObject* target, JSValue source, uint32_t excludedRunNumberPlusOne)
{
    ASSERT(isCompiledObject(target));
    Object* to = uncheckedDowncast<Object>(target);
    Object* from = compiledObjectOrNull(source);
    if (!from)
        return tryRepeatAssignmentFromOrdinaryObject(vm, state, *to, source, excludedRunNumberPlusOne);
    // An object of the target's own layout is copied word by word. Either answer is handed on as it comes, so nothing is saved here for afterwards.
    static_assert(!static_cast<uint8_t>(RepeatedCopy::IsNotDone) && static_cast<uint8_t>(RepeatedCopy::ByWordsOrMoves) == 1);
    if (!excludedRunNumberPlusOne && to->shapeWord() == from->shapeWord())
        return static_cast<RepeatedCopy>(to->tryCopyFieldsOf(vm, *from));
    return tryRepeatAssignmentBetweenLayouts(vm, state, *to, *from, excludedRunNumberPlusOne);
}

AOT_THRESHOLD(size_t, minNumberOfNamesPutTogether, 3);

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
    AssignmentPlan* plan = plans.planFor(vm, &target, extra, *assignmentSource);
    if (!defines && plan->isUsable && plan->storesNameBehindSymbol) {
        // Stores that go one by one through the objects above take the names first, then the symbols.
        if (!PropertyWalk::wouldPerformFastPut(vm, &target))
            return false;
        // The question may have changed an object above, and with it the plan.
        plan = plans.planFor(vm, &target, extra, *assignmentSource);
    }
    if (!plan->isUsable)
        return false;
    assignByPlan(vm, target, state, *plan, extra, *assignmentSource);
    return true;
}

bool Object::tryAssignPropertiesOf(VM& vm, JSObject* source, std::span<UniquedStringImpl* const> excludedNames)
{
    return tryAssignOrDefinePropertiesOf(vm, *this, source, excludedNames, false);
}

bool tryDefinePropertiesOnOrdinaryObject(VM& vm, State& state, JSObject* target, JSObject* source, std::span<UniquedStringImpl* const> excludedNames)
{
    ASSERT(target->type() == FinalObjectType && !target->structure()->isCompiledStructure());
    if (source == target || hasIndexedProperties(target->indexingType()))
        return false;
    std::optional<AssignmentSource> assignmentSource = assignmentSourceFor(source, excludedNames, true);
    if (!assignmentSource || (assignmentSource->compiledObject && &assignmentSource->compiledObject->state() != &state))
        return false;
    DeferGC deferGC(vm);
    AssignmentPlan* plan = state.objects->caches->assignmentPlans.planFor(vm, target, nullptr, *assignmentSource);
    if (!plan->isUsable)
        return false;
    // Nothing of the target changes before every value is there. No collection runs, so the values need no roots.
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

bool tryDefinePropertiesOf(VM& vm, Object& target, JSObject* source, std::span<UniquedStringImpl* const> excludedNames)
{
    return tryAssignOrDefinePropertiesOf(vm, target, source, excludedNames, true);
}

// Where a value does not go to its slot, or a slot is not where a run looks for it: one property behind the other, in the order of the source. False where a value has left its slot.
// Everything is read before the first definition by name and the first value that leaves its slot, which can drop the plan. The caller defers collections, so the values need no roots.
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
        // The place of a field among the keys is in the order word wherever its value lies.
        if (!target.isInKeyOrder(state, field))
            target.didCreateField(state, field, false);
        valuesThatLeaveTheirSlots.append(&property);
    }
    defineWhatWaits();
    for (const Property* property : valuesThatLeaveTheirSlots)
        target.moveFieldToExtraProperties(vm, *property->field, { property->value, 0 }, SlotExitReason::StoreOfOtherType);
    return valuesThatLeaveTheirSlots.isEmpty();
}

static void assignByPlan(VM& vm, Object& target, State& state, AssignmentPlan& plan, JSObject* extra, const AssignmentSource& source)
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
    // A source that holds a value which its field does not take mostly does so the next time too.
    AOT_THRESHOLD(uint8_t, maxNumberOfRunsInVain, 2);
    if (plan.numberOfRunsInVain > maxNumberOfRunsInVain || !run.tryRunListingOnce(source)) {
        plans.notePlanWithoutUse("a run went one property behind the other"_s);
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
