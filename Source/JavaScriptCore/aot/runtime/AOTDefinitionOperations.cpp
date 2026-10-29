/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTClassPrototype.h"
#include "AOTClosure.h"
#include "AOTCompiledCallee.h"
#include "AOTCompiledStructure.h"
#include "AOTFunctionNames.h"
#include "AOTFunctionTables.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTObjectWords.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTPendingStars.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTSlimClosure.h"
#include "AOTProgramStrings.h"
#include "AOTState.h"
#include "ArrayConventions.h"
#include "Butterfly.h"
#include "ButterflyInlines.h"
#include "DeferTermination.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "JSPropertyNameEnumerator.h"
#include "ObjectConstructor.h"
#include "PropertyDescriptor.h"
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT {

static bool canSkipDefineOwnProperty(VM& vm, JSObject* object, Structure* structure, PropertyName name)
{
    if (structure->hasImmutableProperties() || structure->hasListedImmutableProperties())
        return false;
    if (hasOrdinaryDefineForName(vm, object, structure, name.uid()))
        return true;
    if (object->type() == SlimClosureWithPropertiesType)
        return structure->isStructureExtensible() && !parseIndex(name) && ReifiedSlimClosure::hasOrdinaryDefineOwnProperty(vm, name);
    auto* closure = dynamicDowncast<Closure>(object);
    return closure && structure->isStructureExtensible() && !parseIndex(name) && closure->hasOrdinaryDefineOwnProperty(vm, name);
}

void defineDataProperty(Instance* instance, JSObject* object, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = *instance->vm;
    if (Object* compiled = compiledObjectOrNull(object)) {
        if (compiled->tryStoreExtraProperty(vm, name, value, { Object::ExtraPropertyStoreFlag::IsDefinition }).kind != Object::ExtraPropertyStore::NotHandled)
            return;
    }
    Structure* structure = object->structure();
    unsigned attributes = 0;
    if (canSkipDefineOwnProperty(vm, object, structure, name) && (!isValidOffset(structure->get(vm, name, attributes)) || !attributes))
        object->putDirect(vm, name, value, slot);
    else
        object->createDataProperty(instance->globalObject, name, value, true);
}

// These are the arrays for which JSArray::defineOwnProperty() comes down to putDirectIndex(): a length that is read-only puts the array into sparse mode. The text of the index is never made.
static bool canDefineElementsDirectly(JSObject* object)
{
    return (isJSArray(object) || object->type() == FinalObjectType) && !object->structure()->hasImmutableIndexedProperties() && !object->inSparseIndexingMode() && !isCopyOnWrite(object->indexingMode());
}

// What the stub StoreElementOfCompiledObject makes, out of the same allocators: where one of them has run out, this fills it again.
static JSObject* createExtraPropertiesWithElement(Instance* instance, uint32_t index, JSValue value)
{
    VM& vm = *instance->vm;
    uint32_t length = index + 1;
    unsigned vectorLength = Butterfly::optimalContiguousVectorLength(static_cast<size_t>(0), std::max<unsigned>(length + length / 2, BASE_CONTIGUOUS_VECTOR_LEN));
    Butterfly* butterfly = Butterfly::create(vm, nullptr, 0, 0, true, IndexingHeader(), vectorLength * sizeof(EncodedJSValue));
    butterfly->setVectorLength(vectorLength);
    butterfly->setPublicLength(length);
    for (unsigned i = 0; i < vectorLength; ++i)
        butterfly->contiguous().atUnsafe(i).clear();
    butterfly->contiguous().atUnsafe(index).setWithoutWriteBarrier(value);
    return JSFinalObject::createWithButterfly(vm, instance->realm.extraPropertiesWithElementsStructure, butterfly);
}

// The stub gives the next object with the Structure `before` its first element. It stores as it defines, so right above the object is the Object.prototype of its realm, which has no elements and takes none.
static void rememberStructureWithFirstElement(Instance* instance, const CompiledStructure& before, const CompiledStructure& after, JSObject& extra)
{
    Structure* withElements = instance->realm.extraPropertiesWithElementsStructure;
    if (extra.structure() != withElements || instance->vm->aotState().validatesRuntime)
        return;
    if (!before.extraPropertiesSlot() || before.hasOtherPrototype() || before.storedPrototype() != JSValue(instance->realm.objectPrototype) || before.typeInfoBlob() != after.typeInfoBlob())
        return;
    EncodedJSValue inPlaceOfPrototype = std::bit_cast<EncodedJSValue>(withElements);
    std::span { instance->objects.prototypeTransitions, numberOfPrototypeTransitions }[indexOfPrototypeTransition(before.id().bits(), inPlaceOfPrototype)] = { inPlaceOfPrototype, before.id().bits(), after.id().bits() };
}

bool tryDefineElementOfCompiledObject(Instance* instance, JSObject* object, uint32_t index, JSValue value)
{
    Object* compiled = compiledObjectOrNull(object);
    if (!compiled || compiled->isNotExtensible() || compiled->structure()->mayBePrototype() || compiled->layout().isClass())
        return false;
    State& state = *instance->state;
    if (!state.tables.addedNames().empty())
        return false;
    VM& vm = *instance->vm;
    JSObject* extra = compiled->extraProperties();
    if (extra && (!is<JSFinalObject>(extra) || extra->inSparseIndexingMode() || isCopyOnWrite(extra->indexingMode()) || extra->structure()->hasImmutableIndexedProperties()))
        return false;
    if (checksClaimsAboutObjects(vm) && !(extra && extra->hasOwnProperty(instance->globalObject, index)))
        validateMutation(vm, *compiled, ObjectMutation::AddProperty);
    if (!extra) {
        const CompiledStructure* before = WTF::opaque(&compiled->compiledStructure());
        if (index <= maxIndexOfFirstElementMadeByStub)
            extra = createExtraPropertiesWithElement(instance, index, value);
        else {
            extra = constructEmptyObject(vm, instance->realm.emptyExtraPropertiesStructure);
            extra->putDirectIndex(instance->globalObject, index, value, 0, PutDirectIndexShouldThrow);
        }
        compiled->adoptExtraProperties(vm, extra);
        rememberStructureWithFirstElement(instance, *before, compiled->compiledStructure(), *extra);
        ensureStillAliveHere(before);
        return true;
    }
    extra->putDirectIndex(instance->globalObject, index, value, 0, PutDirectIndexShouldThrow);
    compiled->didChangeExtraProperties(vm);
    return true;
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineById, void, (Instance* instance, EncodedJSValue encodedObject, uint32_t name, EncodedJSValue value))
{
    constexpr Entry entry = Entry::operationAOTDefineById;
    AOT_OPERATION_BEGIN(instance, entry);
    JSObject* object = objectArgument(encodedObject, entry);
    if (Options::aotCountsPath()) [[unlikely]]
        instance->state->propertyCaches->countMiss("definition without caches"_s, false, object, identifierAt(instance, name).impl());
    const Identifier& identifier = identifierAt(instance, name);
    if (std::optional<uint32_t> index = parseIndex(identifier); index && tryDefineElementOfCompiledObject(instance, object, *index, JSValue::decode(value)))
        OPERATION_RETURN(scope);
    PutPropertySlot slot(object, true);
    defineDataProperty(instance, object, identifier, JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

// `{ id, [key]: value }`. As for a store by key: the second object that gets the same name behind the same names has the Structure of its extra properties named, and the stub adds the property to the third.
static bool tryDefineExtraPropertyAndCache(Instance* instance, JSObject* object, PropertyName name, JSValue value, const PutPropertySlot& slot)
{
    using Flag = Object::ExtraPropertyStoreFlag;
    VM& vm = *instance->vm;
    Object* compiled = compiledObjectOrNull(object);
    if (!compiled || compiled->fieldNamed(compiled->state(), name) || !compiled->repeatsExtraPropertyStore(vm, name))
        return false;
    compiled->ensureStructureForExtraProperties(vm);
    Structure* structureBefore = WTF::opaque(compiled->structure());
    if (compiled->tryStoreExtraProperty(vm, name, value, { Flag::IsKnownNotToBeField, Flag::IsDefinition, Flag::NamesExtraPropertiesStructure }).kind == Object::ExtraPropertyStore::NotHandled)
        return false;
    instance->state->propertyCaches->cache({ structureBefore, { name.uid(), std::nullopt }, SlotAccess::Define }, classifyStore({ instance->globalObject, object, structureBefore, name.uid(), slot, PutMode::Define }));
    ensureStillAliveHere(structureBefore);
    return true;
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineField, void, (Instance* instance, EncodedJSValue encodedObject, EncodedJSValue encodedKey, EncodedJSValue value))
{
    constexpr Entry entry = Entry::operationAOTDefineField;
    AOT_OPERATION_BEGIN(instance, entry);
    if (!JSValue::decode(encodedObject).isObject()) [[unlikely]] {
        throwException(globalObject, scope, JSC::createNotAnObjectError(globalObject, JSValue::decode(encodedObject)));
        OPERATION_RETURN(scope);
    }
    JSObject* object = objectArgument(encodedObject, entry);
    JSValue key = JSValue::decode(encodedKey);
    if (std::optional<uint32_t> index = key.tryGetAsUint32Index(); index && canDefineElementsDirectly(object)) {
        object->putDirectIndex(globalObject, *index, JSValue::decode(value), 0, PutDirectIndexShouldThrow);
        OPERATION_RETURN(scope);
    }
    if (std::optional<uint32_t> index = key.tryGetAsUint32Index(); index && tryDefineElementOfCompiledObject(instance, object, *index, JSValue::decode(value)))
        OPERATION_RETURN(scope);
    bool isKeyUniqued = key.isSymbol() || atomOfKey(key);
    auto name = key.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (std::optional<uint32_t> index = parseIndex(name)) {
        if (canDefineElementsDirectly(object)) {
            object->putDirectIndex(globalObject, *index, JSValue::decode(value), 0, PutDirectIndexShouldThrow);
            OPERATION_RETURN(scope);
        }
        if (tryDefineElementOfCompiledObject(instance, object, *index, JSValue::decode(value)))
            OPERATION_RETURN(scope);
    }
    Structure* structureBefore = WTF::opaque(object->structure());
    bool files = isKeyUniqued && !parseIndex(name) && instance->state->propertyCaches->shouldFile(structureBefore->id(), name.impl(), SlotAccess::Define);
    if (instance->state->propertyCaches->countsNames()) [[unlikely]]
        instance->state->propertyCaches->countMiss("definition by key"_s, files, object, name.impl());
    PutPropertySlot slot(object, true);
    if (files && tryDefineExtraPropertyAndCache(instance, object, name, JSValue::decode(value), slot))
        OPERATION_RETURN(scope);
    defineDataProperty(instance, object, name, JSValue::decode(value), slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (files)
        instance->state->propertyCaches->cache({ structureBefore, { name.impl(), std::nullopt }, SlotAccess::Define }, classifyStore({ globalObject, object, structureBefore, name.impl(), slot, PutMode::Define }));
    else if (isKeyUniqued && slot.type() == PutPropertySlot::NewProperty)
        instance->state->propertyCaches->didAddProperty(structureBefore, object->structure());
    ensureStillAliveHere(structureBefore);
    OPERATION_RETURN(scope);
}

// Every function is created with a name that is configurable, read-only and not enumerable. A descriptor that sets no other attribute only changes the value: `{ value }`, `{ value, configurable: true }`.
static bool tryDefineNameOfFunction(JSGlobalObject* globalObject, JSObject* function, PropertyName name, JSValue value, PropertyDescriptorBits bits)
{
    VM& vm = globalObject->vm();
    if (name != vm.propertyNames->name || !bits.hasValue || !value.isString())
        return false;
    if ((bits.hasWritable && bits.isWritable) || (bits.hasEnumerable && bits.isEnumerable) || (bits.hasConfigurable && !bits.isConfigurable))
        return false;
    constexpr unsigned attributesAtBirth = PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum;
    if (auto* closure = dynamicDowncast<Closure>(function)) {
        FunctionRareData* rareData = closure->rareData();
        Structure* structure = closure->structure();
        if ((rareData && rareData->hasReifiedName()) || !structure->isStructureExtensible() || structure->mayHaveImmutableProperties() || closure->getDirect(vm, name))
            return false;
        if (const LayoutData* classLayout = closure->classData()) {
            // The fields of a class become properties in front of whatever is defined by name.
            ClassTable& classes = closure->state().objects->classes;
            if (!classLayout->staticFields().empty() || classes.hasStaticMethodNamed(*classLayout, name.uid()))
                return false;
            closure->setNameProperty(vm, asString(value), attributesAtBirth);
            classes.didChangeMember(*classLayout, ClassSide::Static, name.uid());
        } else {
            FunctionKindData kind = closure->kind();
            if (kind.isBound() || kind.hasInitialNamePrefix() || kind.notesLooksForName() || closure->refusesOwnProperty(vm, name))
                return false;
            auto defined = asString(value)->tryGetValue();
            if (defined->isNull())
                return false;
            didLookForNameProperty(vm, closure);
            // A closure that is told the name it has stays as it is.
            if (defined.data == initialNameProperty(closure->function()))
                return true;
            closure->setNameProperty(vm, asString(value), attributesAtBirth);
        }
    } else {
        if (function->type() == SlimClosureType)
            uncheckedDowncast<SlimClosure>(function)->reify(vm);
        else if (function->type() != SlimClosureWithPropertiesType)
            return false;
        Structure* structure = function->structure();
        unsigned attributes = 0;
        PropertyOffset offset = structure->get(vm, name, attributes);
        if (!isValidOffset(offset) || attributes != attributesAtBirth || structure->mayHaveImmutableProperties())
            return false;
        structure->didReplaceProperty(offset);
        function->putDirectOffset(vm, offset, value);
    }
    auto scope = DECLARE_THROW_SCOPE(vm);
    String reported = asString(value)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, true);
    didDefineNameProperty(vm, function, reported);
    return true;
}

static bool tryDefineAbsentProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue valueOrGetter, JSValue setter, PropertyDescriptorBits bits)
{
    VM& vm = globalObject->vm();
    Object* compiled = compiledObjectOrNull(object);
    if (object->type() == SlimClosureType && !parseIndex(name) && ReifiedSlimClosure::hasOrdinaryDefineOwnProperty(vm, name))
        uncheckedDowncast<SlimClosure>(object)->reify(vm);
    Structure* structure = object->structure();
    if (!compiled && !canSkipDefineOwnProperty(vm, object, structure, name))
        return false;
    bool isAccessor = bits.hasGet || bits.hasSet;
    unsigned attributes = (bits.isEnumerable ? PropertyAttribute::None : PropertyAttribute::DontEnum) | (bits.isConfigurable ? PropertyAttribute::None : PropertyAttribute::DontDelete);
    if (isAccessor)
        attributes |= PropertyAttribute::Accessor;
    else if (!bits.isWritable)
        attributes |= PropertyAttribute::ReadOnly;
    const FieldData* field = compiled ? compiled->fieldNamed(compiled->state(), name) : nullptr;
    // The layout has a field for a getter that the build saw defined, with the attributes it saw.
    if (field && field->isAccessor() && bits.hasGet && !bits.hasSet && valueOrGetter.isObject() && compiled->tryDefineAbsentGetterInSlot(vm, globalObject, *field, name.uid(), asObject(valueOrGetter), attributes))
        return true;
    if (field && compiled->isInSlot(*field))
        return false;
    JSValue stored = bits.hasValue ? valueOrGetter : jsUndefined();
    if (isAccessor)
        stored = GetterSetter::create(vm, globalObject, bits.hasGet ? valueOrGetter.getObject() : nullptr, setter.getObject());
    if (compiled)
        return compiled->tryDefineAbsentProperty(vm, field, name, stored, attributes);
    if (object->tryPutDirectByExistingTransition(vm, name, stored, attributes))
        return true;
    if (isValidOffset(structure->get(vm, name)))
        return false;
    if (!isAccessor) {
        object->putDirect(vm, name, stored, attributes);
        return true;
    }
    return object->putDirectNonIndexAccessor(vm, name, uncheckedDowncast<GetterSetter>(stored), attributes);
}

static ALWAYS_INLINE PropertyDescriptor descriptorFromBits(JSValue valueOrGetter, JSValue setter, PropertyDescriptorBits bits)
{
    PropertyDescriptor descriptor;
    if (bits.hasEnumerable)
        descriptor.setEnumerable(bits.isEnumerable);
    if (bits.hasConfigurable)
        descriptor.setConfigurable(bits.isConfigurable);
    if (bits.hasValue)
        descriptor.setValue(valueOrGetter);
    if (bits.hasWritable)
        descriptor.setWritable(bits.isWritable);
    if (bits.hasGet)
        descriptor.setGetter(valueOrGetter);
    if (bits.hasSet)
        descriptor.setSetter(setter);
    return descriptor;
}

// While this holds, a descriptor object whose prototype is Object.prototype has the members that it lists itself and no others.
static bool descriptorObjectsInheritNoMembers(JSGlobalObject* globalObject)
{
    if (globalObject->propertyDescriptorFastPathWatchpointSet().state() == ClearWatchpoint)
        globalObject->tryInstallPropertyDescriptorFastPathWatchpoint();
    return globalObject->propertyDescriptorFastPathWatchpointSet().isStillValid() && globalObject->objectPrototypeChainIsSane();
}

// These run nobody's code when a property is defined. One that refuses is as it was, but for a list that is cut short, which refuses again where it stopped.
static bool definesWithoutOtherCode(VM& vm, JSObject* object, PropertyName name, JSValue value, PropertyDescriptorBits bits)
{
    auto define = object->methodTable()->defineOwnProperty;
    if (define == JSObject::defineOwnProperty || define == Object::info()->methodTable.defineOwnProperty)
        return true;
    if (define == JSArray::info()->methodTable.defineOwnProperty)
        return name != vm.propertyNames->length || !bits.hasValue || value.isUInt32();
    for (const ClassInfo* info : { Closure::info(), SlimClosure::info(), ReifiedSlimClosure::info(), ClassPrototype::info(), JSFunction::info() }) {
        if (define == info->methodTable.defineOwnProperty)
            return true;
    }
    return false;
}

static bool tryDefineOwnPropertyFromBits(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue valueOrGetter, JSValue setter, PropertyDescriptorBits bits)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    RELEASE_ASSERT(!(bits.hasValue || bits.hasWritable) || !(bits.hasGet || bits.hasSet));
    if (!descriptorObjectsInheritNoMembers(globalObject)) [[unlikely]]
        return false;
    bool isDefined = tryDefineNameOfFunction(globalObject, object, name, valueOrGetter, bits);
    RETURN_IF_EXCEPTION(scope, false);
    if (isDefined)
        return true;
    // A closure of the program says by its type that it can be called.
    auto isFunctionOrUndefined = [](JSValue value) {
        return value.isUndefined() || (value.isCell() && isCompiledCalleeType(value.asCell()->type())) || value.isCallable();
    };
    if (bits.hasGet && !isFunctionOrUndefined(valueOrGetter))
        return false;
    if (bits.hasSet && !isFunctionOrUndefined(setter))
        return false;
    if (tryDefineAbsentProperty(globalObject, object, name, valueOrGetter, setter, bits))
        return true;
    if (!definesWithoutOtherCode(vm, object, name, valueOrGetter, bits))
        return false;
    RELEASE_AND_RETURN(scope, object->methodTable()->defineOwnProperty(object, globalObject, name, descriptorFromBits(valueOrGetter, setter, bits), false));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineOwnProperty, void, (Instance* instance, EncodedJSValue encodedObject, EncodedJSValue key, EncodedJSValue encodedValueOrGetter, EncodedJSValue encodedSetter, uint32_t descriptorBits))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDefineOwnProperty);
    JSValue object = JSValue::decode(encodedObject);
    JSValue valueOrGetter = JSValue::decode(encodedValueOrGetter);
    JSValue setter = JSValue::decode(encodedSetter);
    auto bits = std::bit_cast<PropertyDescriptorBits>(descriptorBits);
    if (object.isObject()) [[likely]] {
        // The string keeps its atom alive.
        Identifier converted;
        StringImpl* text = JSValue::decode(key).isString() ? asString(JSValue::decode(key))->tryGetValueImpl() : nullptr;
        if (!text || !text->isAtom()) {
            converted = JSValue::decode(key).toPropertyKey(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope);
            text = converted.impl();
        }
        PropertyName name(static_cast<UniquedStringImpl*>(text));
        bool isDefined = tryDefineOwnPropertyFromBits(globalObject, asObject(object), name, valueOrGetter, setter, bits);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        if (isDefined) [[likely]]
            OPERATION_RETURN(scope);
    }
    // Object.defineProperty() itself refuses, and runs the code of others, in a frame of its own.
    JSObject* literal = constructObjectFromPropertyDescriptor(globalObject, descriptorFromBits(valueOrGetter, setter, bits));
    callInEngine(instance, Builtins::ObjectDefineProperty, object, JSValue::decode(key), literal);
    OPERATION_RETURN(scope);
}

static void defineAccessor(JSGlobalObject* globalObject, JSObject* object, PropertyName property, JSValue function, bool isEnumerable, bool isGetter)
{
    VM& vm = globalObject->vm();
    unsigned accessorAttributes = PropertyAttribute::Accessor | (isEnumerable ? PropertyAttribute::None : PropertyAttribute::DontEnum);
    if (Object* compiled = isGetter ? compiledObjectOrNull(object) : nullptr; compiled && compiled->tryDefineAbsentGetterInSlot(vm, globalObject, property.uid(), asObject(function), accessorAttributes))
        return;
    if (Structure* structure = object->structure(); canSkipDefineOwnProperty(vm, object, structure, property)) {
        unsigned attributes;
        PropertyOffset offset = structure->get(vm, property, attributes);
        JSObject* getter = isGetter ? asObject(function) : nullptr;
        JSObject* setter = isGetter ? nullptr : asObject(function);
        bool isOtherHalf = isValidOffset(offset) && attributes == accessorAttributes;
        if (isOtherHalf) {
            auto* current = uncheckedDowncast<GetterSetter>(object->getDirect(offset));
            if (isGetter)
                setter = current->isSetterNull() ? nullptr : current->setter();
            else
                getter = current->isGetterNull() ? nullptr : current->getter();
        }
        if ((isOtherHalf || !isValidOffset(offset)) && object->putDirectNonIndexAccessor(vm, property, GetterSetter::create(vm, globalObject, getter, setter), accessorAttributes))
            return;
    }
    PropertyDescriptor descriptor;
    if (isGetter)
        descriptor.setGetter(function);
    else
        descriptor.setSetter(function);
    descriptor.setEnumerable(isEnumerable);
    descriptor.setConfigurable(true);
    object->methodTable()->defineOwnProperty(object, globalObject, property, descriptor, true);
}

static void defineAccessor(JSGlobalObject* globalObject, JSObject* object, JSValue key, JSValue function, uint32_t accessorFlags)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (Object* compiled = compiledObjectOrNull(object)) {
        std::array<EncodedJSValue, 2> keyAndFunction { JSValue::encode(key), JSValue::encode(function) };
        if (compiled->tryDefineAccessorsOfLiteral(vm, globalObject, keyAndFunction, !!(accessorFlags & accessorIsGetter), !!(accessorFlags & accessorIsEnumerable)))
            return;
    }
    auto property = key.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    scope.release();
    defineAccessor(globalObject, object, property, function, accessorFlags & accessorIsEnumerable, accessorFlags & accessorIsGetter);
}

// Compiled code that finds no allocator comes here, and makes the next pair by itself.
static void prepareAllocationOfAccessors(Instance* instance)
{
    if (instance->allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::GetterSetter)].allocator) [[likely]]
        return;
    instance->allocation.getterOfAccessorWithoutGetter = instance->globalObject->nullGetterFunction();
    instance->allocation.setterOfAccessorWithoutSetter = instance->globalObject->nullSetterFunction();
    prepareInlineAllocation<GetterSetter>(instance, InlineAllocation::GetterSetter, instance->vm->getterSetterStructure.get());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewGetterSetter, JSCell*, (Instance* instance, JSCell* getter, JSCell* setter))
{
    constexpr Entry entry = Entry::operationAOTNewGetterSetter;
    AOT_OPERATION_BEGIN(instance, entry);
    prepareAllocationOfAccessors(instance);
    JSCell* accessor = GetterSetter::create(vm, globalObject, getter ? cellArgument<JSObject>(getter, entry) : nullptr, setter ? cellArgument<JSObject>(setter, entry) : nullptr);
    OPERATION_RETURN(scope, accessor);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineAccessor, void, (Instance* instance, EncodedJSValue object, EncodedJSValue key, EncodedJSValue function, uint32_t accessorFlags))
{
    constexpr Entry entry = Entry::operationAOTDefineAccessor;
    AOT_OPERATION_BEGIN(instance, entry);
    prepareAllocationOfAccessors(instance);
    defineAccessor(globalObject, objectArgument(object, entry), JSValue::decode(key), JSValue::decode(function), accessorFlags);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineAccessors, void, (Instance* instance, JSCell* cell, uint32_t count, uint32_t getters, uint32_t enumerable, const EncodedJSValue* keysAndFunctions))
{
    constexpr Entry entry = Entry::operationAOTDefineAccessors;
    constexpr uint32_t maskWidth = 32;
    constexpr size_t wordsPerAccessor = sizeof(DefinedAccessor) / sizeof(EncodedJSValue);
    AOT_OPERATION_BEGIN(instance, entry);
    prepareAllocationOfAccessors(instance);
    JSObject* object = cellArgument<JSObject>(cell, entry);
    if (count > maskWidth) [[unlikely]]
        crashOnWrongOperand(entry);
    auto accessors = spanReinterpretCast<const DefinedAccessor>(unsafeMakeSpan(keysAndFunctions, count * wordsPerAccessor));
    Object* compiledObject = compiledObjectOrNull(object);
    if (compiledObject && compiledObject->tryDefineAccessorsOfLiteral(vm, globalObject, unsafeMakeSpan(keysAndFunctions, count * wordsPerAccessor), getters, enumerable))
        OPERATION_RETURN(scope);
    for (uint32_t index = 0; index < count; ++index) {
        const DefinedAccessor& accessor = accessors[index];
        uint32_t accessorFlags = (getters >> index & 1 ? accessorIsGetter : 0) | (enumerable >> index & 1 ? accessorIsEnumerable : 0);
        if (UniquedStringImpl* name = compiledObject && accessorFlags & accessorIsGetter ? atomOfKey(JSValue::decode(accessor.key)) : nullptr) {
            unsigned attributes = PropertyAttribute::Accessor | (accessorFlags & accessorIsEnumerable ? PropertyAttribute::None : PropertyAttribute::DontEnum);
            if (compiledObject->tryDefineAbsentGetterInSlot(vm, globalObject, name, asObject(JSValue::decode(accessor.function)), attributes))
                continue;
        }
        defineAccessor(globalObject, object, JSValue::decode(accessor.key), JSValue::decode(accessor.function), accessorFlags);
        OPERATION_RETURN_IF_EXCEPTION(scope);
    }
    if (compiledObject)
        compiledObject->ensureStructureForExtraProperties(vm);
    OPERATION_RETURN(scope);
}

static void defineMethod(JSGlobalObject* globalObject, JSObject* holder, PropertyName key, JSObject* function, MethodKind kind)
{
    VM& vm = globalObject->vm();
    switch (kind) {
    case MethodKind::Method: {
        constexpr unsigned attributes = static_cast<unsigned>(PropertyAttribute::DontEnum);
        if (Structure* structure = holder->structure(); canSkipDefineOwnProperty(vm, holder, structure, key) && !isValidOffset(structure->get(vm, key))) {
            holder->putDirect(vm, key, function, attributes);
            return;
        }
        holder->methodTable()->defineOwnProperty(holder, globalObject, key, PropertyDescriptor(function, attributes), true);
        return;
    }
    case MethodKind::Getter:
    case MethodKind::Setter:
        defineAccessor(globalObject, holder, key, function, false, kind == MethodKind::Getter);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefineMethod, void, (Instance* instance, JSCell* classObject, EncodedJSValue property, JSCell* function, uint32_t sideAndKind))
{
    constexpr Entry entry = Entry::operationAOTDefineMethod;
    AOT_OPERATION_BEGIN(instance, entry);
    Closure* closure = cellArgument<Closure>(classObject, entry);
    const LayoutData* classLayout = closure->classData();
    if (!classLayout) [[unlikely]]
        crashOnWrongOperand(entry);
    DefinedMethodBits bits = DefinedMethodBits::decode(sideAndKind);
    ClassSide side = static_cast<ClassSide>(bits.side);
    JSObject* holder = side == ClassSide::Static ? closure : asObject(closure->prototypeProperty(vm));
    ClassPrototype::ensureMembers(holder);
    auto key = JSValue::decode(property).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    JSObject* member = cellArgument<JSObject>(function, entry);
    MethodKind kind = static_cast<MethodKind>(bits.kind);
    {
        ClassTable::MemberDefinition definition(instance->state->objects->classes, *classLayout, side, key.impl());
        SetForScope prototypeBeingDefined(vm.aotState().classPrototypeBeingDefined, side == ClassSide::Prototype ? holder : nullptr);
        if (!tryDefineClassMemberWithoutTransition(globalObject, *closure, *classLayout, holder, key, member, kind))
            defineMethod(globalObject, holder, key, member, kind);
    }
    OPERATION_RETURN_IF_EXCEPTION(scope);
    instance->state->objects->classes.didDefineMember(vm, *classLayout, side, holder, key.impl(), member);
    OPERATION_RETURN(scope);
}

struct OwnPropertyOfPlainObject {
    JSValue valueOrAccessor;
    unsigned attributes { 0 };
};

// A property without a value is absent. There is no answer for an index, and none where only code of the object's class could give it.
static std::optional<OwnPropertyOfPlainObject> findOwnPropertyOfPlainObject(VM& vm, JSObject* object, UniquedStringImpl* name)
{
    if (parseIndex(*name))
        return std::nullopt;
    OwnPropertyOfPlainObject property;
    if (Object* compiled = compiledObjectOrNull(object))
        property.valueOrAccessor = compiled->getDirectFromNativeCode(vm, name, &property.attributes);
    else if (object->type() == FinalObjectType)
        property.valueOrAccessor = object->getDirect(vm, name, property.attributes);
    else
        return std::nullopt;
    if (property.valueOrAccessor && (property.attributes & PropertyAttribute::CustomAccessorOrValue))
        return std::nullopt;
    return property;
}

static std::optional<bool> isMarkedAsESModule(JSGlobalObject* globalObject, JSObject* object)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    const Identifier& marker = vm.propertyNames->__esModule;
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool isFound = object->getPropertySlot(globalObject, marker, slot);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return std::nullopt;
    }
    if (slot.isTaintedByOpaqueObject())
        return std::nullopt;
    if (!isFound)
        return false;
    if (!slot.isValue())
        return std::nullopt;
    return slot.getValue(globalObject, marker).toBoolean(globalObject);
}

// Does all of what TypeScript's __createBinding does, or nothing: false where that would run code of the program, throw, or meet a property that exists.
// Since TypeScript 4.6 the helper passes on the source's own descriptor for an accessor of an ES module and for a value that can never change.
static bool tryDefineLiveBinding(JSGlobalObject* globalObject, JSObject* target, JSObject* source, UniquedStringImpl* sourceKey, UniquedStringImpl* targetKey, bool reusesDescriptors, const Invocable<JSObject*()> auto& getterOfBinding)
{
    VM& vm = globalObject->vm();
    if (parseIndex(*targetKey) || !descriptorObjectsInheritNoMembers(globalObject))
        return false;

    PropertyDescriptorBits bits { };
    bits.hasEnumerable = true;
    bits.isEnumerable = true;
    bits.hasGet = true;
    JSValue valueOrGetter;
    JSValue setter = jsUndefined();
    if (reusesDescriptors) {
        std::optional<OwnPropertyOfPlainObject> property = findOwnPropertyOfPlainObject(vm, source, sourceKey);
        if (!property)
            return false;
        JSValue found = property->valueOrAccessor;
        bool isEnumerable = !(property->attributes & PropertyAttribute::DontEnum);
        bool isConfigurable = !(property->attributes & PropertyAttribute::DontDelete);
        if (found && (property->attributes & PropertyAttribute::Accessor)) {
            std::optional<bool> isESModule = isMarkedAsESModule(globalObject, source);
            if (!isESModule)
                return false;
            if (*isESModule) {
                auto* accessor = uncheckedDowncast<GetterSetter>(found);
                bits.hasSet = true;
                bits.hasConfigurable = true;
                bits.isEnumerable = isEnumerable;
                bits.isConfigurable = isConfigurable;
                valueOrGetter = accessor->isGetterNull() ? jsUndefined() : JSValue(accessor->getter());
                setter = accessor->isSetterNull() ? jsUndefined() : JSValue(accessor->setter());
            }
        } else if (found && (property->attributes & PropertyAttribute::ReadOnly) && !isConfigurable) {
            bits = { };
            bits.hasValue = true;
            bits.hasWritable = true;
            bits.hasEnumerable = true;
            bits.hasConfigurable = true;
            bits.isEnumerable = isEnumerable;
            valueOrGetter = found;
        }
    }
    if (!valueOrGetter)
        valueOrGetter = getterOfBinding();
    return tryDefineAbsentProperty(globalObject, target, targetKey, valueOrGetter, setter, bits);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryDefineLiveBinding, size_t, (Instance* instance, EncodedJSValue encodedTarget, EncodedJSValue encodedSource, EncodedJSValue encodedSourceKey, EncodedJSValue encodedTargetKey, JSCell* getter))
{
    constexpr Entry entry = Entry::operationAOTTryDefineLiveBinding;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferTerminationForAWhile deferTermination(vm);
    JSValue target = JSValue::decode(encodedTarget);
    JSValue source = JSValue::decode(encodedSource);
    JSValue targetKey = JSValue::decode(encodedTargetKey);
    UniquedStringImpl* sourceName = atomOfKey(JSValue::decode(encodedSourceKey));
    UniquedStringImpl* targetName = targetKey.isUndefined() ? sourceName : atomOfKey(targetKey);
    if (!target.isObject() || !source.isObject() || !sourceName || !targetName)
        OPERATION_RETURN(scope, 0);
    bool isDefined = tryDefineLiveBinding(globalObject, asObject(target), asObject(source), sourceName, targetName, true, [&] {
        return cellArgument<JSObject>(getter, entry);
    });
    OPERATION_RETURN(scope, static_cast<size_t>(isDefined));
}

// The capture slot of the key in a closure of the getter. The other of its two slots holds the source.
static constexpr uint32_t reExportKeySlotMask = 1;
static constexpr uint32_t reExportReusesDescriptors = 2;

static bool canEnumerateWithoutRunningCode(JSGlobalObject* globalObject, JSObject* source, JSObject* target)
{
    for (JSObject* object = source; object; object = object->getPrototypeDirect().getObject()) {
        bool isPlain = compiledObjectOrNull(object) || object->type() == FinalObjectType || object == globalObject->objectPrototype();
        if (!isPlain || object == target)
            return false;
    }
    return true;
}

// The keys that `for (key in object)` visits while its body changes neither the object nor what the object inherits from. False once the functor says so, or where the keys include indices or cannot be listed.
static bool forEachEnumeratedName(JSGlobalObject* globalObject, JSObject* object, const Invocable<bool(JSString*)> auto& functor)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    auto didThrow = [&] {
        if (!scope.exception()) [[likely]]
            return false;
        scope.clearException();
        return true;
    };
    JSPropertyNameEnumerator* enumerator = enumeratorFor(globalObject, object);
    if (didThrow() || enumerator->indexedLength())
        return false;
    for (uint64_t position = 0;; ++position) {
        EnumeratedKey found = findEnumeratedKey(globalObject, enumerator, object, position);
        if (didThrow())
            return false;
        if (!found.key)
            return true;
        if (!functor(found.key))
            return false;
        position = found.position;
    }
}

// Does what TypeScript's __exportStar does, one name after the other. Behind false the written loop finishes the work: it skips the names that the target has by then.
static bool tryReExportFields(Instance* instance, JSObject* target, JSObject* source, FunctionID getter, uint32_t rule)
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    CaptureCounts counts = captureCountsOf(getter);
    if (counts.captures != 2 || counts.references != 2 || !canEnumerateWithoutRunningCode(globalObject, source, target))
        return false;
    Object* compiledTarget = compiledObjectOrNull(target);
    Object* compiledSource = compiledObjectOrNull(source);
    if (compiledTarget && compiledSource && &compiledTarget->instance() == instance && tryAddPendingStar(vm, *compiledTarget, *compiledSource, getter, rule))
        return true;
    bool isKeyInFirstSlot = !(rule & reExportKeySlotMask);
    UniquedStringImpl* defaultName = vm.propertyNames->defaultKeyword.impl();
    return forEachEnumeratedName(globalObject, source, [&](JSString* key) {
        UniquedStringImpl* name = atomOfKey(key);
        if (!name)
            return false;
        if (name == defaultName)
            return true;
        std::optional<OwnPropertyOfPlainObject> existing = findOwnPropertyOfPlainObject(vm, target, name);
        if (!existing)
            return false;
        if (existing->valueOrAccessor)
            return true;
        return tryDefineLiveBinding(globalObject, target, source, name, name, rule & reExportReusesDescriptors, [&] {
            uint64_t capturedKey = rule & starRuleKeyIsLiteralStringWord ? instance->state->strings->literalStringWordFor(key) : JSValue::encode(key);
            uint64_t capturedSource = JSValue::encode(source);
            std::array<uint64_t, 2> captures { isKeyInFirstSlot ? capturedKey : capturedSource, isKeyInFirstSlot ? capturedSource : capturedKey };
            return instance->state->objects->closures.createClosure(vm, getter, captures, counts.references);
        });
    });
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryReExportFields, size_t, (Instance* instance, EncodedJSValue encodedTarget, EncodedJSValue encodedSource, uint32_t getter, uint32_t rule))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTryReExportFields);
    DeferTerminationForAWhile deferTermination(vm);
    JSValue target = JSValue::decode(encodedTarget);
    JSValue source = JSValue::decode(encodedSource);
    if (!target.isObject() || !source.isObject())
        OPERATION_RETURN(scope, 0);
    OPERATION_RETURN(scope, static_cast<size_t>(tryReExportFields(instance, asObject(target), asObject(source), FunctionID(getter), rule)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTRecordCallee, void, (Instance* instance, JSCell* closure))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTRecordCallee);
    recordCallee(vm, asObject(closure));
    instance->heap.recordedCallees = FunctionTables::recordedCalleesOf(&FunctionTables::of(vm));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSetFunctionName, void, (Instance* instance, JSCell* cell, EncodedJSValue encodedName, uint32_t methodKind))
{
    constexpr Entry entry = Entry::operationAOTSetFunctionName;
    AOT_OPERATION_BEGIN(instance, entry);
    JSValue name = JSValue::decode(encodedName);
    RELEASE_ASSERT(name.isString() || name.isSymbol());
    auto key = name.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    std::optional<FunctionID> function = functionOf(cell);
    if (!function) [[unlikely]]
        crashOnWrongOperand(entry);
    setComputedFunctionName(vm, asObject(cell), key, static_cast<MethodKind>(methodKind));
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
