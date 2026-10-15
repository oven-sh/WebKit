/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTInlineCaches.h"
#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedObject.h"
#include "B3StackmapGenerationParams.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "ObjectConstructor.h"
#include "ObjectConstructorInlines.h"
#include "ObjectPrototype.h"
#include "ReflectObject.h"
#include "StructureRareData.h"

namespace JSC { namespace AOT {

enum class OwnKeys : uint32_t { ObjectKeys, ObjectGetOwnPropertyNames, ObjectGetOwnPropertySymbols, ReflectOwnKeys };

enum class FunctionInEngine : uint32_t { Keys, GetOwnPropertyNames, GetOwnPropertySymbols, OwnKeys, Values, GetOwnPropertyDescriptor };
static_assert(static_cast<uint32_t>(FunctionInEngine::Keys) == static_cast<uint32_t>(OwnKeys::ObjectKeys) && static_cast<uint32_t>(FunctionInEngine::GetOwnPropertyNames) == static_cast<uint32_t>(OwnKeys::ObjectGetOwnPropertyNames));
static_assert(static_cast<uint32_t>(FunctionInEngine::GetOwnPropertySymbols) == static_cast<uint32_t>(OwnKeys::ObjectGetOwnPropertySymbols) && static_cast<uint32_t>(FunctionInEngine::OwnKeys) == static_cast<uint32_t>(OwnKeys::ReflectOwnKeys));

static NEVER_INLINE JSValue callInEngine(Instance* instance, FunctionInEngine which, JSValue first, JSValue second = JSValue())
{
    static constexpr unsigned count = static_cast<unsigned>(FunctionInEngine::GetOwnPropertyDescriptor) + 1;
    static constexpr std::array<std::array<ASCIILiteral, 2>, count> names { {
        { "Object"_s, "keys"_s }, { "Object"_s, "getOwnPropertyNames"_s }, { "Object"_s, "getOwnPropertySymbols"_s }, { "Reflect"_s, "ownKeys"_s }, { "Object"_s, "values"_s }, { "Object"_s, "getOwnPropertyDescriptor"_s },
    } };
    static const std::array<std::array<unsigned, 2>, count> cells = [] {
        auto& builtins = Typed::Builtins::singleton();
        std::array<std::array<unsigned, 2>, count> result;
        for (unsigned i = 0; i < count; ++i)
            result[i] = { builtins.cellOfGlobal(names[i][0]), builtins.cellOfStaticFunction(names[i][0], names[i][1]) };
        return result;
    }();
    unsigned index = static_cast<unsigned>(which);
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(*instance->vm).countOperation("called in the engine", names[index][1].characters());
    MarkedArgumentBuffer list;
    list.append(first);
    if (second)
        list.append(second);
    return callBuiltinInEngine(instance, cells[index][1], typedBuiltinCell(instance, cells[index][0]), list);
}

// See Lowering::lowerOwnPropertyKeys().
static void noteKeyList(Instance* instance, VM& vm, JSObject* object)
{
    if (!object->structure()->compiledLayout())
        return;
    auto* ofProgram = uncheckedDowncast<Typed::Object>(object);
    if (&ofProgram->state() != instance->typed)
        return;
    if (JSCellButterfly* list = ofProgram->keys(vm))
        instance->typedKeyLists[Typed::shapeWordOf(ofProgram->shape()) % Instance::numberOfTypedKeyLists] = { ofProgram->orderWord(), Typed::shapeWordOf(ofProgram->shape()), list };
}

// As objectConstructorKeys(), objectConstructorGetOwnPropertyNames(), objectConstructorGetOwnPropertySymbols(), reflectObjectOwnKeys().
JSC_DEFINE_JIT_OPERATION(operationAOTTypedOwnPropertyKeys, JSCell*, (Instance* instance, EncodedJSValue encodedValue, uint32_t which))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(encodedValue);
    OwnKeys keys = static_cast<OwnKeys>(which);
    JSObject* object;
    if (value.isObject()) [[likely]]
        object = asObject(value);
    else {
        if (keys == OwnKeys::ReflectOwnKeys || value.isUndefinedOrNull()) {
            JSValue list = callInEngine(instance, static_cast<FunctionInEngine>(which), value);
            OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
            OPERATION_RETURN(scope, list.asCell());
        }
        object = value.toObject(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    if (object->structure()->isDictionary()) [[unlikely]]
        flattenOnce(vm, object);
    if (keys != OwnKeys::ObjectGetOwnPropertySymbols)
        noteKeyList(instance, vm, object);
    switch (keys) {
    case OwnKeys::ObjectKeys:
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::Strings, DontEnumPropertiesMode::Exclude));
    case OwnKeys::ObjectGetOwnPropertyNames:
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::Strings, DontEnumPropertiesMode::Include));
    case OwnKeys::ObjectGetOwnPropertySymbols:
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::Symbols, DontEnumPropertiesMode::Include));
    case OwnKeys::ReflectOwnKeys:
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::StringsAndSymbols, DontEnumPropertiesMode::Include));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// As objectConstructorValues().
JSC_DEFINE_JIT_OPERATION(operationAOTTypedObjectValues, JSCell*, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isUndefinedOrNull()) [[unlikely]] {
        JSValue list = callInEngine(instance, FunctionInEngine::Values, value);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        OPERATION_RETURN(scope, list.asCell());
    }
    JSValue values = objectValues(vm, globalObject, value);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, values.asCell());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedObjectEntries, JSCell*, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(encodedValue);
    if (!value.isObject() || globalObject->isHavingABadTime())
        return nullptr;
    JSObject* target = asObject(value);
    Structure* structure = target->structure();
    if (!target->staticPropertiesReified() || target->canHaveExistingOwnIndexedProperties() || target->hasNonReifiedStaticProperties() || !structure->canPerformFastPropertyEnumerationCommon())
        return nullptr;
    JSCellButterfly* names = structure->cachedPropertyNames(CachedPropertyNamesKind::EnumerableStrings);
    if (!names)
        return nullptr;
    Vector<std::pair<UniquedStringImpl*, PropertyOffset>, 16> properties;
    structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) -> bool {
        if (!(entry.attributes() & PropertyAttribute::DontEnum) && !entry.key()->isSymbol())
            properties.append({ entry.key(), entry.offset() });
        return true;
    });
    unsigned count = properties.size();
    if (names->length() != count)
        return nullptr;
    for (unsigned i = 0; i < count; ++i) {
        if (asString(names->get(i))->tryGetValueImpl() != properties[i].first)
            return nullptr;
    }
    Structure* arrayStructure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    JSArray* entries = JSArray::tryCreate(vm, arrayStructure, count);
    if (!entries)
        return nullptr;
    for (unsigned i = 0; i < count; ++i) {
        JSArray* entry;
        {
            ObjectInitializationScope initializationScope(vm);
            entry = JSArray::tryCreateUninitializedRestricted(initializationScope, nullptr, arrayStructure, 2);
            if (!entry)
                return nullptr;
            entry->initializeIndex(initializationScope, 0, names->get(i));
            entry->initializeIndex(initializationScope, 1, target->getDirect(properties[i].second));
        }
        entries->butterfly()->contiguous().at(entries, i).set(vm, entries, entry);
    }
    ensureStillAliveHere(names);
    return entries;
}

static bool mayHaveIndexThatAStoreDoesNotDefine(JSObject* target)
{
    return (hasAnyArrayStorage(target->indexingType()) && target->butterfly()->arrayStorage()->m_sparseMap) || target->structure()->holesMustForwardToPrototype(target);
}

// As operationObjectAssignUntyped().
static void assignFrom(JSGlobalObject* globalObject, JSObject* target, JSValue sourceValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (sourceValue.isUndefinedOrNull())
        return;
    JSObject* source = sourceValue.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    if (auto* targetObject = dynamicDowncast<JSFinalObject>(target); targetObject && targetObject->canPerformFastPutInlineExcludingProto() && targetObject->isStructureExtensible() && !(source->canHaveExistingOwnIndexedProperties() && mayHaveIndexThatAStoreDoesNotDefine(targetObject))) {
        if (!source->staticPropertiesReified()) {
            source->reifyAllStaticProperties(globalObject);
            RETURN_IF_EXCEPTION(scope, void());
        }
        Vector<UniquedStringImpl*, 8> properties;
        MarkedArgumentBuffer values;
        bool isDone = objectAssignFast(globalObject, targetObject, source, properties, values);
        RETURN_IF_EXCEPTION(scope, void());
        if (isDone)
            return;
    }
    RELEASE_AND_RETURN(scope, objectAssignGeneric(globalObject, vm, target, source));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedObjectAssignFrom, void, (Instance* instance, JSCell* target, EncodedJSValue source))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    scope.release();
    assignFrom(globalObject, asObject(target), JSValue::decode(source));
    OPERATION_RETURN(scope);
}

// `Object.assign({}, source)`. Where the properties of the source are plain values, none of them `__proto__`, a store to a new object is a definition: the copy is made as `{ ...source }` makes it.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedObjectAssignToNew, JSCell*, (Instance* instance, EncodedJSValue encodedSource))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue sourceValue = JSValue::decode(encodedSource);
    if (sourceValue.isObject()) {
        JSObject* source = asObject(sourceValue);
        Structure* structure = source->structure();
        if (source->staticPropertiesReified() && structure->canPerformFastPropertyEnumerationCommon()) {
            if (JSObject* copy = tryCreateObjectViaCloning(vm, globalObject, source))
                OPERATION_RETURN(scope, copy);
        }
    }
    JSFinalObject* target = constructEmptyObject(globalObject);
    assignFrom(globalObject, target, sourceValue);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, target);
}

// As HasOwnPropertyCache::tryAdd().
static bool holdsForEveryObjectWithStructure(const PropertySlot& slot, JSObject* object, PropertyName name)
{
    if (parseIndex(name) || (!slot.isCacheable() && !slot.isUnset()) || object->type() == GlobalProxyType)
        return false;
    Structure* structure = object->structure();
    return !structure->typeInfo().prohibitsPropertyCaching() && structure->propertyAccessesAreCacheable() && (!slot.isUnset() || (structure->propertyAccessesAreCacheableForAbsence() && !structure->isDictionary()));
}

// Of an object of a compiled layout no slot is cacheable, and much is known all the same: see Instance::OwnPropertyPlace::isCompiledField. Yields whether every object of the structure has an own property of the name, where they are all alike in that.
static std::optional<bool> notePlaceInCompiledObject(Instance* instance, JSObject* object, PropertyName name)
{
    using Place = Instance::OwnPropertyPlace;
    using Typed::Store;
    Structure* structure = object->structure();
    const Typed::LayoutData* layout = structure->compiledLayout();
    if (!layout || !Typed::isObjectLayout(layout->kind) || parseIndex(name) || name.isPrivateName())
        return std::nullopt;
    auto* compiled = uncheckedDowncast<Typed::Object>(object);
    const Typed::FieldData* field = name.isSymbol() ? nullptr : compiled->fieldNamed(compiled->state(), name);
    if (!field) {
        if (!Typed::Object::absenceIsByStructure(structure, name.uid()))
            return std::nullopt;
        instance->noteOwnPropertyPlace(structure->id().bits(), name.uid(), 0);
        return false;
    }
    if (field->flags & Typed::fieldIsOutOfLine)
        return std::nullopt;
    unsigned width = 0;
    unsigned marker = 0;
    switch (field->store()) {
    case Store::JSValue:
    case Store::Cell:
        width = 8;
        break;
    case Store::Cell32:
    case Store::OptionalCell32:
        static_assert(!Typed::absentCell32);
        width = 4;
        break;
    case Store::OptionalBoolean:
        width = 1;
        break;
    case Store::Int32OrElsewhere:
        width = 4;
        marker = 1;
        break;
    case Store::OptionalDouble:
        width = 8;
        marker = 2;
        break;
    case Store::OptionalInt64:
        width = 8;
        marker = 3;
        break;
    default:
        // It has no way to say "absent".
        instance->noteOwnPropertyPlace(structure->id().bits(), name.uid(), Place::isPresent | Place::isNoValue);
        return true;
    }
    if (field->offset >> 3 >= 1u << (32 - Place::wordShift) || field->offset % width)
        return std::nullopt;
    instance->noteOwnPropertyPlace(structure->id().bits(), name.uid(), Place::isCompiledField | Place::isNoValue | (field->offset & 7) << Place::byteShift | (64 - 8 * width) << Place::maskShiftShift | marker << Place::markerShift | field->offset >> 3 << Place::wordShift);
    return std::nullopt;
}

// What the object has been found to have of its own under the name. See Instance::OwnPropertyPlace.
static void notePlaceOfOwnProperty(Instance* instance, const PropertySlot& slot, JSObject* object, PropertyName name)
{
    using Place = Instance::OwnPropertyPlace;
    static_assert(static_cast<unsigned>(PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete) == Place::attributes);
    notePlaceInCompiledObject(instance, object, name);
    if (!holdsForEveryObjectWithStructure(slot, object, name))
        return;
    uint32_t place = 0;
    if (!slot.isUnset()) {
        place = Place::isPresent | Place::isNoValue;
        if (slot.isCacheableValue() && slot.slotBase() == object) {
            PropertyOffset offset = slot.cachedOffset();
            int32_t displacement = isInlineOffset(offset) ? JSObject::offsetOfInlineStorage() + offset * sizeof(EncodedJSValue) : offsetInButterfly(offset) * static_cast<int32_t>(sizeof(EncodedJSValue));
            if (displacement != static_cast<int32_t>(static_cast<uint32_t>(displacement) << Place::displacementShift) >> Place::displacementShift)
                return;
            place = Place::isPresent | (slot.attributes() & Place::attributes) | (isInlineOffset(offset) ? 0 : Place::isOutOfLine) | static_cast<uint32_t>(displacement) << Place::displacementShift;
        }
    }
    instance->noteOwnPropertyPlace(object->structureID().bits(), name.uid(), place);
}

// As objectConstructorHasOwn().
JSC_DEFINE_JIT_OPERATION(operationAOTTypedHasOwnProperty, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedKey))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (Options::validateAOTTypedFacts()) [[unlikely]]
        instance->validateKeyedTables();
    JSObject* object = JSValue::decode(encodedBase).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    auto name = JSValue::decode(encodedKey).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (object->structure()->isDictionary()) [[unlikely]]
        flattenOnce(vm, object);
    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    bool result = object->hasOwnProperty(globalObject, name, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    notePlaceOfOwnProperty(instance, slot, object, name);
    OPERATION_RETURN(scope, result);
}

static uint64_t headerWith(Structure* structure)
{
    return static_cast<uint64_t>(structure->typeInfoBlob()) << 32 | structure->id().bits();
}

// The allocator is that of every ordinary object of the default size.
static void prepareDataPropertyDescriptorAllocation(Instance* instance)
{
    auto& data = instance->inlineAllocations[static_cast<unsigned>(Instance::InlineAllocation::DataPropertyDescriptor)];
    if (data.allocator) [[likely]]
        return;
    Structure* structure = instance->globalObject->dataPropertyDescriptorObjectStructure();
    RELEASE_ASSERT(structure->inlineCapacity() == JSFinalObject::defaultInlineCapacity);
    auto* allocator = subspaceFor<JSFinalObject>(*instance->vm)->allocatorFor(JSFinalObject::defaultSizeInBytes, AllocatorForMode::EnsureAllocator).localAllocator();
    RELEASE_ASSERT(allocator->cellSize() == JSFinalObject::defaultSizeInBytes);
    data.header = headerWith(structure);
    data.allocator = allocator;
}

// As objectConstructorCreate() of an object. Also sees to it that the next heir of the prototype is made in place.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedObjectCreate, JSCell*, (Instance* instance, JSCell* prototype))
{
    prepareDataPropertyDescriptorAllocation(instance);
    JSFinalObject* object = constructEmptyObject(instance->globalObject, asObject(prototype));
    if (object->structure()->inlineCapacity() == JSFinalObject::defaultInlineCapacity) {
        instance->heirsOfPrototypes[Instance::heirOfPrototypeIndex(std::bit_cast<uintptr_t>(prototype))] = { prototype, headerWith(object->structure()) };
        instance->hasHeirsOfPrototypes = true;
    }
    return object;
}

// As objectConstructorGetOwnPropertyDescriptor(). Also sees to it that the next descriptor of a value is made in place.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetOwnPropertyDescriptor, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedKey))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    prepareDataPropertyDescriptorAllocation(instance);
    JSValue base = JSValue::decode(encodedBase);
    JSObject* object;
    if (base.isObject()) [[likely]]
        object = asObject(base);
    else {
        if (base.isUndefinedOrNull())
            OPERATION_RETURN(scope, JSValue::encode(callInEngine(instance, FunctionInEngine::GetOwnPropertyDescriptor, base, JSValue::decode(encodedKey))));
        object = base.toObject(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    auto name = JSValue::decode(encodedKey).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    // What asking an ordinary object comes to is not seen by anybody.
    if (!object->structure()->typeInfo().overridesGetOwnPropertySlot()) {
        PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
        object->hasOwnProperty(globalObject, name, slot);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        notePlaceOfOwnProperty(instance, slot, object, name);
    }
    OPERATION_RETURN(scope, JSValue::encode(objectConstructorGetOwnPropertyDescriptor(globalObject, object, name)));
}

// The same for a key that is a string of the program.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedHasOwnPropertyOfName, size_t, (Instance* instance, EncodedJSValue encodedBase, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* object = JSValue::decode(encodedBase).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    const Identifier& identifier = instance->typed->name(vm, name);
    if (object->structure()->isDictionary()) [[unlikely]]
        flattenOnce(vm, object);
    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    bool result = object->hasOwnProperty(globalObject, identifier, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (holdsForEveryObjectWithStructure(slot, object, identifier))
        instance->noteOwnPropertyAnswer(object->structureID().bits(), name, result);
    OPERATION_RETURN(scope, result);
}

namespace Typed {

using namespace B3;

// The family aot/builtins/object.def.

std::optional<Writes> writesOfInlineObject(InlineBuiltin which, std::span<const Type> arguments)
{
    switch (which) {
    // A proxy answers with code of the program.
    case InlineBuiltin::ObjectHasOwn:
    case InlineBuiltin::ObjectKeys:
    case InlineBuiltin::ObjectGetOwnPropertyNames:
    case InlineBuiltin::ObjectGetOwnPropertySymbols:
    case InlineBuiltin::ObjectValues:
    case InlineBuiltin::ObjectEntries:
    case InlineBuiltin::ReflectOwnKeys:
        return Writes::Heap;
    case InlineBuiltin::ObjectAssign:
        return writesIf(!arguments.empty(), Writes::Heap);
    case InlineBuiltin::ObjectGetOwnPropertyDescriptor:
        return writesIf(arguments.size() >= 2, Writes::Heap);
    // What becomes a prototype gets another Structure.
    case InlineBuiltin::ObjectCreate:
        return writesIf(arguments.size() == 1, Writes::Heap);
    case InlineBuiltin::ObjectGetPrototypeOf:
    case InlineBuiltin::ReflectGetPrototypeOf:
        return Writes::Heap;
    case InlineBuiltin::ObjectCall:
        return writesIf(arguments.size() == 1, Writes::RuntimeState);
    // At most a rope is made into a string.
    case InlineBuiltin::ObjectIs:
        return areNumbers(arguments) ? Writes::Pure : Writes::RuntimeState;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

CellPointer Lowering::objectOrElse(ValueID id, LBasicBlock otherwise)
{
    return fitOrElse(get(id), typeOf(id), m_program.at(m_program.types.object), otherwise).as<CellPointer>();
}

bool Lowering::hasOnlyObjectsOfProgram(const Type& type) const
{
    return !type.views.isEmpty() && !type.anyObject && !type.anyFunction && type.engineClasses.isEmpty() && !type.admitsForeignObjects(m_program);
}

// As lowerResolvedBuiltin() takes what the function of the engine yields.
Operand Lowering::yieldAsTableSays(Operand yielded, const Type& guarantee, const Value& call, const Type& type)
{
    if (vouchesForItsType(m_program, m_function, call))
        return convert(yielded, type, repOf(type));
    return ensure(yielded, guarantee, type);
}

// As FTL::LowerDFGToB3::allocateHeapCell().
CellPointer Lowering::allocateCell(LValue allocator, size_t cellSize, LBasicBlock slowPath)
{
    LBasicBlock continuation = m_out.newBlock();
    PatchpointValue* patchpoint = m_out.patchpoint(pointerType());
    applyEffects(patchpoint, { Writes::RuntimeState, false });
    patchpoint->effects.terminal = true;
    patchpoint->append(ConstrainedValue(allocator, ValueRep::SomeRegister));
    patchpoint->numGPScratchRegisters = 1;
    patchpoint->resultConstraints = { ValueRep::SomeEarlyRegister };
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    m_out.appendSuccessor(usually(continuation));
    m_out.appendSuccessor(rarely(slowPath));
    patchpoint->setGenerator([cellSize](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        CCallHelpers::JumpList outOfSpace;
        jit.emitAllocateWithNonNullAllocator(params[0].gpr(), JITAllocator::variableNonNullWithConstantCellSize(cellSize), params[1].gpr(), params.gpScratch(0), outOfSpace, CCallHelpers::SlowAllocationResult::UndefinedBehavior);
        CCallHelpers::Jump done;
        if (!params.fallsThroughToSuccessor(0))
            done = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            outOfSpace.linkTo(*labels[1], &jit);
            if (done.isSet())
                done.linkTo(*labels[0], &jit);
        });
    });
    m_out.appendTo(continuation);
    return CellPointer(patchpoint);
}

// An array on a list of boxed values that arrays share until they are written to.
CellPointer Lowering::newCopyOnWriteArray(LValue list, LBasicBlock slowPath)
{
    static_assert(sizeof(JSArray) <= MarkedSpace::preciseCutoff && !(sizeof(JSArray) % MarkedSpace::sizeStep));
    // Zero once arrays are no longer allocated with this structure.
    LValue header = changing64(Instance::offsetOfNewCopyOnWriteArrayHeaders() + 2 * sizeof(uint64_t));
    orElse(m_out.notZero64(header), slowPath);
    CellPointer array = allocateCell(fixedPointer(Instance::offsetOfArrayAllocator()), sizeof(JSArray), slowPath);
    storeHeader(array.value(), header);
    m_out.storePtr(m_out.add(list, m_out.constIntPtr(JSCellButterfly::offsetOfData())), array.value(), m_heaps.JSObject_butterfly);
    fenceAfterInitialization();
    return array;
}

LValue Lowering::keyListOfLayoutOrElse(CellPointer object, LBasicBlock otherwise)
{
    static_assert(sizeof(Instance::TypedKeyList) == 16);
    static_assert(hasOneBitSet(Instance::numberOfTypedKeyLists));
    Word32 shapeWord = shapeWordOf(object);
    LValue entry = m_out.add(fixedPointer(Instance::offsetOfTypedKeyLists()), m_out.shl(zeroExtended(bitAnd(shapeWord, word32(Instance::numberOfTypedKeyLists - 1))).value(), m_out.constInt32(4)));
    orElse(equal(Word32(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, entry, OBJECT_OFFSETOF(Instance::TypedKeyList, shapeWord)))), shapeWord), otherwise);
    Word32 orderWord(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, entry, OBJECT_OFFSETOF(Instance::TypedKeyList, orderWord))));
    orElse(equal(orderWord, orderWordOf(object)), otherwise);
    LValue list = m_out.loadPtr(m_out.address(m_heaps.AOTTyped_runtimeState, entry, OBJECT_OFFSETOF(Instance::TypedKeyList, list)));
    orElse(m_out.notNull(list), otherwise);
    return list;
}

// As FTL::LowerDFGToB3::compileOwnPropertyKeysVariant(): the list the engine keeps for the Structure. Of an object of the program: the list the run time keeps for its layout and key order,
// by Instance::typedKeyLists.
CellPointer Lowering::lowerOwnPropertyKeys(InlineBuiltin which, ValueID argument)
{
    OwnKeys keys = OwnKeys::ObjectKeys;
    const AbstractHeap* keptForStructure = &m_heaps.StructureRareData_cachedEnumerableStrings;
    switch (which) {
    case InlineBuiltin::ObjectKeys:
        break;
    case InlineBuiltin::ObjectGetOwnPropertyNames:
        keys = OwnKeys::ObjectGetOwnPropertyNames;
        keptForStructure = &m_heaps.StructureRareData_cachedStrings;
        break;
    case InlineBuiltin::ObjectGetOwnPropertySymbols:
        keys = OwnKeys::ObjectGetOwnPropertySymbols;
        keptForStructure = &m_heaps.StructureRareData_cachedSymbols;
        break;
    case InlineBuiltin::ReflectOwnKeys:
        keys = OwnKeys::ReflectOwnKeys;
        keptForStructure = &m_heaps.StructureRareData_cachedStringsAndSymbols;
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    auto& type = typeOf(argument);
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock haveList = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 2> lists;
    CellPointer object = objectOrElse(argument, slowPath);

    // Ends the block.
    auto findListOfLayout = [&] {
        lists.append(m_out.anchor(keyListOfLayoutOrElse(object, slowPath)));
        m_out.jump(haveList);
    };
    // Without extra properties its keys are strings and its properties enumerable: one list for all but the symbols.
    bool mayBeOfProgram = keys != OwnKeys::ObjectGetOwnPropertySymbols && (type.anyObject || !type.views.isEmpty());
    if (mayBeOfProgram && hasOnlyObjectsOfProgram(type))
        findListOfLayout();
    else {
        LBasicBlock isNotKept = mayBeOfProgram ? newUnlikelyBlock() : slowPath;
        LValue previousOrRareData = m_out.loadPtr(structureOf(object.value()), m_heaps.Structure_previousOrRareData);
        orElse(m_out.notNull(previousOrRareData), isNotKept);
        orElse(m_out.logicalNot(isCellOfType(previousOrRareData, StructureType)), isNotKept);
        LValue list = m_out.loadPtr(previousOrRareData, *keptForStructure);
        orElse(m_out.above(list, m_out.constIntPtr(std::bit_cast<uintptr_t>(StructureRareData::cachedPropertyNamesSentinel()))), isNotKept);
        lists.append(m_out.anchor(list));
        m_out.jump(haveList);
        if (mayBeOfProgram) {
            m_out.appendTo(isNotKept);
            orElse(passes(shapeWordOf(object), ShapeTest { 1, static_cast<uint32_t>(m_program.layouts.size()) }), slowPath);
            findListOfLayout();
        }
    }

    m_out.appendTo(haveList);
    ValueFromBlock fast = m_out.anchor(newCopyOnWriteArray(m_out.phi(pointerType(), lists), slowPath).value());
    m_out.jump(continuation);

    m_out.appendTo(slowPath);
    ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(OwnPropertyKeys, in<Boxed>(argument), word32(static_cast<int32_t>(keys))).value());
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(pointerType(), fast, slow));
}

// What operationAOTTypedForInKeys() yields for an object whose Structure keeps an enumerator that watchpoints vouch for, as FTL::LowerDFGToB3::compileGetPropertyEnumerator() finds it, if that lists
// nothing but properties of the Structure: the list Object.keys() yields, shared.
CellPointer Lowering::listKeysForIn(Boxed value, const Type& type)
{
    static_assert(!NonArray && ArrayClass == 1 && UndecidedShape == 2 && ArrayWithUndecided == 3);
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    Vector<ValueFromBlock, 3> results;
    // No Structure of anything but an object keeps one.
    orElse(isCell(value, type), slowPath);
    CellPointer object = asCell(value);
    auto enumeratorKeptFor = [&](CellPointer cell) {
        orElse(belowOrEqual(indexingModeOf(cell, IndexingTypeMask), word32(ArrayWithUndecided)), slowPath);
        LValue previousOrRareData = m_out.loadPtr(structureOf(cell.value()), m_heaps.Structure_previousOrRareData);
        orElse(m_out.notNull(previousOrRareData), slowPath);
        orElse(m_out.logicalNot(isCellOfType(previousOrRareData, StructureType)), slowPath);
        LValue enumerator = m_out.loadPtr(previousOrRareData, m_heaps.StructureRareData_cachedPropertyNameEnumeratorAndFlag);
        orElse(m_out.notNull(enumerator), slowPath);
        orElse(m_out.testIsZeroPtr(enumerator, m_out.constIntPtr(StructureRareData::cachedPropertyNameEnumeratorIsValidatedViaTraversingFlag)), slowPath);
        return std::pair { previousOrRareData, enumerator };
    };
    bool isOfProgram = hasOnlyObjectsOfProgram(type);
    if (!type.views.isEmpty()) {
        LBasicBlock isNotOfProgram = isOfProgram ? nullptr : m_out.newBlock();
        if (!isOfProgram)
            orElse(passes(shapeWordOf(object), ShapeTest { 1, static_cast<uint32_t>(m_program.layouts.size()) }), isNotOfProgram);
        LValue list = keyListOfLayoutOrElse(object, slowPath);
        Boxed prototype(m_out.load64(structureOf(object.value()), m_heaps.Structure_prototype));
        orElse(m_out.notZero64(prototype.value()), slowPath);
        orElse(isCell(prototype), slowPath);
        LValue inherited = enumeratorKeptFor(CellPointer(prototype.value())).second;
        orElse(m_out.isZero32(m_out.load32(inherited, m_heaps.JSPropertyNameEnumerator_endGenericPropertyIndex)), slowPath);
        orElse(m_out.isZero32(m_out.load32(inherited, m_heaps.JSPropertyNameEnumerator_indexLength)), slowPath);
        results.append(m_out.anchor(newCopyOnWriteArray(list, slowPath).value()));
        m_out.jump(continuation);
        if (!isOfProgram)
            m_out.appendTo(isNotOfProgram);
    }
    if (!isOfProgram) {
        auto [previousOrRareData, enumerator] = enumeratorKeptFor(object);
        LValue numberOfNames = m_out.load32(enumerator, m_heaps.JSPropertyNameEnumerator_endGenericPropertyIndex);
        orElse(m_out.notZero32(numberOfNames), slowPath);
        orElse(m_out.equal(m_out.load32(enumerator, m_heaps.JSPropertyNameEnumerator_endStructurePropertyIndex), numberOfNames), slowPath);
        orElse(m_out.isZero32(m_out.load32(enumerator, m_heaps.JSPropertyNameEnumerator_indexLength)), slowPath);
        LValue list = m_out.loadPtr(previousOrRareData, m_heaps.StructureRareData_cachedEnumerableStrings);
        orElse(m_out.above(list, m_out.constIntPtr(std::bit_cast<uintptr_t>(StructureRareData::cachedPropertyNamesSentinel()))), slowPath);
        results.append(m_out.anchor(newCopyOnWriteArray(list, slowPath).value()));
        m_out.jump(continuation);
    }

    m_out.appendTo(slowPath);
    results.append(m_out.anchor(AOT_TYPED_OPERATION(ForInKeys, value).value()));
    m_out.jump(continuation);

    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(pointerType(), results));
}

// Whether the objects of the members have an own property of the name, as far as their layouts say. An object has what its layout has a field for, unless the field is absent, and its extra properties.
Lowering::OwnField Lowering::ownFieldOfMembers(LayoutID view, StringID name) const
{
    auto& members = m_program.at(view).members;
    bool isInAll = !members.isEmpty();
    bool isInNone = !members.isEmpty();
    for (LayoutID member : members) {
        auto& layout = m_program.at(member);
        auto index = layout.fieldNamed(name);
        if (!index) {
            isInAll = false;
            continue;
        }
        isInNone = false;
        TypeID stored = layout.fields[*index].stored;
        if (!stored || m_program.at(stored).primitives & Primitive::Absent)
            isInAll = false;
    }
    return isInAll ? OwnField::InAllMembers : isInNone ? OwnField::InNoMember : OwnField::Varies;
}

// Object.hasOwn(object, key).
Word32 Lowering::lowerHasOwnProperty(ValueID object, ValueID key)
{
    // An index is no matter of the Structure.
    auto text = constantASCIIText(key);
    if (!text || text->empty() || isASCIIDigit(text->front()) || m_function.at(originOf(key)).string().index() > Instance::maxNameWithOwnPropertyAnswer) {
        if (!(expectedTypeOf(key).primitives & Primitive::String))
            return AOT_TYPED_OPERATION(HasOwnProperty, in<Boxed>(object), in<Boxed>(key));
        LBasicBlock slowPath = newColdBlock();
        Join join(m_out, Rep::Int32, Int32);
        if (auto* stamp = forInStampOf(object, key)) {
            LBasicBlock hasChanged = newColdBlock();
            isAsListedOrElse(*stamp, in<Boxed>(object), hasChanged);
            join.add(boolean(true));
            m_out.appendTo(hasChanged);
        }
        CellPointer cell = objectOrElse(object, slowPath);
        join.add(hasOwnPropertyAt(cell, placeOfOwnPropertyOrElse(cell, key, slowPath)));
        m_out.appendTo(slowPath);
        join.add(AOT_TYPED_OPERATION(HasOwnProperty, in<Boxed>(object), in<Boxed>(key)));
        return join.finish().as<Word32>();
    }
    StringID name = m_function.at(originOf(key)).string();
    auto generally = [&](Boxed value) {
        return AOT_TYPED_OPERATION(HasOwnPropertyOfName, value, word32(name.index()));
    };
    auto& receiver = typeOf(object);
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, Rep::Int32, Int32);
    CellPointer cell = objectOrElse(object, slowPath);

    // Instance::ownPropertyAnswers.
    auto askStructure = [&]() -> Operand {
        static_assert(Instance::ownPropertyAnswerIndex(0x1230, 7) == (((0x1230u >> 4) ^ Instance::saltOfOwnPropertyAnswer(7)) & (Instance::numberOfOwnPropertyAnswers - 1)));
        Word32 structureID = loadField<Word32>(cell, m_heaps.JSCell_structureID);
        Word32 index = bitAnd(bitXor(shiftRightUnsigned(structureID, word32(4)), word32(Instance::saltOfOwnPropertyAnswer(name.index()))), word32(Instance::numberOfOwnPropertyAnswers - 1));
        Pointer answers(m_out.add(m_instance, m_out.constIntPtr(Instance::offsetOfOwnPropertyAnswers())));
        // Every collection clears them.
        Integer answer(m_out.load64(TypedPointer(m_heaps.AOTTyped_collectorState, offsetBy(answers, zeroExtended(index), 3).value())));
        Integer difference = bitXor(answer, bitOr(zeroExtended(structureID), integer(static_cast<int64_t>(name.index()) << 32)));
        orElse(isZero(Integer(m_out.shl(difference.value(), m_out.constInt32(1)))), slowPath);
        return isNotZero(difference);
    };

    LayoutID view = receiver.onlyView();
    if (!view)
        join.add(askStructure());
    else {
        join.add(ifOfMember(cell, object, view, Rep::Int32, [&]() -> Operand {
            switch (ownFieldOfMembers(view, name)) {
            case OwnField::InAllMembers:
                return boolean(true);
            case OwnField::InNoMember:
                return either<Word32>(lacksBits(orderWordOf(cell), orderWordHasExtraProperties), [&] {
                    return boolean(false);
                }, [&] {
                    return generally(box(cell));
                });
            case OwnField::Varies:
                break;
            }
            // See Field::slot: only a field that placement has seen an access to is where the members have theirs.
            const Field* field = fieldFor(receiver, name);
            if (!field || (field->isLackedBySomeMember && field->membersWithIt.passesNothing()))
                return generally(box(cell));
            return throughView(cell, object, view, *field, Rep::Int32, [&]() -> Operand {
                return hasOwnField(cell, *field);
            }, [&]() -> Operand {
                return generally(box(cell));
            });
        }, askStructure));
    }

    m_out.appendTo(slowPath);
    join.add(generally(in<Boxed>(object)));
    return join.finish().as<Word32>();
}

// As FTL::LowerDFGToB3::compileHasOwnProperty() asks HasOwnPropertyCache. The key is a string that is a name already: what Object.keys() or `for..in` has yielded, or any that has been asked for before.
Word32 Lowering::placeOfOwnPropertyOrElse(CellPointer object, ValueID key, LBasicBlock otherwise)
{
    using Place = Instance::OwnPropertyPlace;
    static_assert(sizeof(Place) == 16 && !OBJECT_OFFSETOF(Place, uid) && OBJECT_OFFSETOF(Place, structureID) == 8 && OBJECT_OFFSETOF(Place, place) == 12);
    static_assert(Instance::ownPropertyPlaceIndex(0x1230, 0x7f40) == (((0x1230u ^ 0x7f40u) >> 4) & (Instance::numberOfOwnPropertyPlaces - 1)));
    CellPointer string = fitOrElse(get(key), typeOf(key), m_program.at(m_program.types.string), otherwise).as<CellPointer>();
    Pointer impl = loadField<Pointer>(string, m_heaps.JSString_value);
    orElse(lacksBits(impl, JSString::isRopeInPointer), otherwise);
    orElse(hasBits(loadField<Word32>(impl, m_heaps.StringImpl_hashAndFlags), StringImpl::flagIsAtom()), otherwise);
    Integer structureID = zeroExtended(loadField<Word32>(object, m_heaps.JSCell_structureID));
    Integer uid(impl.value());
    // The index times the size of an entry.
    Integer offset = bitAnd(bitXor(structureID, uid), integer((Instance::numberOfOwnPropertyPlaces - 1) << 4));
    Pointer entry(m_out.add(m_out.add(m_instance, m_out.constIntPtr(Instance::offsetOfOwnPropertyPlaces())), offset.value()));
    // Every collection clears them.
    orElse(equal(Integer(m_out.load64(m_out.address(m_heaps.AOTTyped_collectorState, entry.value(), OBJECT_OFFSETOF(Place, uid)))), uid), otherwise);
    Integer found(m_out.load64(m_out.address(m_heaps.AOTTyped_collectorState, entry.value(), OBJECT_OFFSETOF(Place, structureID))));
    orElse(equal(low32(found), low32(structureID)), otherwise);
    return low32(Integer(m_out.lShr(found.value(), m_out.constInt32(32))));
}

Word32 Lowering::hasOwnPropertyAt(CellPointer object, Word32 place)
{
    using Place = Instance::OwnPropertyPlace;
    LBasicBlock isField = m_out.newBlock();
    LBasicBlock isByStructure = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    m_out.branch(hasBits(place, Place::isCompiledField).value(), unsure(isField), unsure(isByStructure));
    m_out.appendTo(isByStructure);
    join.add(bitAnd(place, word32(Place::isPresent)));
    m_out.appendTo(isField);
    auto part = [&](unsigned shift, uint32_t mask) {
        return m_out.bitAnd(m_out.lShr(place.value(), m_out.constInt32(shift)), m_out.constInt32(mask));
    };
    static_assert(Place::wordShift + 13 == 32);
    LValue address = m_out.add(object.value(), m_out.shl(m_out.zeroExtPtr(m_out.lShr(place.value(), m_out.constInt32(Place::wordShift))), m_out.constInt32(3)));
    // It may be any field of any layout.
    LValue held = m_out.lShr(m_out.load64(TypedPointer(m_heaps.root, address)), m_out.shl(part(Place::byteShift, 7), m_out.constInt32(3)));
    held = m_out.bitAnd(held, m_out.lShr(m_out.constInt64(-1), part(Place::maskShiftShift, 63)));
    LValue which = part(Place::markerShift, 3);
    auto isMarker = [&](unsigned number) {
        return m_out.equal(which, m_out.constInt32(number));
    };
    LValue absent = m_out.select(isMarker(1), integer(static_cast<uint32_t>(absentInt32)).value(), m_out.select(isMarker(2), integer(static_cast<int64_t>(absentDouble)).value(), m_out.select(isMarker(3), integer(absentInteger).value(), m_out.int64Zero)));
    join.add(Word32(m_out.notEqual(held, absent)));
    return join.finish().as<Word32>();
}

TypedPointer Lowering::inlineProperty(CellPointer object, PropertyOffset offset)
{
    return m_out.address(m_heaps.properties.atAnyNumber(), object.value(), JSObject::offsetOfInlineStorage() + offset * sizeof(EncodedJSValue));
}

// Of a property that is a value of an ordinary object: the object the engine makes, constructObjectFromPropertyDescriptor(), made in place.
Operand Lowering::lowerGetOwnPropertyDescriptor(ValueID result, const Value& value)
{
    using Place = Instance::OwnPropertyPlace;
    constexpr auto kind = Instance::InlineAllocation::DataPropertyDescriptor;
    static_assert(JSFinalObject::defaultInlineCapacity == 6 && !dataPropertyDescriptorValuePropertyOffset && dataPropertyDescriptorWritablePropertyOffset == 1 && dataPropertyDescriptorEnumerablePropertyOffset == 2 && dataPropertyDescriptorConfigurablePropertyOffset == 3);
    static_assert(JSValue::ValueTrue == (JSValue::ValueFalse | 1));
    auto& type = typeOf(result);
    auto& anything = m_program.at(m_program.types.anything);
    ValueID key = value.arguments[1];
    auto generally = [&] {
        return yieldAsTableSays(AOT_TYPED_OPERATION(GetOwnPropertyDescriptor, in<Boxed>(value.arguments[0]), in<Boxed>(key)), anything, value, type);
    };
    if (!(expectedTypeOf(key).primitives & Primitive::String))
        return generally();
    Rep rep = repOf(type);
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock isPresent = m_out.newBlock();
    LBasicBlock isAbsent = m_out.newBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    CellPointer object = objectOrElse(value.arguments[0], slowPath);
    Word32 place = placeOfOwnPropertyOrElse(object, key, slowPath);
    orElse(lacksBits(place, Place::isNoValue), slowPath);
    m_out.branch(hasBits(place, Place::isPresent).value(), unsure(isPresent), unsure(isAbsent));

    m_out.appendTo(isPresent);
    // Null until the operation has been called once.
    LValue allocator = m_out.loadPtr(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, Instance::offsetOfAllocatorFor(kind)));
    orElse(m_out.notNull(allocator), slowPath);
    CellPointer descriptor = allocateCell(allocator, JSFinalObject::defaultSizeInBytes, slowPath);
    Pointer base = select(hasBits(place, Place::isOutOfLine), butterflyOf(object), Pointer(object.value()));
    LValue displacement = m_out.signExt32ToPtr(m_out.aShr(place.value(), m_out.constInt32(Place::displacementShift)));
    // It may be any property of any object.
    LValue held = m_out.load64(TypedPointer(m_heaps.root, m_out.add(base.value(), displacement)));
    auto lacksAttribute = [&](PropertyAttribute attribute) {
        return m_out.bitOr(m_out.constInt64(JSValue::ValueFalse), zeroExtended(lacksBits(place, static_cast<unsigned>(attribute))).value());
    };
    m_out.store64(m_out.load64(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, Instance::offsetOfHeaderFor(kind))), descriptor.value(), m_heaps.JSCell_header);
    m_out.storePtr(m_out.intPtrZero, descriptor.value(), m_heaps.JSObject_butterfly);
    m_out.store64(held, inlineProperty(descriptor, dataPropertyDescriptorValuePropertyOffset));
    m_out.store64(lacksAttribute(PropertyAttribute::ReadOnly), inlineProperty(descriptor, dataPropertyDescriptorWritablePropertyOffset));
    m_out.store64(lacksAttribute(PropertyAttribute::DontEnum), inlineProperty(descriptor, dataPropertyDescriptorEnumerablePropertyOffset));
    m_out.store64(lacksAttribute(PropertyAttribute::DontDelete), inlineProperty(descriptor, dataPropertyDescriptorConfigurablePropertyOffset));
    for (PropertyOffset offset = dataPropertyDescriptorConfigurablePropertyOffset + 1; offset < static_cast<PropertyOffset>(JSFinalObject::defaultInlineCapacity); ++offset)
        m_out.store64(m_out.int64Zero, inlineProperty(descriptor, offset));
    fenceAfterInitialization();
    join.add(yieldAsTableSays(descriptor, m_program.at(m_program.types.object), value, type));

    m_out.appendTo(isAbsent);
    join.add(yieldAsTableSays(undefined(), m_program.at(m_program.types.undefined), value, type));

    m_out.appendTo(slowPath);
    join.add(generally());
    return join.finish();
}

// Object.create(prototype): Instance::heirsOfPrototypes.
Operand Lowering::lowerCreate(ValueID result, const Value& value)
{
    using Heir = Instance::HeirOfPrototype;
    static_assert(sizeof(Heir) == 16 && !OBJECT_OFFSETOF(Heir, prototype) && Instance::heirOfPrototypeIndex(0x7f40) == ((0x7f40 >> 4) & (Instance::numberOfHeirsOfPrototypes - 1)));
    auto& type = typeOf(result);
    auto& objectType = m_program.at(m_program.types.object);
    Rep rep = repOf(type);
    LBasicBlock noObject = newBlockForGeneralCall(value);
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, rep, b3TypeOf(rep));
    CellPointer prototype = objectOrElse(value.arguments[0], noObject);
    // The index times the size of an entry.
    LValue offset = m_out.bitAnd(prototype.value(), m_out.constIntPtr((Instance::numberOfHeirsOfPrototypes - 1) << 4));
    LValue entry = m_out.add(m_out.add(m_instance, m_out.constIntPtr(Instance::offsetOfHeirsOfPrototypes())), offset);
    // Every collection clears them.
    orElse(m_out.equal(m_out.loadPtr(m_out.address(m_heaps.AOTTyped_collectorState, entry, OBJECT_OFFSETOF(Heir, prototype))), prototype.value()), slowPath);
    LValue header = m_out.load64(m_out.address(m_heaps.AOTTyped_collectorState, entry, OBJECT_OFFSETOF(Heir, header)));
    // Whoever has filled the entry has seen to the allocator.
    LValue allocator = m_out.loadPtr(m_out.address(m_heaps.AOTInstance_mutableFields, m_instance, Instance::offsetOfAllocatorFor(Instance::InlineAllocation::DataPropertyDescriptor)));
    CellPointer object = allocateCell(allocator, JSFinalObject::defaultSizeInBytes, slowPath);
    m_out.store64(header, object.value(), m_heaps.JSCell_header);
    m_out.storePtr(m_out.intPtrZero, object.value(), m_heaps.JSObject_butterfly);
    for (PropertyOffset slot = 0; slot < static_cast<PropertyOffset>(JSFinalObject::defaultInlineCapacity); ++slot)
        m_out.store64(m_out.int64Zero, inlineProperty(object, slot));
    fenceAfterInitialization();
    join.add(yieldAsTableSays(object, objectType, value, type));

    m_out.appendTo(slowPath);
    join.add(yieldAsTableSays(AOT_TYPED_OPERATION(ObjectCreate, prototype), objectType, value, type));

    if (!isGeneralArmOnOffer(noObject)) {
        m_out.appendTo(noObject);
        join.add(yieldAsTableSays(callBuiltinFunction(value, effectsOfBuiltinCall(value)), m_program.at(m_program.types.anything), value, type));
    }
    return join.finish();
}

// As FTL::LowerDFGToB3::compileGetPrototypeOf().
Operand Lowering::lowerGetPrototypeOf(ValueID result, const Value& value)
{
    auto& type = typeOf(result);
    Rep rep = repOf(type);
    LBasicBlock slowPath = newBlockForGeneralCall(value);
    Join join(m_out, rep, b3TypeOf(rep));
    CellPointer object = objectOrElse(value.arguments[0], slowPath);
    orElse(lacksBits(loadByte(object, m_heaps.JSCell_typeInfoFlags), OverridesGetPrototype), slowPath);
    Boxed prototype(m_out.load64(structureOf(object.value()), m_heaps.Structure_prototype));
    // Empty: the object keeps it itself.
    orElse(isNotEmpty(prototype), slowPath);
    Type objectOrNull = Type::object();
    objectOrNull.primitives = Primitive::Null;
    join.add(yieldAsTableSays(prototype, objectOrNull, value, type));

    if (!isGeneralArmOnOffer(slowPath)) {
        m_out.appendTo(slowPath);
        join.add(yieldAsTableSays(callBuiltinFunction(value, effectsOfBuiltinCall(value)), m_program.at(m_program.types.anything), value, type));
    }
    return join.finish();
}

// The key orders that objects of the members of the view are made with, or get to by the orders the program expects. An order word says the same of an object for ever (Layout::orders), so
// what is listed here decides how fast an object is dealt with, and nothing else. Empty: there are too many, or the value is not expected to be of one view.
Vector<Lowering::KeyOrder, 2> Lowering::keyOrdersExpectedOf(ValueID id) const
{
    static constexpr unsigned maxOrders = 2;
    static constexpr unsigned maxFields = 8;
    LayoutID view = expectedTypeOf(id).withoutPrimitives(Primitive::UndefinedOrNull).onlyView();
    if (!view || m_program.at(view).members.size() > maxOrders)
        return { };
    Vector<KeyOrder, 2> result;
    bool areTooMany = false;
    auto add = [&](LayoutID member, uint32_t order, uint32_t count) {
        uint32_t orderWord = Typed::orderWordOf(order, count);
        if (areTooMany || result.containsIf([&](const KeyOrder& known) { return known.layout == member && known.orderWord == orderWord; }))
            return;
        auto& layout = m_program.at(member);
        KeyOrder keys { member, orderWord, { } };
        for (uint16_t index : layout.orders[order].span().first(count)) {
            auto name = m_program.string(layout.fields[index].name);
            if (isPrivateName(name))
                continue;
            // An index comes before every name, and `__proto__` is not stored like any other.
            areTooMany |= name.empty() || isASCIIDigit(name.front()) || equalSpans(name, "__proto__"_s.span8());
            keys.fields.append(index);
        }
        areTooMany |= result.size() == maxOrders || keys.fields.size() > maxFields;
        if (!areTooMany)
            result.append(WTF::move(keys));
    };
    for (LayoutID member : m_program.at(view).members) {
        auto& layout = m_program.at(member);
        if (!isObjectLayout(layout.kind))
            return { };
        for (auto& allocation : m_program.allocations) {
            if (allocation.layout != member)
                continue;
            // As placement numbers them: what nothing stores does not exist.
            Vector<uint16_t, 16> expected;
            uint32_t born = 0;
            for (unsigned i = 0; i < allocation.order.size(); ++i) {
                if (layout.fields[allocation.order[i]].flags & fieldIsNeverStored)
                    continue;
                expected.append(allocation.order[i]);
                born += i < allocation.born;
            }
            size_t order = layout.orders.findIf([&](auto& candidate) {
                return candidate.size() >= expected.size() && std::equal(expected.begin(), expected.end(), candidate.begin());
            });
            if (order != notFound && born <= maxFieldsInOrder)
                add(member, order, born);
        }
        for (unsigned order = 0; order < layout.orders.size(); ++order)
            add(member, order, layout.orders[order].size());
    }
    return areTooMany ? Vector<KeyOrder, 2> { } : result;
}

// `inPlace` is given the object and the key order it has, `generally` nothing. Each ends its block.
template<typename InPlace, typename Generally>
void Lowering::byKeyOrder(ValueID object, const Vector<KeyOrder, 2>& orders, const InPlace& inPlace, const Generally& generally)
{
    if (orders.isEmpty()) {
        generally();
        return;
    }
    LBasicBlock otherwise = newColdBlock();
    CellPointer cell = objectOrElse(object, otherwise);
    // In their place any other object has a pointer, whose upper half is no shape word.
    Word32 shapeWord = shapeWordOf(cell);
    Word32 orderWord = orderWordOf(cell);
    for (auto& order : orders) {
        LBasicBlock matches = m_out.newBlock();
        LBasicBlock next = &order == &orders.last() ? otherwise : m_out.newBlock();
        m_out.branch(logicalAnd(equal(shapeWord, word32(Typed::shapeWordOf(m_program.at(order.layout).shape))), equal(orderWord, word32(order.orderWord))).value(), usually(matches), rarely(next));
        m_out.appendTo(matches);
        inPlace(cell, order);
        m_out.appendTo(next);
    }
    generally();
}

// `{ ...source }` where the copy is an ordinary object.
CellPointer Lowering::cloneAsPlainObject(ValueID source, const DefinedOnCopy& defined)
{
    auto& anything = m_program.at(m_program.types.anything);
    Join made(m_out, Rep::Cell, b3TypeOf(Rep::Cell));
    byKeyOrder(source, keyOrdersExpectedOf(source), [&](CellPointer cell, const KeyOrder& order) {
        auto& fields = m_program.at(order.layout).fields;
        Vector<StringID, 8> names;
        Vector<Boxed, 8> values;
        for (uint16_t index : order.fields) {
            names.append(fields[index].name);
            values.append(readField(cell, fields[index], anything).as<Boxed>());
        }
        made.add(newObjectLiteral(names.span(), values.span()));
    }, [&] {
        if (defined.more)
            made.add(AOT_TYPED_OPERATION(CloneObjectForMore, in<Boxed>(source), integer(static_cast<int64_t>(defined.more)), defined.values.size() > 0 ? in<Boxed>(defined.values[0]) : undefined(), defined.values.size() > 1 ? in<Boxed>(defined.values[1]) : undefined()));
        else
            made.add(AOT_TYPED_OPERATION(CloneObject, in<Boxed>(source), word32(false)));
    });
    return made.finish().as<CellPointer>();
}

// As newArray().
CellPointer Lowering::newArrayOf(std::span<const Boxed> elements)
{
    unsigned length = elements.size();
    if (length <= maxArrayElementsInRegisters) {
        Vector<std::pair<LValue, GPRReg>, 4> operands;
        for (unsigned i = 0; i < length; ++i)
            operands.append({ elements[i].value(), GPRInfo::toArgumentRegister(1 + i) });
        return CellPointer(callAllocationStub(arrayLiteralStub(length), operands, effectsOfOperation<operationAOTTypedNewArray>(), true));
    }
    unsigned vectorLength = std::max<unsigned>(length, BASE_CONTIGUOUS_VECTOR_LEN) | 1;
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    auto [array, butterfly] = allocateArray(word32(length), word32(vectorLength), slowPath);
    for (unsigned i = 0; i < length; ++i)
        m_out.store64(elements[i].value(), butterfly.value(), m_heaps.indexedContiguousProperties[i]);
    fenceAfterInitialization();
    ValueFromBlock fast = m_out.anchor(array.value());
    m_out.jump(continuation);
    m_out.appendTo(slowPath);
    LValue buffer = transientBuffer(length * sizeof(EncodedJSValue));
    for (unsigned i = 0; i < length; ++i)
        m_out.store64(elements[i].value(), m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
    ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(NewArray, word32(length), Pointer(buffer)).value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, fast, slow));
}

// Of an object with a layout, while its keys are in an order the program expects: its fields are read as any read of them does, and no list is gone through.
CellPointer Lowering::lowerValuesOrEntries(InlineBuiltin which, const Value& value)
{
    ValueID object = value.arguments[0];
    auto& anything = m_program.at(m_program.types.anything);
    Join join(m_out, Rep::Cell, b3TypeOf(Rep::Cell));
    byKeyOrder(object, keyOrdersExpectedOf(object), [&](CellPointer cell, const KeyOrder& order) {
        auto& fields = m_program.at(order.layout).fields;
        Vector<Boxed, 8> elements;
        for (uint16_t index : order.fields)
            elements.append(readField(cell, fields[index], anything).as<Boxed>());
        if (which == InlineBuiltin::ObjectEntries) {
            for (unsigned i = 0; i < elements.size(); ++i) {
                Boxed pair[] = { box(stringConstant(fields[order.fields[i]].name)), elements[i] };
                elements[i] = box(newArrayOf(pair));
            }
        }
        join.add(newArrayOf(elements.span()));
    }, [&] {
        if (which == InlineBuiltin::ObjectValues)
            join.add(AOT_TYPED_OPERATION(ObjectValues, in<Boxed>(object)));
        else {
            CellPointer made = AOT_TYPED_OPERATION(ObjectEntries, in<Boxed>(object));
            LBasicBlock isMade = m_out.newBlock();
            LBasicBlock isNotMade = newBlockForGeneralCall(value);
            m_out.branch(m_out.notNull(made.value()), usually(isMade), rarely(isNotMade));
            m_out.appendTo(isMade);
            join.add(made);
            if (!isGeneralArmOnOffer(isNotMade)) {
                m_out.appendTo(isNotMade);
                join.add(convert(callBuiltinFunction(value, effectsOfBuiltinCall(value)), Type::engineClass(m_builtins.arrayClass()), Rep::Cell));
            }
        }
    });
    return join.finish().as<CellPointer>();
}

// `{}`, or `{ a: x }`, that has been made in this block and that nothing has had since but what defines its properties. It inherits from Object.prototype, which has one setter.
bool Lowering::isNewPlainObject(ValueID id) const
{
    auto& values = m_currentBlock->values;
    for (size_t index = m_currentIndexInBlock; index--;) {
        if (values[index] == id)
            return m_function.at(id).opcode == Opcode::NewObject;
        auto& value = m_function.at(values[index]);
        if (isDefineField(value) && value.arguments[0] == id && value.arguments[2] != id && m_function.at(value.arguments[1]).opcode == Opcode::String)
            continue;
        bool usesObject = false;
        value.forEachChild([&](ValueID child) {
            usesObject |= child == id;
        });
        if (usesObject)
            return false;
    }
    return false;
}

// Source by source, as DFG::ByteCodeParser has it. A store to a new plain object runs no code, so what has been found out of a source holds until its last field has been read.
Operand Lowering::lowerAssign(ValueID result, const Value& value)
{
    auto& type = typeOf(result);
    auto& anything = m_program.at(m_program.types.anything);
    Rep rep = repOf(type);
    LBasicBlock noObject = nullptr;
    Join join(m_out, rep, b3TypeOf(rep));
    ValueID into = value.arguments[0];
    bool isNew = isNewPlainObject(into);
    unsigned firstSource = 1;
    CellPointer target;
    if (isMadeHere(into)) {
        // The target is made of its first source.
        ValueID source = value.arguments[firstSource++];
        Join made(m_out, Rep::Cell, b3TypeOf(Rep::Cell));
        byKeyOrder(source, keyOrdersExpectedOf(source), [&](CellPointer cell, const KeyOrder& order) {
            auto& fields = m_program.at(order.layout).fields;
            Vector<StringID, 8> names;
            Vector<Boxed, 8> values;
            for (uint16_t index : order.fields) {
                names.append(fields[index].name);
                values.append(readField(cell, fields[index], anything).as<Boxed>());
            }
            made.add(newObjectLiteral(names.span(), values.span()));
        }, [&] {
            made.add(AOT_TYPED_OPERATION(ObjectAssignToNew, in<Boxed>(source)));
        });
        target = made.finish().as<CellPointer>();
        defineObjectMadeLater(into, target);
        RELEASE_ASSERT(isNew);
    } else {
        noObject = newBlockForGeneralCall(value);
        target = objectOrElse(into, noObject);
    }
    for (unsigned i = firstSource; i < value.arguments.size(); ++i) {
        ValueID source = value.arguments[i];
        LBasicBlock continuation = m_out.newBlock();
        byKeyOrder(source, isNew ? keyOrdersExpectedOf(source) : Vector<KeyOrder, 2> { }, [&](CellPointer cell, const KeyOrder& order) {
            auto& fields = m_program.at(order.layout).fields;
            for (uint16_t index : order.fields)
                emitPutById(box(target), fields[index].name, readField(cell, fields[index], anything).as<Boxed>());
            m_out.jump(continuation);
        }, [&] {
            AOT_TYPED_OPERATION(ObjectAssignFrom, target, in<Boxed>(source));
            m_out.jump(continuation);
        });
        m_out.appendTo(continuation);
    }
    if (isNew && hasOneUse(into))
        target = finishObjectLiteral(target);
    join.add(yieldAsTableSays(target, m_program.at(m_program.types.object), value, type));
    if (noObject && !isGeneralArmOnOffer(noObject)) {
        m_out.appendTo(noObject);
        join.add(yieldAsTableSays(callBuiltinFunction(value, effectsOfBuiltinCall(value)), anything, value, type));
    }
    return join.finish();
}

// SameValue is strict equality, but for numbers: NaN is itself, and the two zeros differ.
Word32 Lowering::lowerSameValue(ValueID a, ValueID b)
{
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    auto areSameNumber = [&](Number x, Number y) {
        return logicalOr(equal(bitsOf(x), bitsOf(y)), logicalAnd(notEqualOrUnordered(x, x), notEqualOrUnordered(y, y)));
    };
    if (left.isInteger() && right.isInteger())
        return equal(get(a).as<Integer>(), get(b).as<Integer>());
    if (left.isNumber() && right.isNumber())
        return areSameNumber(in<Number>(a), in<Number>(b));
    if (!(left.primitives & right.primitives & Primitive::Number))
        return lowerStrictEq(a, b);
    Boxed x = in<Boxed>(a);
    Boxed y = in<Boxed>(b);
    LBasicBlock numbers = m_out.newBlock();
    LBasicBlock others = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    m_out.branch(logicalAnd(isNumber(x), isNumber(y)).value(), unsure(numbers), unsure(others));
    m_out.appendTo(numbers);
    join.add(areSameNumber(Number(numberToDouble(x.value())), Number(numberToDouble(y.value()))));
    m_out.appendTo(others);
    join.add(lowerStrictEq(a, b));
    return join.finish().as<Word32>();
}

std::optional<Operand> Lowering::lowerInlineObject(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    auto& type = typeOf(result);
    switch (which) {
    case InlineBuiltin::ObjectHasOwn: {
        Word32 answer = lowerHasOwnProperty(value.arguments[0], value.arguments[1]);
        Operand yielded = yieldBoolean(answer, type);
        m_truthinessPhis.add(result.index(), Truthiness { yielded.value, answer.value() });
        return yielded;
    }
    case InlineBuiltin::ObjectKeys:
    case InlineBuiltin::ObjectGetOwnPropertyNames:
    case InlineBuiltin::ObjectGetOwnPropertySymbols:
    case InlineBuiltin::ReflectOwnKeys:
        return ensure(lowerOwnPropertyKeys(which, value.arguments[0]), Type::engineClass(m_builtins.arrayClass()), type);
    case InlineBuiltin::ObjectValues:
    case InlineBuiltin::ObjectEntries:
        return ensure(lowerValuesOrEntries(which, value), Type::engineClass(m_builtins.arrayClass()), type);
    case InlineBuiltin::ObjectAssign:
        return lowerAssign(result, value);
    case InlineBuiltin::ObjectGetOwnPropertyDescriptor:
        return lowerGetOwnPropertyDescriptor(result, value);
    case InlineBuiltin::ObjectCreate:
        return lowerCreate(result, value);
    case InlineBuiltin::ObjectGetPrototypeOf:
    case InlineBuiltin::ReflectGetPrototypeOf:
        return lowerGetPrototypeOf(result, value);
    case InlineBuiltin::ObjectIs:
        return yieldBoolean(lowerSameValue(value.arguments[0], value.arguments[1]), type);
    // ToObject of an object is the object.
    case InlineBuiltin::ObjectCall: {
        Rep rep = repOf(type);
        LBasicBlock slowPath = newBlockForGeneralCall(value);
        Join join(m_out, rep, b3TypeOf(rep));
        join.add(yieldAsTableSays(objectOrElse(value.arguments[0], slowPath), m_program.at(m_program.types.object), value, type));
        if (!isGeneralArmOnOffer(slowPath)) {
            m_out.appendTo(slowPath);
            join.add(yieldAsTableSays(callBuiltinFunction(value, effectsOfBuiltinCall(value)), m_program.at(m_program.types.anything), value, type));
        }
        return join.finish();
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
