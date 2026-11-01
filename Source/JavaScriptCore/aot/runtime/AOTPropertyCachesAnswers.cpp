/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCachesAnswers.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableBindings.h"
#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTCompiledStructure.h"
#include "AOTImageOfCellScope.h"
#include "AOTInstance.h"
#include "AOTModuleHost.h"
#include "AOTModuleNamespace.h"
#include "AOTModules.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeCounters.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "AOTVMState.h"
#include "DeletePropertySlot.h"
#include "ErrorInstance.h"
#include "GetterSetter.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSGlobalProxy.h"
#include "JSTypedArrayViewPrototypeInternal.h"
#include "PropertySlot.h"
#include "PutPropertySlot.h"
#include <wtf/Atomics.h>

namespace JSC { namespace AOT {

namespace {

enum class ExtraPropertyLookupResult : uint8_t { Unknown, Absent, Found };

struct ExtraPropertyLookup {
    ExtraPropertyLookupResult result { ExtraPropertyLookupResult::Unknown };
    PropertyOffset offset { invalidOffset };
    unsigned attributes { 0 };
};

enum class OutOfLineStorageChange : uint8_t { Unsupported, Stays, IsFirst, Doubled, GrowsBesideIndexingHeader };

}

static PropertyAnswer uncacheableOnce(SlotAccess access)
{
    return PropertyAnswer::uncacheable(access, true);
}

static bool isFunctionOwnName(VM& vm, UniquedStringImpl* uid)
{
    return uid == vm.propertyNames->prototype.impl() || uid == vm.propertyNames->length.impl() || uid == vm.propertyNames->name.impl();
}

bool isNeverCachedName(VM& vm, UniquedStringImpl* uid)
{
    return parseIndex(PropertyName(uid)) || uid == vm.propertyNames->underscoreProto.impl() || (PropertyName(uid).isPrivateName() && !PropertyName(uid).passesThroughProxies());
}

static PropertyCaches* cachesWithPreparationBeforeLookupsUnder(VM& vm, UniquedStringImpl* uid)
{
    if (!uid->isSymbol())
        return nullptr;
    for (Instance* instance : vm.aotState().instances) {
        PropertyCaches& caches = *instance->state->propertyCaches;
        if (caches.preparationBeforeLookupsUnder(uid))
            return &caches;
    }
    return nullptr;
}

bool isPreparedBeforeLookupsSlow(VM& vm, UniquedStringImpl* symbol)
{
    return cachesWithPreparationBeforeLookupsUnder(vm, symbol);
}

void prepareForLookup(JSGlobalObject* globalObject, JSObject* compiledObject, UniquedStringImpl* uid)
{
    // Each instance registers its own function under the one uid of an engine symbol: the holder's instance answers for it.
    PropertyCaches* caches = &*uncheckedDowncast<Object>(compiledObject)->state().propertyCaches;
    JSObject* function = caches->preparationBeforeLookupsUnder(uid);
    // What the function itself looks at is in the library's own form. A frozen object was asked when it was frozen, and takes no store since.
    if (!function || caches->isPreparingHolder() || caches->holderLookedAtByName() == compiledObject || uncheckedDowncast<Object>(compiledObject)->isFrozen())
        return;
    caches->setIsPreparingHolder(true);
    call(globalObject, function, JSC::getCallData(function), compiledObject, ArgList());
    caches->setIsPreparingHolder(false);
}

void LookByName::begin(VM&, JSValue holder, UniquedStringImpl* symbol)
{
    if (!holder.isObject())
        return;
    Object* compiled = compiledObjectOrNull(asObject(holder));
    if (!compiled || !compiled->state().propertyCaches->preparationBeforeLookupsUnder(symbol))
        return;
    m_caches = &*compiled->state().propertyCaches;
    m_previous = m_caches->exchangeHolderLookedAtByName(holder.asCell());
}

void LookByName::end()
{
    m_caches->exchangeHolderLookedAtByName(m_previous);
}

void prepareForWalk(JSGlobalObject* globalObject, JSObject* object)
{
    Object* compiled = compiledObjectOrNull(object);
    if (!compiled || !compiled->layout().hasFieldsWithSymbolKeys || !compiled->state().propertyCaches->hasPreparationsBeforeLookups())
        return;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    RuntimeLayoutTable& layouts = compiled->state().objects->layouts;
    for (const FieldData& field : compiled->layout().fields()) {
        if (!layouts.hasSymbolKey(field) || !compiled->isInSlot(field))
            continue;
        prepareForLookup(globalObject, compiled, layouts.nameOf(vm, field));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

Structure* structureForPrimitive(JSGlobalObject* globalObject, JSValue primitive)
{
    // Whoever asks keeps the answer across code that may give the cell another Structure: see putAndCache().
    if (primitive.isCell())
        return WTF::opaque(primitive.asCell()->structure());
    if (primitive.isNumber())
        return globalObject->numberObjectStructure();
    return primitive.isBoolean() ? globalObject->booleanObjectStructure() : nullptr;
}

std::optional<PropertyOffset> findOffset(VM& vm, Structure* structure, UniquedStringImpl* uid, ChainWalkMode mode, unsigned& attributes)
{
    std::optional<PropertyOffset> offset = structure->getWithoutMaterializing(uid, attributes);
    if (!offset && mode == ChainWalkMode::MayAllocate)
        offset = structure->get(vm, uid, attributes);
    return offset;
}

bool flattenOnce(VM& vm, JSObject* object)
{
    Structure* structure = object->structure();
    if (!structure->isDictionary())
        return true;
    if (structure->hasBeenFlattenedBefore() || structure->typeInfo().prohibitsPropertyCaching() || structure->isCompiledStructure())
        return false;
    bool hasGivenNamesUp = structure->isUncacheableDictionary();
    structure->flattenDictionaryStructure(vm, object);
    if (vm.aotState().hasObjectsBeingFilled) {
        Instance* instance = Instance::of(structure->realm());
        if (instance && instance->state->firstAdditions && instance->state->firstAdditions->forget(object) && !hasGivenNamesUp)
            didFinishFilling(structure);
    }
    return true;
}

bool flattenForListing(VM& vm, JSObject* object)
{
    Structure* structure = object->structure();
    if (!structure->isDictionary() || structure->isUncacheableDictionary())
        return flattenOnce(vm, object);
    if (structure->typeInfo().prohibitsPropertyCaching() || structure->isCompiledStructure())
        return false;
    structure->flattenDictionaryStructure(vm, object);
    return true;
}

static bool hasCacheableArrayLength(VM& vm, JSCell* base, Structure* structure, UniquedStringImpl* uid)
{
    if (!isJSArray(base) || uid != vm.propertyNames->length.impl() || structure->mayBePrototype())
        return false;
    IndexingType mode = structure->indexingMode();
    return hasUndecided(mode) || hasInt32(mode) || hasDouble(mode) || hasContiguous(mode);
}

bool structureProvesOwnAbsence(VM& vm, JSCell* base, Structure* structure, UniquedStringImpl* uid, ChainWalkMode mode)
{
    if (structure->isDictionary() || !structure->propertyAccessesAreCacheable() || isModuleNamespace(base))
        return false;
    if (!structure->propertyAccessesAreCacheableForAbsence() && (mode == ChainWalkMode::AllocatesNothing || mayHaveLazyProperty(vm, base, uid, LookupPosition::Receiver)))
        return false;
    unsigned attributes = 0;
    std::optional<PropertyOffset> offset = findOffset(vm, structure, uid, mode, attributes);
    if (!offset || isValidOffset(*offset))
        return false;
    JSType type = structure->typeInfo().type();
    return !((isJSFunctionType(type) || type == SlimClosureType) && isFunctionOwnName(vm, uid));
}

static PropertyAnswer answerForCustomAccessor(VM& vm, SlotAccess access, ChainWalkMode mode, const CustomAccessorRecord& record, bool isOwn, ChainObjects&& changeableObjects)
{
    CustomAccessorTable& table = vm.aotState().propertyCaches->customAccessors();
    std::optional<uint32_t> index = mode == ChainWalkMode::MayAllocate ? table.add(record) : table.find(record);
    if (!index)
        return uncacheableOnce(access);
    return { .kind = PropertyAnswerKind::CustomAccessor, .access = access, .isOwn = isOwn, .customAccessor = *index, .changeableObjects = WTF::move(changeableObjects) };
}

static PropertyAnswer classifyCustomGetter(const ReadQuery& query)
{
    VM& vm = query.globalObject->vm();
    const PropertySlot& slot = query.slot;
    if (!query.structureBefore->propertyAccessesAreCacheable() || query.structureBefore->isDictionary())
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    // Null for a primitive. classifyRead() has answered for what a primitive has of its own.
    JSObject* base = query.base.getObject();
    JSObject* holder = slot.slotBase();
    if (base && base->structure() != query.structureBefore)
        return uncacheableOnce(SlotAccess::Read);
    ChainWalk walk;
    if (holder != base) {
        auto* closure = base ? dynamicDowncast<Closure>(base) : nullptr;
        if ((closure && closure->isLazyClassObject()) || (base && !structureProvesOwnAbsence(vm, base, query.structureBefore, query.uid, query.mode)))
            return PropertyAnswer::uncacheable(SlotAccess::Read);
        walk = walkChain({ query.globalObject, query.structureBefore, base, query.mode }, query.uid);
        if (walk.result != ChainWalkResult::Found || walk.holder != holder || !(walk.attributes & PropertyAttribute::CustomAccessorOrValue))
            return PropertyAnswer::uncacheableAfter(SlotAccess::Read, WTF::move(walk));
    }
    bool passesHolder = !(slot.attributes() & PropertyAttribute::CustomAccessor);
    auto domAttribute = slot.domAttribute();
    JSObject* passed = passesHolder ? holder : base;
    if (domAttribute && (!passed || !passed->inherits(domAttribute->classInfo)))
        return PropertyAnswer::uncacheableAfter(SlotAccess::Read, WTF::move(walk));
    uint32_t offsetOfCachedValue = !passesHolder && domAttribute ? BuiltinBindings::cachedValueOffsetOf(domAttribute->classInfo, query.uid).value_or(0) : 0;
    JSObject* slotBase = passesHolder && holder != base ? holder : nullptr;
    CustomAccessorRecord record { std::bit_cast<void*>(slot.customGetter().taggedPtr()), slotBase, holder->realm(), offsetOfCachedValue, 0 };
    return answerForCustomAccessor(vm, SlotAccess::Read, query.mode, record, holder == base, WTF::move(walk.changeableObjects));
}

static PropertyAnswer classifyAbsentClosurePrototype(const ReadQuery& query)
{
    if (!query.structureBefore->propertyAccessesAreCacheable() || query.structureBefore->isDictionary())
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    ChainWalk walk = walkChain({ query.globalObject, query.structureBefore, asObject(query.base), query.mode }, query.uid);
    if (walk.result != ChainWalkResult::Absent)
        return PropertyAnswer::uncacheableAfter(SlotAccess::Read, WTF::move(walk));
    CustomAccessorRecord record { std::bit_cast<void*>(closurePrototypePropertyGetter().taggedPtr()), nullptr, query.structureBefore->realm(), 0, 0 };
    return answerForCustomAccessor(query.globalObject->vm(), SlotAccess::Read, query.mode, record, true, WTF::move(walk.changeableObjects));
}

static PropertyAnswer classifyCustomSetter(const StoreQuery& query)
{
    VM& vm = query.globalObject->vm();
    SlotAccess access = accessFor(query.mode);
    JSObject* base = query.base;
    JSObject* holder = query.slot.base();
    Structure* structureBefore = query.structureBefore;
    if (base->structure() != structureBefore)
        return uncacheableOnce(access);
    if (!structureBefore->propertyAccessesAreCacheable() || structureBefore->isDictionary() || structureBefore->typeInfo().overridesPut() || structureBefore->isImmutableProperty(query.uid))
        return PropertyAnswer::uncacheable(access);
    ChainWalk walk;
    if (holder != base) {
        if (!structureProvesOwnAbsence(vm, base, structureBefore, query.uid, ChainWalkMode::MayAllocate))
            return PropertyAnswer::uncacheable(access);
        walk = walkChain({ query.globalObject, structureBefore, base, ChainWalkMode::MayAllocate }, query.uid);
        if (storeThroughChain(walk) != StoreThroughChain::CallsCustomSetter || walk.holder != holder)
            return PropertyAnswer::uncacheableAfter(access, WTF::move(walk));
    }
    CustomAccessorRecord record { std::bit_cast<void*>(query.slot.customSetter().taggedPtr()), nullptr, holder->realm(), 0, 0 };
    return answerForCustomAccessor(vm, access, ChainWalkMode::MayAllocate, record, holder == base, WTF::move(walk.changeableObjects));
}

static ExtraPropertyLookup lookUpExtraProperty(VM& vm, const CompiledStructure& structure, UniquedStringImpl* uid, ChainWalkMode mode)
{
    if (!structure.hasExtraProperties())
        return { ExtraPropertyLookupResult::Absent };
    ExtraPropertyLookup lookup;
    Structure* extraPropertiesStructure = structure.extraPropertiesStructure();
    std::optional<PropertyOffset> offset = extraPropertiesStructure ? findOffset(vm, extraPropertiesStructure, uid, mode, lookup.attributes) : std::nullopt;
    if (!offset)
        return lookup;
    if (isValidOffset(*offset))
        lookup.result = ExtraPropertyLookupResult::Found;
    else if (structure.listsEveryExtraProperty())
        lookup.result = ExtraPropertyLookupResult::Absent;
    lookup.offset = *offset;
    return lookup;
}

static PropertyAnswer classifyExtraPropertyRead(const ReadQuery& query, Object& object)
{
    VM& vm = query.globalObject->vm();
    if (query.mode == ChainWalkMode::MayAllocate && Object::ensureStructureForExtraPropertiesIfNeeded(vm, &object))
        return uncacheableOnce(SlotAccess::Read);
    ExtraPropertyLookup lookup = lookUpExtraProperty(vm, object.compiledStructure(), query.uid, query.mode);
    if (lookup.result != ExtraPropertyLookupResult::Found)
        return uncacheableOnce(SlotAccess::Read);
    if (!(lookup.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
        return PropertyAnswer::own(PropertyAnswerKind::ExtraProperty, SlotAccess::Read, lookup.offset);
    if (lookup.attributes & PropertyAttribute::Accessor)
        return PropertyAnswer::own(PropertyAnswerKind::ExtraAccessor, SlotAccess::Read, lookup.offset);
    return PropertyAnswer::uncacheable(SlotAccess::Read);
}

static void nameInheritedProperty(PropertyAnswer& answer, const ChainWalk& walk)
{
    if (walk.result != ChainWalkResult::Found)
        return;
    bool isAccessor = walk.attributes & PropertyAttribute::Accessor;
    if (isValidOffset(walk.offset)) {
        answer.kind = isAccessor ? PropertyAnswerKind::FieldOrInheritedAccessor : PropertyAnswerKind::FieldOrInheritedValue;
        answer.offset = walk.offset;
    } else if (!isAccessor && answer.access == SlotAccess::Read)
        answer.kind = PropertyAnswerKind::FieldOrInheritedField;
    else
        return;
    answer.holder = walk.holder;
}

static PropertyAnswer classifyFieldRead(const ReadQuery& query, Object& object, const FieldData& field)
{
    bool isAccessor = field.isAccessor();
    SlotRepresentation representation = field.slot.representation();
    if (isAccessor && representation != SlotRepresentation::JSValue && representation != SlotRepresentation::Cell && representation != SlotRepresentation::Cell32 && representation != SlotRepresentation::OptionalCell32)
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    PropertyAnswer answer { .kind = isAccessor ? PropertyAnswerKind::FieldAccessor : PropertyAnswerKind::Field, .isOwn = true, .field = &field };
    if (!field.canLeaveSlot())
        return answer;
    bool mayExistWithoutStorage = field.flags & fieldMayExistWithoutStorage;
    ExtraPropertyLookupResult amongExtraProperties = lookUpExtraProperty(query.globalObject->vm(), object.compiledStructure(), query.uid, query.mode).result;
    if (amongExtraProperties != ExtraPropertyLookupResult::Absent) {
        if (mayExistWithoutStorage || (amongExtraProperties == ExtraPropertyLookupResult::Unknown && object.isInSlot(field)))
            return answer;
        return classifyExtraPropertyRead(query, object);
    }
    ChainWalk walk = walkChain({ query.globalObject, query.structureBefore, &object, query.mode, ChainWalkUse::KeptForReadsThatAskObjects }, query.uid);
    if (walk.didFlatten)
        return uncacheableOnce(SlotAccess::Read);
    if (mayExistWithoutStorage) {
        bool findsPlainValue = walk.result == ChainWalkResult::Found && !(walk.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue) && isValidOffset(walk.offset);
        if (walk.firstObjectWithEmptySlot || (walk.result != ChainWalkResult::Absent && !findsPlainValue))
            return answer;
        nameInheritedProperty(answer, walk);
        answer.emptySlotMayBeUndefined = true;
    } else if (walk.firstObjectWithEmptySlot) {
        if (walk.result != ChainWalkResult::Unknown && !isAccessor) {
            answer.kind = PropertyAnswerKind::FieldOrInheritedField;
            answer.holder = walk.firstObjectWithEmptySlot;
        }
    } else {
        answer.absentIsUndefined = walk.result == ChainWalkResult::Absent;
        if (!(walk.attributes & PropertyAttribute::CustomAccessorOrValue) && !isAccessor)
            nameInheritedProperty(answer, walk);
    }
    answer.changeableObjects = WTF::move(walk.changeableObjects);
    return answer;
}

PropertyAnswer classifyNamespaceRead(const ReadQuery& query)
{
    VM& vm = query.globalObject->vm();
    auto* moduleNamespace = uncheckedDowncast<ModuleNamespace>(query.base.asCell());
    if (moduleNamespace->structure() != query.structureBefore)
        return uncacheableOnce(SlotAccess::Read);
    if (!moduleNamespace->hasOwnStructure()) {
        if (query.mode == ChainWalkMode::MayAllocate)
            moduleNamespace->ensureOwnStructure(vm);
        return uncacheableOnce(SlotAccess::Read);
    }
    if (vm.aotState().propertyCaches->isSharedByInstances() || moduleNamespace->isDeferred() || moduleNamespace->isAnsweredByMock())
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    std::optional<unsigned> index = moduleNamespace->find(query.uid);
    if (!index) {
        // `import * as pkg from "a-commonjs-package"; pkg.member`. The object has a Structure of its own, and gets another one when a spy or a mock replaces the value.
        if (JSValue captured = moduleNamespace->capturedValueWithoutAllocating(query.uid))
            return { .kind = PropertyAnswerKind::Constant, .isOwn = true, .constant = captured };
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    }
    // The module may be of another image of the realm: code there reads what it imports from here through this object.
    State& stateOfReader = State::of(query.structureBefore->realm());
    State& state = vm.aotState().hasSeveralImages ? *Instance::owning(moduleNamespace)->state : stateOfReader;
    bool isOfOtherImage = &state != &stateOfReader;
    const ModuleExportData& moduleExport = state.modules->table().exports(moduleNamespace->module())[*index];
    if (moduleExport.kind() == ModuleExportKind::Namespace) {
        // `export * as inner from "./inner"`: the one namespace object that the realm has for that module. What a spy or a mock replaces is replaced inside it.
        JSValue inner = state.modules->readExportIfInitialized(moduleNamespace->module(), *index);
        if (!inner)
            return uncacheableOnce(SlotAccess::Read);
        return { .kind = PropertyAnswerKind::Constant, .isOwn = true, .constant = inner };
    }
    if (moduleExport.kind() != ModuleExportKind::Variable || state.modules->mockThatAnswersFor(moduleExport))
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    SlotValueKind valueKind = traitsOf(moduleExport.variable.representation()).valueKind;
    if (valueKind == SlotValueKind::DeferredClosure) {
        // The variable has no storage, because nothing is ever stored to it: it is the one closure of a function.
        if (!isOfOtherImage)
            return PropertyAnswer::uncacheable(SlotAccess::Read);
        JSValue closure = ProgramStatics::readVariableWithoutAllocating(state.instance, moduleExport.variable);
        if (!closure)
            return uncacheableOnce(SlotAccess::Read);
        return { .kind = PropertyAnswerKind::Constant, .isOwn = true, .constant = closure };
    }
    if (!ProgramStatics::readVariableWithoutAllocating(state.instance, moduleExport.variable))
        return uncacheableOnce(SlotAccess::Read);
    std::optional<uint16_t> imageOfVariable = isOfOtherImage ? PropertyAnswer::tryEncodeImageOfVariable(state.instance.program->position()) : std::optional<uint16_t>(0);
    if (!imageOfVariable)
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    return { .kind = PropertyAnswerKind::Variable, .isOwn = true, .variable = moduleExport.variable, .imageOfVariable = *imageOfVariable };
}

enum class WordOfTypedArray : uint8_t { None, Length, ByteOffset };

// The typed array getters that return a word of the view unchanged.
static WordOfTypedArray findWordReturnedByGetter(Structure* receiverStructure, const ChainWalk& walk)
{
    JSType type = receiverStructure->typeInfo().type();
    if (!isTypedArrayType(type) || !isValidOffset(walk.offset))
        return WordOfTypedArray::None;
    auto* accessors = uncheckedDowncast<GetterSetter>(valueAtOffset(walk.holder, walk.offset).asCell());
    auto* getter = dynamicDowncast<JSFunction>(accessors->getter());
    if (!getter || !getter->isHostFunction())
        return WordOfTypedArray::None;
    if (getter->nativeFunction() == toTagged(NativeFunction(typedArrayViewProtoGetterFuncLength)))
        return WordOfTypedArray::Length;
    if (getter->nativeFunction() == toTagged(NativeFunction(typedArrayViewProtoGetterFuncByteOffset)))
        return WordOfTypedArray::ByteOffset;
    bool returnsLength = elementSize(typedArrayType(type)) == 1 && getter->nativeFunction() == toTagged(NativeFunction(typedArrayViewProtoGetterFuncByteLength));
    return returnsLength ? WordOfTypedArray::Length : WordOfTypedArray::None;
}

static int16_t wordOfInheritedField(JSCell* holder, UniquedStringImpl* uid)
{
    if (!isCompiledObject(holder))
        return 0;
    Object& object = *uncheckedDowncast<Object>(holder);
    const FieldData* field = object.fieldNamed(object.state(), uid);
    // A very large program numbers more names than an entry of the table of fields can hold.
    if (!field || !FieldTableEntry::hasColumn(field->name))
        return 0;
    FieldTableEntry entry = FieldTableEntry::forField(object.layout(), *field);
    switch (entry.form()) {
    case FieldTableEntry::Form::InlineInt32:
        return static_cast<int16_t>(entry.operand());
    case FieldTableEntry::Form::InlineNarrowCell:
        return -static_cast<int16_t>(entry.operand());
    default:
        return 0;
    }
}

static PropertyAnswer classifyReadAboveReceiver(const ReadQuery& query)
{
    const PropertySlot& slot = query.slot;
    if (slot.isCacheableCustom())
        return classifyCustomGetter(query);
    ChainWalk walk = walkChain({ query.globalObject, query.structureBefore, query.base.getObject(), query.mode, ChainWalkUse::KeptForReadsThatAskObjects }, query.uid);
    PropertyAnswerKind kind = PropertyAnswerKind::Uncacheable;
    WordOfTypedArray wordOfTypedArray = WordOfTypedArray::None;
    const FieldData* staticField = nullptr;
    if (walk.result != ChainWalkResult::Unknown && walk.firstObjectWithEmptySlot) {
        walk.holder = walk.firstObjectWithEmptySlot;
        walk.offset = invalidOffset;
        kind = PropertyAnswerKind::InheritedField;
    } else if (slot.isUnset()) {
        if (walk.result == ChainWalkResult::Absent)
            kind = PropertyAnswerKind::Absent;
    } else if (walk.result == ChainWalkResult::Found && walk.holder == slot.slotBase()) {
        bool isOwnedByCompiledObject = walk.owner->structure()->isCompiledStructure();
        bool isValue = slot.isCacheableValue() || (slot.isValue() && isOwnedByCompiledObject);
        bool isGetter = slot.isCacheableGetter() || (slot.isAccessor() && isOwnedByCompiledObject);
        if (isValue && !(walk.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
            kind = isValidOffset(walk.offset) ? PropertyAnswerKind::Value : PropertyAnswerKind::InheritedField;
        else if (isGetter && isValidOffset(walk.offset) && (walk.attributes & PropertyAttribute::Accessor)) {
            wordOfTypedArray = findWordReturnedByGetter(query.structureBefore, walk);
            kind = wordOfTypedArray == WordOfTypedArray::None ? PropertyAnswerKind::Accessor : PropertyAnswerKind::TypedArrayLength;
        }
        else if (query.mode == ChainWalkMode::MayAllocate && (staticField = StaticFields::findFieldForCache(query.globalObject->vm(), walk.holder, query.uid)))
            kind = PropertyAnswerKind::Variable;
    }
    if (kind == PropertyAnswerKind::Uncacheable)
        return PropertyAnswer::uncacheableAfter(SlotAccess::Read, WTF::move(walk));
    PropertyAnswer answer = PropertyAnswer::above(kind, SlotAccess::Read, WTF::move(walk));
    answer.readsByteOffset = wordOfTypedArray == WordOfTypedArray::ByteOffset;
    if (kind == PropertyAnswerKind::InheritedField && answer.dependsOnChain())
        answer.wordOfInheritedField = wordOfInheritedField(answer.holder, query.uid);
    if (staticField) {
        answer.field = staticField;
        answer.variable = staticField->slot;
    }
    return answer;
}

PropertyAnswer classifyRead(const ReadQuery& query)
{
    constexpr SlotAccess access = SlotAccess::Read;
    Structure* structure = query.structureBefore;
    VM& vm = query.globalObject->vm();
    UniquedStringImpl* uid = query.uid;
    const PropertySlot& slot = query.slot;
    bool mayAllocate = query.mode == ChainWalkMode::MayAllocate;
    if (!structure || isNeverCachedName(vm, uid))
        return PropertyAnswer::uncacheable(access);
    if (!query.base.isObject()) {
        if (query.base.isCell() && vm.aotState().propertyCaches->isSharedByInstances())
            return PropertyAnswer::uncacheable(access);
        if (query.base.isString()) {
            if (uid == vm.propertyNames->length.impl())
                return PropertyAnswer::own(PropertyAnswerKind::StringLength, access);
            if (!slot.isUnset() && !slot.slotBase())
                return PropertyAnswer::uncacheable(access);
        }
        return classifyReadAboveReceiver(query);
    }
    JSObject* receiver = asObject(query.base);
    if (vm.aotState().hasSeveralImages && belongsToOtherImageSlow(vm, receiver)) [[unlikely]] {
        if (isModuleNamespace(receiver) && access == SlotAccess::Read && receiver->structure() == structure)
            return classifyNamespaceRead(query);
        // Where a field lies and in what form says its slot alone, but for text, which is found through the tables of its image. A closure that is not made yet is made by the run time, under its image.
        if (access == SlotAccess::Read && isCompiledObject(receiver) && receiver->structure() == structure) {
            Object& object = *uncheckedDowncast<Object>(receiver);
            const FieldData* field = object.fieldNamed(object.state(), uid);
            SlotValueKind valueKind = field ? traitsOf(field->slot.representation()).valueKind : SlotValueKind::DeferredClosure;
            if (field && !field->isAccessor() && valueKind != SlotValueKind::LiteralString) {
                // What an empty slot means is found above the object as it is above any object, and a field that has left its slot is among the extra properties, whose place the Structure fixes.
                PropertyAnswer answer = classifyFieldRead(query, object, *field);
                if (answer.kind == PropertyAnswerKind::Field || answer.kind == PropertyAnswerKind::FieldOrInheritedValue || answer.kind == PropertyAnswerKind::ExtraProperty || !answer.isCacheable())
                    return answer;
                return { .kind = PropertyAnswerKind::Field, .isOwn = true, .field = field };
            }
            // What stands above the object is found as it is above any object, and is filed under the Structure.
            bool isOwn = !slot.isUnset() && slot.slotBase() == receiver;
            if (!field && !slot.isTaintedByOpaqueObject()) {
                if (Object::structureGuaranteesAbsence(structure, uid)) {
                    if (!isOwn)
                        return classifyReadAboveReceiver(query);
                } else if (PropertyAnswer answer = classifyExtraPropertyRead(query, object); answer.kind == PropertyAnswerKind::ExtraProperty || !answer.isCacheable())
                    return answer;
            }
        }
        // `TheirClass.field`: a static field lies in a variable of the image of its class. A class object with static fields shares its Structure with no other class,
        // and a field that leaves its variable leaves an empty word behind.
        if (isCompiledClosure(receiver) && mayAllocate && receiver->structure() == structure) {
            ImageOfCellScope imageOfClass(vm, receiver);
            const FieldData* field = StaticFields::findFieldForCache(vm, receiver, uid);
            if (receiver->structure() != structure)
                return uncacheableOnce(access);
            // Under a dictionary nothing is filed but a value of its own. The properties that stand for the fields have made the class object one: it is made flat once, like any object that is read by name.
            if (field && structure->isDictionary())
                return PropertyAnswer::uncacheable(access, flattenOnce(vm, receiver));
            SlotValueKind valueKind = field ? traitsOf(field->slot.representation()).valueKind : SlotValueKind::DeferredClosure;
            if (field && valueKind != SlotValueKind::DeferredClosure && valueKind != SlotValueKind::LiteralString) {
                std::optional<uint16_t> imageOfVariable = PropertyAnswer::tryEncodeImageOfVariable(Instance::owning(receiver)->program->position());
                if (!imageOfVariable)
                    return PropertyAnswer::uncacheable(access);
                return { .kind = PropertyAnswerKind::Variable, .isOwn = true, .field = field, .variable = field->slot, .imageOfVariable = *imageOfVariable };
            }
        }
        // Whatever else is read of a class or a function: `x instanceof Theirs` reads Symbol.hasInstance and "prototype". Its own image says what there is to say of it.
        // Kept is what the stubs read without a table of their image.
        if (access == SlotAccess::Read && isCompiledClosure(receiver) && receiver->structure() == structure) {
            Instance* owner = Instance::owning(receiver);
            PropertyAnswer answer = [&] {
                RunningImageScope imageOfClosure(vm.aotState(), owner->program);
                return classifyRead(query);
            }();
            switch (answer.kind) {
            case PropertyAnswerKind::Uncacheable:
            case PropertyAnswerKind::Value:
            case PropertyAnswerKind::Absent:
            case PropertyAnswerKind::Accessor:
                return answer;
            case PropertyAnswerKind::Variable: {
                std::optional<uint16_t> imageOfVariable = PropertyAnswer::tryEncodeImageOfVariable(owner->program->position());
                if (traitsOf(answer.variable.representation()).valueKind == SlotValueKind::DeferredClosure || !imageOfVariable)
                    break;
                answer.imageOfVariable = *imageOfVariable;
                return answer;
            }
            default:
                break;
            }
        }
        return PropertyAnswer::uncacheable(access);
    }
    JSType type = structure->typeInfo().type();
    if (type == GlobalProxyType && structure->classInfoForCells() == JSGlobalProxy::info() && mayAllocate && receiver->structure() == structure
        && structure->realm() == query.globalObject && uncheckedDowncast<JSGlobalProxy>(receiver)->target() == query.globalObject)
        return classifyGlobalProxyRead(query);
    if (type == ProxyObjectType && mayAllocate && !PropertyName(uid).isPrivateName()) {
        PropertyAnswer answer = PropertyAnswer::handlerOfProxyAnswers(access, State::of(query.globalObject).propertyCaches->keyForTraps(uid));
        std::optional<uint32_t> number = identifiersOfRealm(vm, query.globalObject)->numberOf(uid);
        if (number && RowDisplacementTable::hasColumn(*number) && *number <= UINT16_MAX)
            answer.columnOfNameForTrap = static_cast<uint16_t>(*number);
        return answer;
    }
    if ((structure->typeInfo().prohibitsPropertyCaching() && !structure->isCompiledStructure()) || isProxyType(type) || structure->needImpurePropertyWatchpoint())
        return PropertyAnswer::uncacheable(access);
    if (structure->isDictionary()) {
        // Nothing of a cacheable dictionary moves when it is made flat: what the look found is where it was found.
        bool staysInPlace = !structure->isUncacheableDictionary();
        bool didFlatten = mayAllocate && flattenOnce(vm, receiver);
        if (!(didFlatten && staysInPlace) && (didFlatten || structure->isUncacheableDictionary() || !slot.isCacheableValue() || slot.slotBase() != receiver))
            return PropertyAnswer::uncacheable(access, didFlatten);
    } else if (slot.isTaintedByOpaqueObject() && !isModuleNamespace(receiver)) {
        ChainWalk walk = walkChain({ query.globalObject, structure, receiver, query.mode }, uid);
        return walk.result != ChainWalkResult::Unknown ? uncacheableOnce(access) : PropertyAnswer::uncacheableAfter(access, WTF::move(walk));
    }
    if (receiver->structure() != structure && !slot.isCacheableGetter())
        return uncacheableOnce(access);
    if (isCompiledClosure(receiver) && mayAllocate) {
        if (const FieldData* field = StaticFields::findFieldForCache(vm, receiver, uid)) {
            if (receiver->structure() != structure)
                return uncacheableOnce(access);
            return { .kind = PropertyAnswerKind::Variable, .isOwn = true, .field = field, .variable = field->slot };
        }
    }
    if (type == SlimClosureType || (isCompiledClosure(receiver) && uncheckedDowncast<Closure>(receiver)->hasStructureOfNewClosure(vm))) {
        if (uid == vm.propertyNames->length.impl())
            return PropertyAnswer::own(PropertyAnswerKind::ClosureLength, access);
        if (uid == vm.propertyNames->name.impl())
            return PropertyAnswer::own(PropertyAnswerKind::ClosureName, access);
    }
    static_assert(Slot::presenceOf(Slot::Kind::CustomGetter) != SlotPresence::Present);
    if (slot.isUnset() && uid == vm.propertyNames->prototype.impl() && closureHandlesPrototypeProperty(receiver))
        return classifyAbsentClosurePrototype(query);
    bool isOwn = !slot.isUnset() && slot.slotBase() == receiver;
    if (structure->isCompiledStructure()) {
        if (Object::structureGuaranteesAbsence(structure, uid))
            return isOwn ? PropertyAnswer::uncacheable(access) : classifyReadAboveReceiver(query);
        if (!isCompiledObject(receiver) || receiver->structure() != structure)
            return uncacheableOnce(access);
        Object& object = *uncheckedDowncast<Object>(receiver);
        if (const FieldData* field = object.fieldNamed(object.state(), uid))
            return classifyFieldRead(query, object, *field);
        object.state().objects->layouts.didReadAbsentField(vm, object.layout(), uid);
        return classifyExtraPropertyRead(query, object);
    }
    if (isModuleNamespace(receiver))
        return classifyNamespaceRead(query);
    if (!isOwn) {
        if (!structureProvesOwnAbsence(vm, receiver, structure, uid, query.mode))
            return PropertyAnswer::uncacheable(access);
        ASSERT(!isCompiledClosure(receiver) || !uncheckedDowncast<Closure>(receiver)->isLazyClassObject());
        return classifyReadAboveReceiver(query);
    }
    bool isValue = slot.isCacheableValue();
    if ((isValue || slot.isCacheableGetter()) && structure->propertyAccessesAreCacheable()) {
        unsigned attributes = 0;
        std::optional<PropertyOffset> offset = findOffset(vm, structure, uid, query.mode, attributes);
        if (offset && isValidOffset(*offset) && *offset == slot.cachedOffset()) {
            if (isValue && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
                return PropertyAnswer::own(PropertyAnswerKind::Value, access, *offset);
            if (!isValue && attributes & PropertyAttribute::Accessor)
                return PropertyAnswer::own(PropertyAnswerKind::Accessor, access, *offset);
        }
    }
    if (slot.isCacheableCustom())
        return classifyCustomGetter(query);
    if (hasCacheableArrayLength(vm, receiver, structure, uid))
        return PropertyAnswer::own(PropertyAnswerKind::ArrayLength, access);
    return PropertyAnswer::uncacheable(access);
}

LazyPrototypeRead classifyReadThroughLazyPrototypes(JSGlobalObject* globalObject, JSObject* base, UniquedStringImpl* uid)
{
    VM& vm = globalObject->vm();
    if (isNeverCachedName(vm, uid))
        return { };
    auto found = State::of(globalObject).objects->classes.findThroughLazyPrototypes(vm, base, uid, false);
    if (!found)
        return { };
    if (!found->member && found->firstObjectAbove)
        return { LazyPrototypeReadResult::ContinuesAbove, { }, found->firstObjectAbove };
    LazyPrototypeRead read { LazyPrototypeReadResult::Answered, { .kind = PropertyAnswerKind::Absent } };
    if (found->member) {
        read.answer.kind = found->member->isAccessor ? PropertyAnswerKind::Accessor : PropertyAnswerKind::Value;
        read.answer.offset = found->member->offset;
        read.answer.holder = found->member->holder;
    }
    for (JSObject* prototype = base->getPrototypeDirect().getObject(); prototype && prototype->classInfo() == ClassPrototype::info(); prototype = prototype->getPrototypeDirect().getObject())
        read.answer.changeableObjects.append(prototype);
    return read;
}

JSValue readThroughAnswer(JSGlobalObject* globalObject, JSValue base, const PropertyAnswer& answer)
{
    if (answer.kind == PropertyAnswerKind::Absent)
        return jsUndefined();
    RELEASE_ASSERT(answer.kind == PropertyAnswerKind::Value || answer.kind == PropertyAnswerKind::Accessor);
    JSValue value = valueAtOffset(answer.holder, answer.offset);
    return answer.kind == PropertyAnswerKind::Value ? value : uncheckedDowncast<GetterSetter>(value.asCell())->callGetter(globalObject, base);
}

bool repeatsPropertyAddition(Structure* structure, UniquedStringImpl* uid)
{
    PropertyOffset offset = invalidOffset;
    return structure->isDictionary() || Structure::addPropertyTransitionToExistingStructure(structure, uid, 0, offset);
}

static ChainWalk walkChainAbove(const StoreQuery& query)
{
    Structure* before = query.structureBefore;
    bool hasSamePrototype = before->hasMonoProto() && query.base->getPrototypeDirect() == before->storedPrototype();
    return walkChain({ query.globalObject, before, hasSamePrototype ? query.base : nullptr, ChainWalkMode::MayAllocate, ChainWalkUse::KeptForStores }, query.uid);
}

bool isLastAdditionOfRun(Structure* before, Structure* after, UniquedStringImpl* uid)
{
    if (after->transitionKind() != TransitionKind::PropertyRunAddition || before->isDictionary() || !after->hasSameCellFlagsAs(*before))
        return false;
    PropertyOffset offsetOfLastName = invalidOffset;
    return Structure::addPropertyTransitionToExistingStructure(before, uid, 0, offsetOfLastName) == after;
}

static OutOfLineStorageChange outOfLineStorageChange(Structure* before, Structure* after, PropertyOffset offset, bool leadsToStructureOfRun = false)
{
    if (before->isDictionary() || after->isDictionary() || (after->previousID() != before && !leadsToStructureOfRun) || before->typeInfoBlob() != after->typeInfoBlob())
        return OutOfLineStorageChange::Unsupported;
    unsigned capacityBefore = before->outOfLineCapacity();
    unsigned capacityAfter = after->outOfLineCapacity();
    if (capacityAfter == capacityBefore)
        return OutOfLineStorageChange::Stays;
    if (offset != firstOutOfLineOffset + static_cast<PropertyOffset>(capacityBefore))
        return OutOfLineStorageChange::Unsupported;
    if (before->couldHaveIndexingHeader())
        return OutOfLineStorageChange::GrowsBesideIndexingHeader;
    if (!capacityBefore)
        return capacityAfter == initialOutOfLineCapacity ? OutOfLineStorageChange::IsFirst : OutOfLineStorageChange::Unsupported;
    bool doublesCapacity = capacityBefore >= initialOutOfLineCapacity && capacityAfter == 2 * capacityBefore && capacityAfter * sizeof(EncodedJSValue) <= MarkedSpace::largeCutoff;
    return doublesCapacity ? OutOfLineStorageChange::Doubled : OutOfLineStorageChange::Unsupported;
}

std::optional<uint32_t> capacityOfStorageGrownByStub(VM& vm, Structure* before, Structure* after, PropertyOffset offset)
{
    if (before->mayBePrototype() || before->mayHaveImmutableProperties() || before->isCompiledStructure() || isFunctionType(before->typeInfo().type()) || !after->propertyAccessesAreCacheable())
        return std::nullopt;
    switch (outOfLineStorageChange(before, after, offset)) {
    case OutOfLineStorageChange::Stays:
        return 0;
    case OutOfLineStorageChange::IsFirst:
        break;
    case OutOfLineStorageChange::Doubled:
        if (before->outOfLineCapacity() > runtimeLimitValue(vm, LimitID::MaxCapacityGrownByStub))
            return std::nullopt;
        break;
    case OutOfLineStorageChange::Unsupported:
    case OutOfLineStorageChange::GrowsBesideIndexingHeader:
        return std::nullopt;
    }
    return after->outOfLineCapacity();
}

bool isHostObjectOverridingPut(Structure* structure)
{
    return structure->typeInfo().overridesPut() && structure->typeInfo().type() == ObjectType;
}

static bool storesAreOrdinaryAlongPrototypeChain(Structure* structure)
{
    for (;;) {
        TypeInfo typeInfo = structure->typeInfo();
        bool hasOrdinaryPut = !typeInfo.overridesPut() || isFunctionType(typeInfo.type()) || structure->isCompiledStructure();
        if (typeInfo.overridesGetPrototype() || structure->hasPolyProto() || isProxyType(typeInfo.type()) || !hasOrdinaryPut)
            return false;
        JSValue prototype = structure->storedPrototype();
        if (!prototype.isObject())
            return true;
        structure = asObject(prototype)->structure();
    }
}

static bool isFunctionWithOrdinaryPut(VM& vm, JSCell* base, Structure* structure, UniquedStringImpl* uid)
{
    if (isCompiledClosure(base)) {
        auto* closure = uncheckedDowncast<Closure>(base);
        FunctionKindData kind = closure->kind();
        if (closure->classData() || kind.callable() == CallableKind::Bound || kind.isConstructor)
            return false;
    } else if (base->type() != SlimClosureWithPropertiesType && (base->type() != JSFunctionType || base->classInfo() != JSBoundFunction::info()))
        return false;
    if (isFunctionOwnName(vm, uid) || uid == vm.propertyNames->displayName.impl() || Closure::isNameThatBuiltinFunctionsRefuse(vm, uid))
        return false;
    if (structure->hasPolyProto() || structure->typeInfo().overridesGetPrototype())
        return false;
    JSValue prototype = structure->storedPrototype();
    return prototype.isNull() || (prototype.isObject() && storesAreOrdinaryAlongPrototypeChain(asObject(prototype)->structure()));
}

bool hasOrdinaryDefineForName(VM& vm, JSCell* base, Structure* structure, UniquedStringImpl* uid)
{
    if (!base->isObject() || parseIndex(*uid) || !structure->isStructureExtensible() || structure->isCompiledStructure())
        return false;
    if (base->type() == FinalObjectType)
        return true;
    if (isJSArray(base))
        return uid != vm.propertyNames->length.impl();
    if (structure->classInfoForCells() == ErrorInstance::info())
        return !ErrorInstance::isLazilyMaterialized(vm, uid);
    TypeInfo typeInfo = structure->typeInfo();
    return structure->classInfoForCells()->methodTable.defineOwnProperty == JSObject::defineOwnProperty && !typeInfo.overridesGetOwnPropertySlot() && !typeInfo.hasStaticPropertyTable();
}

static std::optional<PropertyAnswer> classifyReplacement(const StoreQuery& query)
{
    const PutPropertySlot& slot = query.slot;
    Structure* structure = query.structureBefore;
    if (!slot.isCacheablePut() || slot.type() != PutPropertySlot::ExistingProperty || slot.base() != query.base || query.base->structure() != structure)
        return std::nullopt;
    bool isDefinition = query.mode == PutMode::Define;
    if ((!isDefinition && isHostObjectOverridingPut(structure)) || structure->isImmutableProperty(query.uid))
        return std::nullopt;
    VM& vm = query.globalObject->vm();
    unsigned attributes = 0;
    PropertyOffset offset = slot.cachedOffset();
    if (structure->get(vm, query.uid, attributes) != offset || (isDefinition ? attributes : attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue))
        return std::nullopt;
    structure->didCachePropertyReplacement(vm, offset);
    return PropertyAnswer::own(PropertyAnswerKind::Value, accessFor(query.mode), offset);
}

static std::optional<PropertyAnswer> classifyAddition(const StoreQuery& query)
{
    const PutPropertySlot& slot = query.slot;
    VM& vm = query.globalObject->vm();
    SlotAccess access = accessFor(query.mode);
    Structure* before = query.structureBefore;
    Structure* after = query.base->structure();
    bool slotSaysSo = slot.isCacheablePut() && slot.type() == PutPropertySlot::NewProperty && slot.base() == query.base;
    bool structureSaysSo = after->previousID() == before && after->transitionKind() == TransitionKind::PropertyAddition && after->transitionPropertyName() == query.uid;
    bool isLastOfRun = !slotSaysSo && !structureSaysSo && isLastAdditionOfRun(before, after, query.uid);
    if (!slotSaysSo && !structureSaysSo && !isLastOfRun)
        return std::nullopt;
    if (before->isDictionary() || after->isDictionary() || before->mayBePrototype() || before->mayHaveImmutableProperties() || !after->propertyAccessesAreCacheable())
        return std::nullopt;
    if (after->previousID() != before && !isLastOfRun)
        return uncacheableOnce(access);
    unsigned attributes = 0;
    PropertyOffset offset = slotSaysSo ? slot.cachedOffset() : after->transitionOffset();
    if (after->get(vm, query.uid, attributes) != offset || attributes)
        return std::nullopt;
    PropertyAnswer answer = PropertyAnswer::own(PropertyAnswerKind::Addition, access, offset);
    answer.newStructure = after;
    bool isStore = access == SlotAccess::Store;
    bool isFunction = isStore && isFunctionWithOrdinaryPut(vm, query.base, before, query.uid);
    if (isStore) {
        bool hasOrdinarySet = isJSArray(query.base) ? hasOrdinaryDefineForName(vm, query.base, before, query.uid) : !before->typeInfo().overridesPut();
        if (!isFunction && !hasOrdinarySet)
            return std::nullopt;
        ChainWalk walk = walkChainAbove(query);
        if (storeThroughChain(walk) != StoreThroughChain::CreatesOwnProperty)
            return PropertyAnswer::uncacheableAfter(access, WTF::move(walk));
        answer.changeableObjects = WTF::move(walk.changeableObjects);
    }
    OutOfLineStorageChange change = outOfLineStorageChange(before, after, offset, isLastOfRun);
    if (change == OutOfLineStorageChange::Stays)
        return answer;
    if (change == OutOfLineStorageChange::Unsupported || (isJSFunctionType(before->typeInfo().type()) && !isFunction) || before->isCompiledStructure())
        return std::nullopt;
    if (change == OutOfLineStorageChange::Doubled && before->outOfLineCapacity() > runtimeLimitValue(vm, LimitID::MaxCapacityGrownByStub)) {
        countLimitHit(vm, LimitID::MaxCapacityGrownByStub);
        return std::nullopt;
    }
    answer.storageHasIndexingHeader = change == OutOfLineStorageChange::GrowsBesideIndexingHeader;
    answer.newStorageCapacity = Slot::storageCapacityAfterAdding(Slot::wordIndexFor(offset));
    return answer;
}

static std::optional<PropertyAnswer> classifySetterCall(const StoreQuery& query)
{
    if (query.mode == PutMode::Define || !query.slot.isCacheableSetter())
        return std::nullopt;
    VM& vm = query.globalObject->vm();
    Structure* structureBefore = query.structureBefore;
    bool isCompiled = structureBefore->isCompiledStructure();
    JSObject* holder = query.slot.base();
    if (holder == query.base) {
        unsigned attributes = 0;
        PropertyOffset offset = query.slot.cachedOffset();
        if (isCompiled || structureBefore->get(vm, query.uid, attributes) != offset || !(attributes & PropertyAttribute::Accessor))
            return std::nullopt;
        return PropertyAnswer::own(PropertyAnswerKind::Accessor, SlotAccess::Store, offset);
    }
    if (!isCompiled && (isCopyOnWrite(structureBefore->indexingMode()) || !structureProvesOwnAbsence(vm, query.base, structureBefore, query.uid, ChainWalkMode::MayAllocate)))
        return std::nullopt;
    ChainWalk walk = walkChainAbove(query);
    if (storeThroughChain(walk) != StoreThroughChain::CallsSetter || walk.holder != holder || !isValidOffset(walk.offset))
        return PropertyAnswer::uncacheableAfter(SlotAccess::Store, WTF::move(walk));
    return PropertyAnswer::above(PropertyAnswerKind::Accessor, SlotAccess::Store, WTF::move(walk));
}

static std::optional<PropertyAnswer> classifyStoreDecidedByChain(const StoreQuery& query)
{
    Structure* structureBefore = query.structureBefore;
    if (query.mode == PutMode::Define || query.slot.isCacheablePut() || !structureProvesOwnAbsence(query.globalObject->vm(), query.base, structureBefore, query.uid, ChainWalkMode::MayAllocate))
        return std::nullopt;
    ChainWalk walk = walkChainAbove(query);
    bool receiverStoresAsOrdinaryObject = !structureBefore->typeInfo().overridesPut() && !isCopyOnWrite(structureBefore->indexingMode());
    if (receiverStoresAsOrdinaryObject && storeThroughChain(walk) == StoreThroughChain::CallsSetter && isValidOffset(walk.offset) && walk.owner->structure()->isCompiledStructure())
        return PropertyAnswer::above(PropertyAnswerKind::Accessor, SlotAccess::Store, WTF::move(walk));
    return PropertyAnswer::uncacheableAfter(SlotAccess::Store, WTF::move(walk));
}

static PropertyAnswer classifyExtraPropertyReplacement(SlotAccess access, const ExtraPropertyLookup& lookup)
{
    if (access == SlotAccess::Define ? !lookup.attributes : !(lookup.attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue))
        return PropertyAnswer::own(PropertyAnswerKind::ExtraProperty, access, lookup.offset);
    if (access == SlotAccess::Store && (lookup.attributes & PropertyAttribute::Accessor))
        return PropertyAnswer::own(PropertyAnswerKind::ExtraAccessor, access, lookup.offset);
    return PropertyAnswer::uncacheable(access);
}

static PropertyAnswer classifyFieldStore(const StoreQuery& query, Object& object, const FieldData& field)
{
    SlotAccess access = accessFor(query.mode);
    const CompiledStructure* before = compiledObjectStructureOrNull(query.structureBefore);
    if (!before || &before->layout() != &object.layout() || (object.structure() != before && !query.slot.isCacheableSetter()))
        return uncacheableOnce(access);
    if (field.flags & (fieldIsStoredOnce | fieldAttributeFlags) || field.slot.representation() == SlotRepresentation::DeferredClosure)
        return PropertyAnswer::uncacheable(access);
    PropertyAnswer answer { .kind = PropertyAnswerKind::Field, .access = access, .isOwn = true, .field = &field };
    answer.undefinedNeedsNoStorage = field.flags & fieldMayExistWithoutStorage && object.state().objects->layouts.slotAdmits(field, jsUndefined());
    if (!field.canLeaveSlot() || before->mayBePrototype())
        return answer;
    ExtraPropertyLookup amongExtraProperties = lookUpExtraProperty(query.globalObject->vm(), *before, query.uid, ChainWalkMode::MayAllocate);
    if (amongExtraProperties.result == ExtraPropertyLookupResult::Found && object.structure() == before)
        return classifyExtraPropertyReplacement(access, amongExtraProperties);
    if (amongExtraProperties.result != ExtraPropertyLookupResult::Absent)
        return answer;
    if (access == SlotAccess::Define) {
        answer.kind = PropertyAnswerKind::FieldCreation;
        return answer;
    }
    bool hasSamePrototype = object.getPrototypeDirect() == before->storedPrototype();
    ChainWalk walk = walkChain({ query.globalObject, query.structureBefore, hasSamePrototype ? &object : nullptr, ChainWalkMode::MayAllocate, ChainWalkUse::KeptForStores }, query.uid);
    if (walk.didFlatten)
        return uncacheableOnce(access);
    StoreThroughChain outcome = storeThroughChain(walk);
    if (outcome == StoreThroughChain::CreatesOwnProperty)
        answer.kind = PropertyAnswerKind::FieldCreation;
    else if (outcome == StoreThroughChain::CallsSetter && !(field.flags & fieldMayExistWithoutStorage))
        nameInheritedProperty(answer, walk);
    answer.changeableObjects = WTF::move(walk.changeableObjects);
    return answer;
}

static PropertyAnswer classifyExtraPropertyAddition(const StoreQuery& query, Object& object, const ExtraPropertyLookup& lookup)
{
    SlotAccess access = accessFor(query.mode);
    const CompiledStructure* before = compiledObjectStructureOrNull(query.structureBefore);
    const CompiledStructure& after = object.compiledStructure();
    if (!before || lookup.attributes || !Object::structureGuaranteesAbsence(query.structureBefore, query.uid) || &after.layout() != &before->layout() || after.storedPrototype() != before->storedPrototype())
        return uncacheableOnce(access);
    Structure* extraPropertiesBefore = before->extraPropertiesStructure();
    Structure* extraPropertiesAfter = after.extraPropertiesStructure();
    if (!extraPropertiesAfter || (before->hasExtraProperties() && !extraPropertiesBefore) || extraPropertiesAfter->previousID() != (extraPropertiesBefore ? extraPropertiesBefore : Instance::of(before->realm())->realm.emptyExtraPropertiesStructure))
        return uncacheableOnce(access);
    OutOfLineStorageChange storageChange = OutOfLineStorageChange::Stays;
    if (!before->hasExtraProperties() && (lookup.offset || extraPropertiesAfter->typeInfoBlob() != JSFinalObject::defaultTypeInfoBlob()))
        storageChange = OutOfLineStorageChange::Unsupported;
    else if (before->hasExtraProperties() && !isInlineOffset(lookup.offset))
        storageChange = outOfLineStorageChange(extraPropertiesBefore, extraPropertiesAfter, lookup.offset);
    if (before->mayBePrototype() || before->hasKeyOrderOfOneObject() || storageChange == OutOfLineStorageChange::Unsupported)
        return PropertyAnswer::uncacheable(access);
    PropertyAnswer answer = PropertyAnswer::own(PropertyAnswerKind::ExtraPropertyAddition, access, lookup.offset);
    answer.newStructure = object.structure();
    if (storageChange != OutOfLineStorageChange::Stays)
        answer.newStorageCapacity = extraPropertiesAfter->outOfLineCapacity();
    answer.storageHasIndexingHeader = storageChange == OutOfLineStorageChange::GrowsBesideIndexingHeader;
    if (access == SlotAccess::Define)
        return answer;
    ChainWalk walk = walkChain({ query.globalObject, query.structureBefore, &object, ChainWalkMode::MayAllocate, ChainWalkUse::KeptForStores }, query.uid);
    if (storeThroughChain(walk) != StoreThroughChain::CreatesOwnProperty)
        return PropertyAnswer::uncacheableAfter(access, WTF::move(walk));
    answer.changeableObjects = WTF::move(walk.changeableObjects);
    return answer;
}

PropertyAnswer classifyCompiledObjectStore(const StoreQuery& query, Object& object)
{
    VM& vm = query.globalObject->vm();
    SlotAccess access = accessFor(query.mode);
    if (isNeverCachedName(vm, query.uid) || !isCompiledObject(&object))
        return uncacheableOnce(access);
    if (const FieldData* field = object.fieldNamed(object.state(), query.uid))
        return classifyFieldStore(query, object, *field);
    ExtraPropertyLookup lookup = lookUpExtraProperty(vm, object.compiledStructure(), query.uid, ChainWalkMode::MayAllocate);
    if (lookup.result != ExtraPropertyLookupResult::Found)
        return uncacheableOnce(access);
    return object.structure() == query.structureBefore ? classifyExtraPropertyReplacement(access, lookup) : classifyExtraPropertyAddition(query, object, lookup);
}

// The order word says which fields an object has, that none of them lies outside its slot, and that the object is neither sealed nor frozen.
static PropertyAnswer classifyFieldDeletion(const DeletionQuery& query, const Object& object, const CompiledStructure& before, const FieldData& field)
{
    constexpr SlotAccess access = SlotAccess::Delete;
    if (before.mayBePrototype() || before.hasOutOfLineKeyOrder() || field.isOutOfLine() || field.isPrivate() || !absentPatternOf(field.slot.representation()) || field.slot.offset > UINT16_MAX)
        return PropertyAnswer::uncacheable(access);
    if (object.structure() != &before || query.orderWordBefore & (orderWordIntegrityBits | orderWordHasFieldOutsideSlot))
        return uncacheableOnce(access);
    PropertyAnswer answer { .kind = PropertyAnswerKind::FieldRemoval, .access = access, .isOwn = true, .field = &field };
    answer.orderWords = std::pair { query.orderWordBefore, object.orderWord() };
    return answer;
}

static PropertyAnswer classifyCompiledObjectDeletion(const DeletionQuery& query)
{
    constexpr SlotAccess access = SlotAccess::Delete;
    VM& vm = query.globalObject->vm();
    const CompiledStructure* before = compiledObjectStructureOrNull(query.structureBefore);
    if (!before || !isCompiledObject(query.base))
        return PropertyAnswer::uncacheable(access);
    Object& object = *uncheckedDowncast<Object>(query.base);
    if (const FieldData* field = object.fieldNamed(object.state(), query.uid))
        return classifyFieldDeletion(query, object, *before, *field);
    ExtraPropertyLookup lookup = lookUpExtraProperty(vm, *before, query.uid, ChainWalkMode::MayAllocate);
    if (lookup.result == ExtraPropertyLookupResult::Absent && object.structure() == before)
        return PropertyAnswer::own(PropertyAnswerKind::NothingToRemove, access);
    const CompiledStructure& after = object.compiledStructure();
    if (lookup.result != ExtraPropertyLookupResult::Found || &after.layout() != &before->layout() || after.storedPrototype() != before->storedPrototype())
        return uncacheableOnce(access);
    if (before->mayBePrototype() || before->hasOutOfLineKeyOrder() || after.hasOutOfLineKeyOrder() || !before->extraPropertiesSlot() || !before->listsEveryExtraProperty())
        return PropertyAnswer::uncacheable(access);
    if (object.orderWord() != query.orderWordBefore || object.keyOrderCountsExtraKeys())
        return uncacheableOnce(access);
    Structure* extraPropertiesAfter = after.extraPropertiesStructure();
    PropertyOffset offset = invalidOffset;
    if (!extraPropertiesAfter || extraPropertiesAfter->isDictionary() || Structure::removePropertyTransitionFromExistingStructure(before->extraPropertiesStructure(), query.uid, offset) != extraPropertiesAfter || offset != lookup.offset)
        return uncacheableOnce(access);
    PropertyAnswer answer = PropertyAnswer::own(PropertyAnswerKind::ExtraPropertyRemoval, access, offset);
    answer.newStructure = object.structure();
    answer.orderWords = std::pair { query.orderWordBefore, query.orderWordBefore };
    return answer;
}

PropertyAnswer classifyDeletion(const DeletionQuery& query)
{
    constexpr SlotAccess access = SlotAccess::Delete;
    VM& vm = query.globalObject->vm();
    Structure* before = query.structureBefore;
    if (isNeverCachedName(vm, query.uid))
        return uncacheableOnce(access);
    if (vm.aotState().hasSeveralImages && belongsToOtherImageSlow(vm, query.base)) [[unlikely]]
        return PropertyAnswer::uncacheable(access);
    if (before->isCompiledStructure())
        return classifyCompiledObjectDeletion(query);
    MethodTable::DeletePropertyFunctionPtr ordinaryDelete = JSObject::deleteProperty;
    if (!query.slot.isCacheableDelete() || before->classInfoForCells()->methodTable.deleteProperty != ordinaryDelete || !before->propertyAccessesAreCacheable()
        || before->typeInfo().prohibitsPropertyCaching() || before->needImpurePropertyWatchpoint() || before->isDictionary())
        return PropertyAnswer::uncacheable(access);
    Structure* after = query.base->structure();
    if (query.slot.isConfigurableDeleteMiss()) {
        if (after != before || !structureProvesOwnAbsence(vm, query.base, before, query.uid, ChainWalkMode::MayAllocate))
            return uncacheableOnce(access);
        return PropertyAnswer::own(PropertyAnswerKind::NothingToRemove, access);
    }
    if (!query.slot.isDeleteHit() || before->mayBePrototype())
        return PropertyAnswer::uncacheable(access);
    PropertyOffset offset = invalidOffset;
    if (Structure::removePropertyTransitionFromExistingStructure(before, query.uid, offset) != after || offset != query.slot.cachedOffset())
        return uncacheableOnce(access);
    if (after->isDictionary() || !after->propertyAccessesAreCacheable() || after->typeInfoBlob() != before->typeInfoBlob())
        return PropertyAnswer::uncacheable(access);
    PropertyAnswer answer = PropertyAnswer::own(PropertyAnswerKind::Removal, access, offset);
    answer.newStructure = after;
    return answer;
}

// storeField(): the 32-bit integers that it puts into a slot of the form. Any other leaves the stub.
static std::pair<int64_t, int64_t> integersThatStubStores(const SlotTraits& traits)
{
    bool isSigned = traits.valueKind == SlotValueKind::SignedInteger;
    if (!isSigned && traits.valueKind != SlotValueKind::UnsignedInteger)
        return { INT32_MIN, INT32_MAX };
    switch (traits.width) {
    case 1:
        return isSigned ? std::pair<int64_t, int64_t> { INT8_MIN, INT8_MAX } : std::pair<int64_t, int64_t> { 0, UINT8_MAX };
    case 2:
        return isSigned ? std::pair<int64_t, int64_t> { INT16_MIN, INT16_MAX } : std::pair<int64_t, int64_t> { 0, UINT16_MAX };
    default:
        return { !isSigned ? 0 : traits.width == sizeof(int32_t) && traits.hasAbsent ? minInlineInt32 : INT32_MIN, INT32_MAX };
    }
}

static uint32_t bitsOfTypeForOtherImages(const RuntimeType& type, SlotRepresentation representation)
{
    static_assert(Typed::Primitive::All <= UINT16_MAX);
    uint32_t primitives = type.primitives & Typed::Primitive::All;
    if (auto range = type.integerRange()) {
        auto [least, most] = integersThatStubStores(traitsOf(representation));
        primitives &= ~Typed::Primitive::Double;
        if (range->first > least || range->second < most)
            primitives &= ~Typed::Primitive::Integer;
    }
    return Slot::typeIsGivenByBits | primitives << Slot::shiftOfPrimitivesInType | (type.acceptsEveryObject() ? Slot::typeAcceptsEveryObject : 0);
}

// The stub tests a value against a type in the tables of the image that runs, which do not have the types of another image. A store to a slot that is filled asks nothing of the chain,
// and one to an empty slot leaves the stub. Text and closures are put into a slot through the tables of their image.
static PropertyAnswer classifyStoreToObjectOfOtherImage(const StoreQuery& query)
{
    SlotAccess access = accessFor(query.mode);
    if (access != SlotAccess::Store || !isCompiledObject(query.base) || query.base->structure() != query.structureBefore || isNeverCachedName(query.globalObject->vm(), query.uid))
        return PropertyAnswer::uncacheable(access);
    Object& object = *uncheckedDowncast<Object>(query.base);
    const FieldData* field = object.fieldNamed(object.state(), query.uid);
    // Where an extra property lies and that it takes a store says the Structure of the object, which fixes that of its extra properties. A field that has left its slot is one of them.
    if (!field || field->canLeaveSlot()) {
        ExtraPropertyLookup lookup = lookUpExtraProperty(query.globalObject->vm(), object.compiledStructure(), query.uid, ChainWalkMode::MayAllocate);
        if (lookup.result == ExtraPropertyLookupResult::Found) {
            PropertyAnswer answer = classifyExtraPropertyReplacement(access, lookup);
            return answer.kind == PropertyAnswerKind::ExtraProperty ? answer : PropertyAnswer::uncacheable(access);
        }
    }
    // `theirs.size = 1` where a class above has `set size(v)`. The image of the object says what the store meets above it. A setter in the storage of its holder is called by any stub.
    if (!field && query.slot.isCacheableSetter()) {
        VM& vm = query.globalObject->vm();
        PropertyAnswer answer = [&] {
            RunningImageScope imageOfObject(vm.aotState(), object.instance().program);
            return classifyStore(query);
        }();
        return answer.kind == PropertyAnswerKind::Accessor || !answer.isCacheable() ? answer : PropertyAnswer::uncacheable(access);
    }
    if (!field || field->isAccessor() || field->isPrivate() || field->flags & (fieldIsStoredOnce | fieldAttributeFlags))
        return PropertyAnswer::uncacheable(access);
    SlotValueKind valueKind = traitsOf(field->slot.representation()).valueKind;
    if (valueKind == SlotValueKind::LiteralString || valueKind == SlotValueKind::DeferredClosure)
        return PropertyAnswer::uncacheable(access);
    return { .kind = PropertyAnswerKind::Field, .access = access, .isOwn = true, .bitsOfType = bitsOfTypeForOtherImages(object.state().objects->layouts.typeOf(*field), field->slot.representation()), .field = field };
}

PropertyAnswer classifyStore(const StoreQuery& query)
{
    VM& vm = query.globalObject->vm();
    SlotAccess access = accessFor(query.mode);
    Structure* structureBefore = query.structureBefore;
    if (vm.aotState().hasSeveralImages && belongsToOtherImageSlow(vm, query.base)) [[unlikely]]
        return classifyStoreToObjectOfOtherImage(query);
    bool isCompiled = structureBefore->isCompiledStructure();
    bool isNeverCached = isNeverCachedName(vm, query.uid) || isProxyType(structureBefore->typeInfo().type()) || structureBefore->needImpurePropertyWatchpoint()
        || (!isCompiled && (!structureBefore->propertyAccessesAreCacheable() || structureBefore->typeInfo().prohibitsPropertyCaching() || (access == SlotAccess::Store && isHostObjectOverridingPut(structureBefore))));
    if (structureBefore->isDictionary() && !isCompiled) {
        if (auto answer = isNeverCached ? std::nullopt : classifyReplacement(query))
            return WTF::move(*answer);
        return PropertyAnswer::uncacheable(access, flattenOnce(vm, query.base));
    }
    if (access == SlotAccess::Store && structureBefore->typeInfo().type() == ProxyObjectType && !isNeverCachedName(vm, query.uid) && !PropertyName(query.uid).isPrivateName())
        return PropertyAnswer::handlerOfProxyAnswers(access, State::of(query.globalObject).propertyCaches->keyForTraps(query.uid));
    if (isNeverCached)
        return PropertyAnswer::uncacheable(access);
    if (access == SlotAccess::Store && isCompiledClosure(query.base)) {
        if (const FieldData* field = StaticFields::findFieldForCache(vm, query.base, query.uid)) {
            if (query.base->structure() != structureBefore)
                return uncacheableOnce(access);
            return { .kind = PropertyAnswerKind::Variable, .access = access, .isOwn = true, .field = field, .variable = field->slot };
        }
    }
    if (isCompiled) {
        PropertyAnswer answer = query.base->structure()->isCompiledStructure() ? classifyCompiledObjectStore(query, *uncheckedDowncast<Object>(query.base)) : uncacheableOnce(access);
        if (!answer.isCacheable() && Object::structureGuaranteesAbsence(structureBefore, query.uid)) {
            if (auto setterCall = classifySetterCall(query))
                return WTF::move(*setterCall);
        }
        return answer;
    }
    if (access == SlotAccess::Define && !hasOrdinaryDefineForName(vm, query.base, structureBefore, query.uid))
        return PropertyAnswer::uncacheable(access);
    if (access == SlotAccess::Store && hasCacheableArrayLength(vm, query.base, structureBefore, query.uid) && !isCopyOnWrite(structureBefore->indexingMode()))
        return PropertyAnswer::own(PropertyAnswerKind::ArrayLength, access);
    for (auto* rule : { classifyReplacement, classifyAddition, classifySetterCall }) {
        if (auto answer = rule(query))
            return WTF::move(*answer);
    }
    if (access == SlotAccess::Store && query.slot.isCustomAccessor() && query.slot.isCacheableCustom())
        return classifyCustomSetter(query);
    return classifyStoreDecidedByChain(query).value_or(PropertyAnswer::uncacheable(access));
}

static Slot uncacheableSlot(StructureID id, uint8_t flags)
{
    return Slot::createWithoutPayload(id, Slot::Kind::Uncacheable, flags & Slot::dependsOnChain);
}

static const CompiledStructure& compiledStructureOf(Structure* structure)
{
    RELEASE_ASSERT(structure);
    const CompiledStructure* compiled = compiledObjectStructureOrNull(structure);
    RELEASE_ASSERT(compiled);
    return *compiled;
}

static std::optional<uint16_t> locationOfHiddenWord(std::optional<SlotData> hiddenWord)
{
    uint32_t offset = hiddenWord ? hiddenWord->offset : 0;
    return offset > UINT16_MAX ? std::nullopt : std::optional { static_cast<uint16_t>(offset) };
}

static bool canRememberOrder(const PropertyAnswer& answer)
{
    if (!answer.orderWords || answer.access == SlotAccess::Read)
        return false;
    auto [before, after] = *answer.orderWords;
    static_assert(orderWordOrderMask == (1u << orderWordCountShift) - 1 && orderWordOrderMask <= UINT16_MAX);
    return orderOf(before) != orderStoredOutOfLine && orderOf(after) != orderStoredOutOfLine && !(before & (orderWordIntegrityBits | orderWordHasFieldOutsideSlot))
        && (after & ~orderWordOrderMask) == ((before + (1u << orderWordCountShift)) & ~orderWordOrderMask);
}

static Slot slotForField(const PropertyAnswer& answer, Structure* receiverStructure, Slot::Kind kind, uint8_t flags)
{
    RELEASE_ASSERT(answer.field);
    const FieldData& field = *answer.field;
    std::optional<SlotData> extensionWord = compiledStructureOf(receiverStructure).extensionSlot();
    RELEASE_ASSERT(extensionWord || !field.isOutOfLine());
    StructureID id = receiverStructure->id();
    std::optional<uint16_t> extensionWordOffset = locationOfHiddenWord(extensionWord);
    if (!extensionWordOffset)
        return uncacheableSlot(id, flags);
    if (field.isOutOfLine())
        flags |= Slot::isOutOfLine;
    if (answer.absentIsUndefined)
        flags |= Slot::absentIsUndefined;
    if (answer.kind == PropertyAnswerKind::Field && answer.emptySlotMayBeUndefined)
        flags |= Slot::emptySlotMayBeUndefined;
    if (kind == Slot::Kind::FieldCreation && answer.undefinedNeedsNoStorage)
        flags |= Slot::undefinedNeedsNoStorage;
    if (kind == Slot::Kind::FieldCreation && canRememberOrder(answer))
        return Slot::createField(id, kind, flags | Slot::remembersOrder, field.slot, answer.orderWords->first, static_cast<uint16_t>(answer.orderWords->second & orderWordOrderMask));
    uint32_t type = answer.bitsOfType ? answer.bitsOfType : field.type;
    return Slot::createField(id, kind, flags, field.slot, answer.access == SlotAccess::Read ? 0 : type, *extensionWordOffset);
}

static Slot slotForFieldOrInherited(const PropertyAnswer& answer, Structure* receiverStructure, uint8_t flags)
{
    RELEASE_ASSERT(answer.field && answer.holder);
    bool isRead = answer.access == SlotAccess::Read;
    uintptr_t cageBase = receiverStructure->vm().heap.cage().base();
    uint8_t flagsOfKind = answer.field->isOutOfLine() ? flags | Slot::isOutOfLine : flags;
    std::optional<Slot> slot;
    if (answer.kind == PropertyAnswerKind::FieldOrInheritedField) {
        RELEASE_ASSERT(isRead);
        if (std::optional<uint16_t> extensionWordOffset = locationOfHiddenWord(compiledStructureOf(receiverStructure).extensionSlot()))
            slot = Slot::tryCreateFieldOrInheritedField(receiverStructure->id(), flagsOfKind, answer.field->slot, answer.holder, *extensionWordOffset, cageBase);
    } else {
        if (isRead && answer.kind == PropertyAnswerKind::FieldOrInheritedAccessor)
            flagsOfKind |= Slot::inheritsGetter;
        if (answer.emptySlotMayBeUndefined)
            flagsOfKind |= Slot::emptySlotMayBeUndefined;
        slot = Slot::tryCreateFieldAndHolder(receiverStructure->id(), isRead ? Slot::Kind::FieldOrInherited : Slot::Kind::FieldOrInheritedSetter, flagsOfKind, answer.field->slot, answer.holder, answer.offset, cageBase);
    }
    return slot ? *slot : slotForField(answer, receiverStructure, Slot::Kind::Field, flags);
}

static Slot slotForExtraProperty(const PropertyAnswer& answer, StructureID id, Structure* structureWithProperty, Slot::Kind kind, uint8_t flags)
{
    const CompiledStructure& structure = compiledStructureOf(structureWithProperty);
    RELEASE_ASSERT(structure.extraPropertiesSlot() && structure.extraPropertiesStructure());
    std::optional<uint16_t> extraPropertiesWordOffset = locationOfHiddenWord(structure.extraPropertiesSlot());
    if (!extraPropertiesWordOffset)
        return uncacheableSlot(id, flags);
    if (kind != Slot::Kind::ExtraPropertyAddition)
        return Slot::createExtraProperty(id, kind, flags, StructureID(), answer.offset, *extraPropertiesWordOffset);
    if (answer.newStorageCapacity)
        flags |= Slot::growsStorage;
    if (answer.storageHasIndexingHeader)
        flags |= Slot::storageHasIndexingHeader;
    return Slot::createExtraPropertyAddition(id, flags, answer.newStructure->id(), answer.offset, *extraPropertiesWordOffset);
}

Slot slotFor(const PropertyAnswer& answer, Structure* receiverStructure)
{
    using Kind = Slot::Kind;
    StructureID id = receiverStructure ? receiverStructure->id() : StructureID();
    bool isRead = answer.access == SlotAccess::Read;
    uint8_t flags = (answer.isOwn ? Slot::isOwn : 0) | (answer.dependsOnChain() ? Slot::dependsOnChain : 0);
    switch (answer.kind) {
    case PropertyAnswerKind::Uncacheable:
        return answer.keyForTrap ? Slot::createForProxies(id, answer.keyForTrap, answer.columnOfNameForTrap) : uncacheableSlot(id, flags);
    case PropertyAnswerKind::Value:
        return Slot::createInStorage(id, Kind::Value, flags, answer.holder, answer.offset);
    case PropertyAnswerKind::Accessor:
        return Slot::createInStorage(id, isRead ? Kind::Getter : Kind::Setter, flags, answer.holder, answer.offset);
    case PropertyAnswerKind::CustomAccessor:
        return Slot::createCustomAccessor(id, isRead ? Kind::CustomGetter : Kind::CustomSetter, flags, answer.customAccessor);
    case PropertyAnswerKind::Field:
        return slotForField(answer, receiverStructure, Kind::Field, flags);
    case PropertyAnswerKind::FieldAccessor:
        RELEASE_ASSERT(isRead);
        return slotForField(answer, receiverStructure, Kind::FieldGetter, flags);
    case PropertyAnswerKind::FieldCreation:
        return slotForField(answer, receiverStructure, Kind::FieldCreation, flags);
    case PropertyAnswerKind::FieldOrInheritedValue:
    case PropertyAnswerKind::FieldOrInheritedAccessor:
    case PropertyAnswerKind::FieldOrInheritedField:
        return slotForFieldOrInherited(answer, receiverStructure, flags);
    case PropertyAnswerKind::InheritedField:
        return Slot::createInheritedField(id, flags, answer.holder, answer.wordOfInheritedField);
    case PropertyAnswerKind::Variable:
        RELEASE_ASSERT(isRead || answer.field);
        return Slot::createField(id, Kind::Variable, flags, answer.variable, isRead ? 0 : answer.field->type, answer.imageOfVariable);
    case PropertyAnswerKind::ExtraProperty:
        return slotForExtraProperty(answer, id, receiverStructure, Kind::ExtraProperty, flags);
    case PropertyAnswerKind::ExtraAccessor:
        return slotForExtraProperty(answer, id, receiverStructure, isRead ? Kind::ExtraGetter : Kind::ExtraSetter, flags);
    case PropertyAnswerKind::ExtraPropertyAddition:
        return slotForExtraProperty(answer, id, answer.newStructure, Kind::ExtraPropertyAddition, flags);
    case PropertyAnswerKind::Addition:
        if (answer.storageHasIndexingHeader)
            flags |= Slot::storageHasIndexingHeader;
        return Slot::tryCreateTransition(id, flags, answer.newStructure->id(), answer.offset, answer.newStorageCapacity).value_or(uncacheableSlot(id, flags));
    case PropertyAnswerKind::Removal:
        return Slot::tryCreateRemoval(id, Kind::Removal, flags, answer.newStructure->id(), answer.offset, 0).value_or(uncacheableSlot(id, flags));
    case PropertyAnswerKind::ExtraPropertyRemoval:
        return Slot::tryCreateRemoval(id, Kind::ExtraPropertyRemoval, flags, answer.newStructure->id(), answer.offset, answer.orderWords->first).value_or(uncacheableSlot(id, flags));
    case PropertyAnswerKind::FieldRemoval:
        if (answer.field->isFilledAtBirth)
            flags |= Slot::leavesStateOfBirth;
        return Slot::createFieldRemoval(id, flags, *absentPatternOf(answer.field->slot.representation()), static_cast<uint16_t>(answer.field->slot.offset), answer.orderWords->first, answer.orderWords->second);
    case PropertyAnswerKind::NothingToRemove:
        return Slot::createWithoutPayload(id, Kind::NothingToRemove, flags);
    case PropertyAnswerKind::Absent:
        return Slot::createWithoutPayload(id, Kind::Absent, flags);
    case PropertyAnswerKind::ArrayLength:
        return Slot::createWithoutPayload(id, Kind::ArrayLength, flags);
    case PropertyAnswerKind::TypedArrayLength:
        return Slot::createWithoutPayload(id, Kind::TypedArrayLength, answer.readsByteOffset ? flags | Slot::readsByteOffset : flags);
    case PropertyAnswerKind::StringLength:
        return Slot::createWithoutPayload(id, Kind::StringLength, flags);
    case PropertyAnswerKind::ClosureLength:
        return Slot::createWithoutPayload(id, Kind::ClosureLength, flags);
    case PropertyAnswerKind::ClosureName:
        return Slot::createWithoutPayload(id, Kind::ClosureName, flags);
    case PropertyAnswerKind::Constant:
        return Slot::createConstant(id, flags, answer.constant);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
