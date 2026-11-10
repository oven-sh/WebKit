/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinIDTypes.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinModules.h"
#include "AOTBuiltinTable.h"
#include "AOTCompactTable.h"
#include "AOTErrorMessages.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTPropertyWalk.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "ArrayConstructor.h"
#include "ClonedArguments.h"
#include "CommonSlowPathsInlines.h"
#include "IteratorOperations.h"
#include "JSArrayIterator.h"
#include "JSArrayIteratorInlines.h"
#include "JSAsyncFromSyncIterator.h"
#include "JSCInlines.h"
#include "JSMapIterator.h"
#include "JSPropertyNameEnumeratorInlines.h"
#include "JSSetIterator.h"
#include "JSStringIteratorInlines.h"
#include <array>
#include <span>

namespace JSC { namespace AOT {

static_assert(sizeof(UCPURegister) == sizeof(uint64_t), "operationAOTEnumeratorNext returns a position in one register.");

JSPropertyNameEnumerator* enumeratorFor(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (value.isUndefinedOrNull())
        return vm.emptyPropertyNameEnumerator();
    JSObject* object = value.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (Object* compiled = compiledObjectOrNull(object))
        RELEASE_AND_RETURN(scope, compiled->propertyNameEnumerator(globalObject));
    PropertyWalk::materializeLazyProperties(vm, object);
    if (object->structure()->isDictionary()) [[unlikely]]
        flattenForListing(vm, object);
    RELEASE_AND_RETURN(scope, JSC::propertyNameEnumerator(globalObject, object));
}

EnumeratedKey findEnumeratedKey(JSGlobalObject* globalObject, JSPropertyNameEnumerator* enumerator, JSValue value, uint64_t position)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint64_t indexedLength = enumerator->indexedLength();
    uint64_t end = indexedLength + enumerator->sizeOfPropertyNames();
    if (position >= end)
        return { nullptr, end };
    JSObject* base = value.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    uint32_t index = 0;
    JSPropertyNameEnumerator::Flag mode = JSPropertyNameEnumerator::InitMode;
    if (position > indexedLength) {
        index = static_cast<uint32_t>(position - indexedLength - 1);
        mode = JSPropertyNameEnumerator::OwnStructureMode;
    } else if (position) {
        index = static_cast<uint32_t>(position - 1);
        mode = JSPropertyNameEnumerator::IndexedMode;
    }
    JSString* key = enumerator->computeNext(globalObject, base, index, mode);
    RETURN_IF_EXCEPTION(scope, { });
    if (!key)
        return { nullptr, end };
    RELEASE_ASSERT(mode == JSPropertyNameEnumerator::IndexedMode || mode == JSPropertyNameEnumerator::OwnStructureMode || mode == JSPropertyNameEnumerator::GenericMode);
    return { key, mode == JSPropertyNameEnumerator::IndexedMode ? index : indexedLength + index };
}

static bool isIteratedAsListsAre(JSArray* list)
{
    if (list->isIteratorProtocolFastAndNonObservable())
        return true;
    JSGlobalObject* realm = list->realm();
    VM& vm = realm->vm();
    if (!realm->isArrayPrototypeIteratorProtocolFastAndNonObservable() || list->structure()->mayInterceptIndexedAccesses() || list->getDirectOffset(vm, vm.propertyNames->iteratorSymbol) != invalidOffset)
        return false;
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    for (JSValue above = list->getPrototypeDirect(); above != JSValue(realm->arrayPrototype());) {
        JSObject* object = above.getObject();
        if (!object || (object->type() != ObjectType && object->type() != FinalObjectType) || object->structure()->typeInfo().overridesGetPrototype())
            return false;
        PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isFound = object->methodTable()->getOwnPropertySlot(object, realm, vm.propertyNames->iteratorSymbol, slot);
        scope.assertNoException();
        if (isFound)
            return false;
        above = object->getPrototypeDirect();
    }
    return true;
}

static bool isReadByIndexWhereNoHoleIsMet(JSValue iterable)
{
    JSArray* list = iterable.isCell() ? dynamicDowncast<JSArray>(iterable.asCell()) : nullptr;
    if (!list || !isIteratedAsListsAre(list))
        return false;
    switch (list->indexingType() & IndexingShapeMask) {
    case UndecidedShape:
    case Int32Shape:
    case DoubleShape:
    case ContiguousShape:
        return true;
    default:
        return false;
    }
}

namespace {

bool isReadByIndexUnseen(JSValue iterable)
{
    return isReadByIndexWhereNoHoleIsMet(iterable) && !asArray(iterable)->structure()->holesMustForwardToPrototype(asArray(iterable));
}

enum class WordingOfIteration : uint8_t { Pattern, Spread };

class IterationInRunTime {
public:
    IterationInRunTime(Instance& instance, JSGlobalObject* globalObject)
        : m_instance(instance)
        , m_globalObject(globalObject)
        , m_vm(globalObject->vm())
    {
    }

    bool open(JSValue iterable, WordingOfIteration);
    bool tryOpenUnseen(JSValue iterable);
    bool step(JSValue&);
    bool tryStepToEndUnseen(MarkedArgumentBuffer& values);
    void close();

private:
    enum class Stepper : uint8_t { Protocol, ArrayIterator, MapIterator, SetIterator, StringIterator };

    bool isOwnFunctionOfEngine(JSValue, BuiltinID);
    Stepper findStepper();

    Instance& m_instance;
    JSGlobalObject* m_globalObject;
    VM& m_vm;
    JSValue m_iterator;
    JSValue m_next;
    Stepper m_stepper { Stepper::Protocol };
    WordingOfIteration m_wording { WordingOfIteration::Pattern };
};

bool IterationInRunTime::tryOpenUnseen(JSValue iterable)
{
    if (!iterable.isCell())
        return false;
    JSCell* cell = iterable.asCell();
    switch (cell->type()) {
    case ArrayType:
        if (!isIteratedAsListsAre(uncheckedDowncast<JSArray>(cell)))
            return false;
        m_iterator = JSArrayIterator::create(m_vm, m_globalObject->arrayIteratorStructure(), uncheckedDowncast<JSArray>(cell), IterationKind::Values);
        m_stepper = Stepper::ArrayIterator;
        return true;
    case JSMapType:
        if (uncheckedDowncast<JSMap>(cell)->realm() != m_globalObject || !uncheckedDowncast<JSMap>(cell)->isIteratorProtocolFastAndNonObservable())
            return false;
        m_iterator = JSMapIterator::create(m_vm, m_globalObject->mapIteratorStructure(), uncheckedDowncast<JSMap>(cell), IterationKind::Entries);
        m_stepper = Stepper::MapIterator;
        return true;
    case JSSetType:
        if (!uncheckedDowncast<JSSet>(cell)->isIteratorProtocolFastAndNonObservable())
            return false;
        m_iterator = JSSetIterator::create(m_vm, m_globalObject->setIteratorStructure(), uncheckedDowncast<JSSet>(cell), IterationKind::Values);
        m_stepper = Stepper::SetIterator;
        return true;
    case StringType:
        if (!m_globalObject->isStringPrototypeIteratorProtocolFastAndNonObservable())
            return false;
        m_iterator = JSStringIterator::create(m_vm, m_globalObject->stringIteratorStructure(), uncheckedDowncast<JSString>(cell));
        m_stepper = Stepper::StringIterator;
        return true;
    default:
        return false;
    }
}

bool IterationInRunTime::isOwnFunctionOfEngine(JSValue value, BuiltinID builtin)
{
    return value.isCell() && value.asCell() == m_instance.state->builtinCells->findObject(builtin);
}

IterationInRunTime::Stepper IterationInRunTime::findStepper()
{
    if (!m_iterator.isCell())
        return Stepper::Protocol;
    switch (m_iterator.asCell()->type()) {
    case JSMapIteratorType:
        return isOwnFunctionOfEngine(m_next, Builtins::MapIteratorPrototypeNext) ? Stepper::MapIterator : Stepper::Protocol;
    case JSSetIteratorType:
        return isOwnFunctionOfEngine(m_next, Builtins::SetIteratorPrototypeNext) ? Stepper::SetIterator : Stepper::Protocol;
    case JSStringIteratorType:
        return isOwnFunctionOfEngine(m_next, Builtins::StringIteratorPrototypeNext) ? Stepper::StringIterator : Stepper::Protocol;
    case JSArrayIteratorType:
        return isJSArray(uncheckedDowncast<JSArrayIterator>(m_iterator.asCell())->iteratedObject()) && isOwnFunctionOfEngine(m_next, Builtins::ArrayIteratorPrototypeNext) ? Stepper::ArrayIterator : Stepper::Protocol;
    default:
        return Stepper::Protocol;
    }
}

bool IterationInRunTime::open(JSValue iterable, WordingOfIteration wording)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    m_wording = wording;
    if (tryOpenUnseen(iterable))
        return true;

    JSValue method = iterable.get(m_globalObject, m_vm.propertyNames->iteratorSymbol);
    RETURN_IF_EXCEPTION(scope, false);
    switch (wording) {
    case WordingOfIteration::Pattern:
        if (auto validation = validateIterable(m_vm, iterable, method); validation != IterableValidationResult::Valid) [[unlikely]] {
            throwTypeError(m_globalObject, scope, getIteratorErrorMessage(validation, iterable));
            return false;
        }
        break;
    case WordingOfIteration::Spread:
        if (!method.isCallable()) [[unlikely]] {
            throwException(m_globalObject, scope, createSpreadIteratorNotFunctionError(m_globalObject));
            return false;
        }
        break;
    }

    JSValue iterator = iterable;
    if (!iterable.isObject() || !isOwnFunctionOfEngine(method, Builtins::IteratorPrototypeSymbolIterator)) {
        iterator = call(m_globalObject, method, JSC::getCallData(method), iterable, ArgList());
        RETURN_IF_EXCEPTION(scope, false);
        if (wording == WordingOfIteration::Pattern && !iterator.isObject()) [[unlikely]] {
            throwException(m_globalObject, scope, createIteratorResultNotObjectError(m_globalObject));
            return false;
        }
    }
    m_iterator = iterator;
    m_next = m_iterator.get(m_globalObject, m_vm.propertyNames->next);
    RETURN_IF_EXCEPTION(scope, false);
    m_stepper = findStepper();
    return true;
}

bool IterationInRunTime::step(JSValue& value)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    switch (m_stepper) {
    case Stepper::ArrayIterator:
        RELEASE_AND_RETURN(scope, uncheckedDowncast<JSArrayIterator>(m_iterator.asCell())->next(m_globalObject, value));
    case Stepper::MapIterator:
        RELEASE_AND_RETURN(scope, uncheckedDowncast<JSMapIterator>(m_iterator.asCell())->next(m_globalObject, value));
    case Stepper::SetIterator:
        RELEASE_AND_RETURN(scope, uncheckedDowncast<JSSetIterator>(m_iterator.asCell())->next(m_globalObject, value));
    case Stepper::StringIterator: {
        JSString* piece = uncheckedDowncast<JSStringIterator>(m_iterator.asCell())->nextWithAdvance(m_globalObject, m_vm);
        RETURN_IF_EXCEPTION(scope, false);
        value = piece;
        return piece;
    }
    case Stepper::Protocol:
        break;
    }

    if (!m_next.isCallable()) [[unlikely]] {
        throwNotAFunctionError(m_globalObject, scope, m_next);
        return false;
    }
    JSValue result = call(m_globalObject, m_next, JSC::getCallData(m_next), m_iterator, ArgList());
    RETURN_IF_EXCEPTION(scope, false);
    if (!result.isObject()) [[unlikely]] {
        throwException(m_globalObject, scope, m_wording == WordingOfIteration::Spread ? createSpreadResultNotObjectError(m_globalObject) : createIteratorResultNotObjectError(m_globalObject));
        return false;
    }
    JSValue done = asObject(result)->get(m_globalObject, m_vm.propertyNames->done);
    RETURN_IF_EXCEPTION(scope, false);
    if (done.toBoolean(m_globalObject))
        return false;
    value = asObject(result)->get(m_globalObject, m_vm.propertyNames->value);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

bool IterationInRunTime::tryStepToEndUnseen(MarkedArgumentBuffer& values)
{
    if (m_stepper != Stepper::ArrayIterator)
        return false;
    auto* iterator = uncheckedDowncast<JSArrayIterator>(m_iterator.asCell());
    auto* array = uncheckedDowncast<JSArray>(iterator->iteratedObject());
    IterationKind kind = iterator->kind();
    int64_t first = iterator->index();
    uint32_t end = array->length();
    if (kind == IterationKind::Entries || first == JSArrayIterator::doneIndex || first >= end)
        return false;
    if (kind == IterationKind::Values) {
        for (uint32_t index = first; index < end; ++index) {
            if (!array->canGetIndexQuickly(index))
                return false;
        }
    }
    iterator->setIndex(JSArrayIterator::doneIndex);
    values.fill(m_vm, end - first, [&](JSValue* place) {
        for (uint32_t index = first; index < end; ++index)
            *place++ = kind == IterationKind::Keys ? jsNumber(index) : array->getIndexQuickly(index);
    });
    return true;
}

void IterationInRunTime::close()
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    JSValue method = m_iterator.get(m_globalObject, m_vm.propertyNames->returnKeyword);
    RETURN_IF_EXCEPTION(scope, void());
    if (method.isUndefinedOrNull())
        return;
    if (!method.isCallable()) [[unlikely]] {
        throwNotAFunctionError(m_globalObject, scope, method);
        return;
    }
    JSValue result = call(m_globalObject, method, JSC::getCallData(method), m_iterator, ArgList());
    RETURN_IF_EXCEPTION(scope, void());
    if (!result.isObject()) [[unlikely]]
        throwException(m_globalObject, scope, createIteratorResultNotObjectError(m_globalObject));
}

} // namespace

static JSFunction* makeFunctionOfLibrary(JSGlobalObject*, BuiltinID);

static ALWAYS_INLINE JSFunction* functionOfLibrary(JSGlobalObject* globalObject, JSCell*& kept, BuiltinID helper)
{
    if (!kept) [[unlikely]]
        kept = makeFunctionOfLibrary(globalObject, helper);
    return uncheckedDowncast<JSFunction>(kept);
}

static ALWAYS_INLINE bool mayBeSteppedByRunTime(JSValue iterable)
{
    if (!iterable.isCell())
        return true;
    switch (iterable.asCell()->type()) {
    case StringType:
    case ArrayType:
    case DerivedArrayType:
    case JSMapType:
    case JSSetType:
    case JSArrayIteratorType:
    case JSMapIteratorType:
    case JSSetIteratorType:
    case JSStringIteratorType:
        return true;
    default:
        return false;
    }
}

template<typename Collection>
static bool tryTakeOutOfTable(JSGlobalObject* globalObject, Collection* collection, std::span<EncodedJSValue> elements)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (collection->realm() != globalObject || !collection->isIteratorProtocolFastAndNonObservable())
        return false;
    JSCell* table = collection->storage();
    if (!table || elements.empty())
        return true;
    size_t numberTaken = 0;
    auto take = [&](JSValue key, JSValue value) ALWAYS_INLINE_LAMBDA {
        if constexpr (std::is_same_v<Collection, JSMap>) {
            JSArray* pair = constructArrayPair(globalObject, key, value);
            if (scope.exception()) [[unlikely]]
                return false;
            elements[numberTaken++] = JSValue::encode(pair);
        } else {
            UNUSED_PARAM(value);
            elements[numberTaken++] = JSValue::encode(key);
        }
        return numberTaken < elements.size();
    };
    if (Collection::Helper::isCompact(*table)) {
        uint32_t& deletedPrefix = CompactTable::deletedPrefixOf(collection);
        CompactTable::forEachEntryFrom(vm, table, deletedPrefix, [&](uint32_t entry, JSValue key, JSValue value) ALWAYS_INLINE_LAMBDA {
            if (!numberTaken)
                deletedPrefix = entry;
            return take(key, value);
        });
        return true;
    }
    typename Collection::Helper::Entry entry = 0;
    while (true) {
        auto found = Collection::Helper::transitAndNext(vm, Collection::Helper::ordinary(*table), entry);
        if (!found.storage || !take(found.key, found.value))
            break;
        table = found.storage;
        entry = found.entry + 1;
    }
    return true;
}

static void takeCharacters(Instance& instance, JSString* string, std::span<EncodedJSValue> elements)
{
    JSGlobalObject* globalObject = instance.globalObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto view = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    StringView characters = view;
    unsigned at = 0;
    for (EncodedJSValue& element : elements) {
        if (at >= characters.length())
            break;
        char16_t unit = characters[at++];
        if (U16_IS_LEAD(unit) && at < characters.length() && U16_IS_TRAIL(characters[at])) {
            std::array<char16_t, 2> pair { unit, characters[at++] };
            element = JSValue::encode(jsNontrivialString(vm, String(std::span<const char16_t> { pair })));
        } else
            element = JSValue::encode(stringOfCharacter(instance, unit));
    }
    ensureStillAliveHere(string);
}

static void destructureIterable(Instance& instance, JSGlobalObject* globalObject, JSValue iterable, std::span<EncodedJSValue> elements)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isReadByIndexWhereNoHoleIsMet(iterable)) {
        JSArray* list = asArray(iterable);
        size_t length = std::min<size_t>(list->length(), elements.size());
        size_t index = 0;
        for (; index < length; ++index) {
            JSValue element = list->tryGetIndexQuickly(static_cast<unsigned>(index));
            if (!element && list->structure()->holesMustForwardToPrototype(list))
                break;
            elements[index] = JSValue::encode(element ? element : jsUndefined());
        }
        if (index == length) {
            std::ranges::fill(elements.subspan(length), JSValue::encode(jsUndefined()));
            return;
        }
    }
    for (EncodedJSValue& element : elements)
        element = JSValue::encode(jsUndefined());

    if (iterable.isCell()) {
        JSCell* cell = iterable.asCell();
        bool isTaken = false;
        if (cell->type() == JSSetType)
            isTaken = tryTakeOutOfTable(globalObject, uncheckedDowncast<JSSet>(cell), elements);
        else if (cell->type() == JSMapType)
            isTaken = tryTakeOutOfTable(globalObject, uncheckedDowncast<JSMap>(cell), elements);
        else if (cell->isString() && globalObject->isStringPrototypeIteratorProtocolFastAndNonObservable()) {
            takeCharacters(instance, asString(cell), elements);
            isTaken = true;
        }
        RETURN_IF_EXCEPTION(scope, void());
        if (isTaken)
            return;
    }

    IterationInRunTime iteration(instance, globalObject);
    bool isOpenedUnseen = iteration.tryOpenUnseen(iterable);
    if (!isOpenedUnseen) {
        if (!elements.empty() && !mayBeSteppedByRunTime(iterable)) {
            if (JSFunction* takes = functionOfLibrary(globalObject, instance.iteration.functionThatTakesFromIterable, Builtins::CompilerHelpersTakeFromIterable)) {
                MarkedArgumentBuffer arguments;
                arguments.append(iterable);
                arguments.append(jsNumber(static_cast<uint32_t>(elements.size())));
                ASSERT(!arguments.hasOverflowed());
                JSValue taken = call(globalObject, takes, JSC::getCallData(takes), jsUndefined(), arguments);
                RETURN_IF_EXCEPTION(scope, void());
                JSArray* list = asArray(taken);
                if (list->length() && list->getIndexQuickly(0) == JSValue(vm.orderedHashTableSentinel())) [[unlikely]] {
                    if (list->length() == 1)
                        throwTypeError(globalObject, scope, getIteratorErrorMessage(validateIterable(vm, iterable, jsUndefined()), iterable));
                    else
                        throwNotAFunctionError(globalObject, scope, list->getIndexQuickly(1));
                    return;
                }
                size_t length = std::min<size_t>(list->length(), elements.size());
                for (size_t index = 0; index < length; ++index)
                    elements[index] = JSValue::encode(list->getIndexQuickly(static_cast<unsigned>(index)));
                return;
            }
        }
        bool isOpen = iteration.open(iterable, WordingOfIteration::Pattern);
        EXCEPTION_ASSERT(!!scope.exception() == !isOpen);
        if (!isOpen)
            return;
    }
    for (EncodedJSValue& element : elements) {
        JSValue value;
        bool hasValue = iteration.step(value);
        RETURN_IF_EXCEPTION(scope, void());
        if (!hasValue)
            return;
        element = JSValue::encode(value);
    }
    if (isOpenedUnseen)
        return;
    RELEASE_AND_RETURN(scope, iteration.close());
}

void initializeIterationWords(Instance& instance)
{
    instance.iteration.emptyEnumerator = instance.vm->emptyPropertyNameEnumerator();
    instance.iteration.functionThatAppendsIterable = nullptr;
    instance.iteration.functionThatTakesFromIterable = nullptr;
}

static uint32_t placeAmongListsWithoutNames(Structure* structure)
{
    static_assert(Int32Shape >> 1 == 2 && DoubleShape >> 1 == 3 && ContiguousShape >> 1 == 4 && ArrayWords::numberOfStructuresWithoutNames == 4);
    return (structure->indexingType() & IndexingShapeMask) >> 1 & (ArrayWords::numberOfStructuresWithoutNames - 1);
}

static JSPropertyNameEnumerator* enumeratorOfListWithoutNames(Instance& instance, JSArray* list)
{
    VM& vm = *instance.vm;
    Structure* structure = list->structure();
    uint32_t structureID = structure->id().bits();
    uint32_t place = placeAmongListsWithoutNames(structure);
    if (instance.arrays.structuresOfListsWithoutNames[place] != structureID || instance.arrays.chainsAboveListsWithoutNames[place] != structure->prototypeChain(vm, instance.globalObject, list))
        return nullptr;
    uint32_t length = list->getEnumerableLength();
    if (length != list->getArrayLength())
        return nullptr;
    if (!length)
        return vm.emptyPropertyNameEnumerator();
    EnumeratorCacheEntry& entry = instance.objects.enumerators[indexOfCachedEnumerator(structureID, length)];
    if (entry.structureID == structureID && entry.orderWord == length)
        return uncheckedDowncast<JSPropertyNameEnumerator>(entry.enumerator);
    JSPropertyNameEnumerator* enumerator = JSPropertyNameEnumerator::tryCreate(vm, structure, length, 0, PropertyNameArrayBuilder(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude));
    if (enumerator)
        entry = { length, structureID, enumerator };
    return enumerator;
}

static bool otherNamesRequireAnotherStructure(Structure* structure)
{
    if (structure->isDictionary() || hasIndexedProperties(structure->indexingType()))
        return false;
    JSType type = structure->typeInfo().type();
    return !structure->typeInfo().overridesAnyFormOfGetOwnPropertyNames() || type == ArrayType || type == DerivedArrayType;
}

static void rememberListsWithoutNames(Instance& instance, JSArray* list, JSPropertyNameEnumerator* enumerator)
{
    Structure* structure = list->structure();
    bool listsNothing = enumerator == instance.vm->emptyPropertyNameEnumerator();
    if (structure->isDictionary() || (!listsNothing && (!enumerator->indexedLength() || enumerator->sizeOfPropertyNames() || enumerator->cachedStructureID() != structure->id())))
        return;
    bool sawPolyProto = false;
    if (normalizePrototypeChain(instance.globalObject, list, sawPolyProto) == InvalidPrototypeChain || sawPolyProto)
        return;
    StructureChain* chain = structure->prototypeChain(*instance.vm, instance.globalObject, list);
    for (StructureID* above = chain->head(); *above; ++above) {
        if (!otherNamesRequireAnotherStructure(above->decode()))
            return;
    }
    uint32_t place = placeAmongListsWithoutNames(structure);
    instance.arrays.structuresOfListsWithoutNames[place] = structure->id().bits();
    instance.arrays.chainsAboveListsWithoutNames[place] = chain;
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetPropertyEnumerator, JSCell*, (Instance* instance, EncodedJSValue object))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetPropertyEnumerator);
    JSValue value = JSValue::decode(object);
    JSArray* list = value.isCell() ? dynamicDowncast<JSArray>(value.asCell()) : nullptr;
    if (list) {
        if (JSPropertyNameEnumerator* enumerator = enumeratorOfListWithoutNames(*instance, list))
            OPERATION_RETURN(scope, enumerator);
    }
    JSPropertyNameEnumerator* enumerator = enumeratorFor(globalObject, value);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (list)
        rememberListsWithoutNames(*instance, list, enumerator);
    OPERATION_RETURN(scope, enumerator);
}

JSC_DEFINE_JIT_OPERATION(operationAOTEnumeratorNext, UGPRPair, (Instance* instance, JSCell* enumerator, EncodedJSValue object, uint64_t position))
{
    constexpr Entry entry = Entry::operationAOTEnumeratorNext;
    AOT_OPERATION_BEGIN(instance, entry);
    EnumeratedKey found = findEnumeratedKey(globalObject, cellArgument<JSPropertyNameEnumerator>(enumerator, entry), JSValue::decode(object), position);
    OPERATION_RETURN(scope, makeUGPRPair(std::bit_cast<UCPURegister>(found.key), found.position));
}

JSC_DEFINE_JIT_OPERATION(operationAOTEnumeratorGetByVal, EncodedJSValue, (Instance* instance, EncodedJSValue object, EncodedJSValue property, JSCell*))
{
    constexpr Entry entry = Entry::operationAOTEnumeratorGetByVal;
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, entry);
    JSValue key = JSValue::decode(property);
    Identifier identifier = cellArgument<JSString>(key.isCell() ? key.asCell() : nullptr, entry)->toIdentifier(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(objectArgument(object, entry)->get(globalObject, identifier)));
}

static NEVER_INLINE OperationReturnType<void> JIT_OPERATION_ATTRIBUTES destructureAnyIterable(Instance* instance, EncodedJSValue iterable, uint32_t count, EncodedJSValue* results)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDestructureIterable);
    if (JSValue::decode(iterable).isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createIterationOfNullishError(globalObject, JSValue::decode(iterable)));
        OPERATION_RETURN(scope);
    }
    destructureIterable(*instance, globalObject, JSValue::decode(iterable), unsafeMakeSpan(results, count));
    OPERATION_RETURN(scope);
}

static ALWAYS_INLINE bool tryTakeLatin1Characters(Instance& instance, JSValue iterable, std::span<EncodedJSValue> elements)
{
    if (!iterable.isString() || !instance.globalObject->isStringPrototypeIteratorProtocolFastAndNonObservable())
        return false;
    const StringImpl* impl = asString(iterable)->tryGetValueImpl();
    if (!impl || !impl->is8Bit())
        return false;
    std::span<const Latin1Character> characters = impl->span8();
    for (size_t index = 0; index < elements.size(); ++index)
        elements[index] = JSValue::encode(index < characters.size() ? JSValue(jsSingleCharacterString(*instance.vm, characters[index])) : jsUndefined());
    return true;
}

JSC_DEFINE_JIT_OPERATION(operationAOTDestructureIterable, void, (Instance* instance, EncodedJSValue iterable, uint32_t count, EncodedJSValue* results))
{
    if (tryTakeLatin1Characters(*instance, JSValue::decode(iterable), unsafeMakeSpan(results, count)))
        return { nullptr };
    AOT_MUST_TAIL_CALL return destructureAnyIterable(instance, iterable, count, results);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCreateAsyncFromSyncIterator, JSCell*, (Instance* instance, JSCell* iterator, EncodedJSValue next))
{
    constexpr Entry entry = Entry::operationAOTCreateAsyncFromSyncIterator;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferTerminationForAWhile deferTermination(vm);
    JSObject* syncIterator = cellArgument<JSObject>(iterator, entry);
    JSValue nextMethod = JSValue::decode(next);
    IterationMode mode = IterationMode::Generic;
    if (auto* arrayIterator = dynamicDowncast<JSArrayIterator>(syncIterator); arrayIterator && nextMethod.isCell() && nextMethod.asCell() == instance->state->builtinCells->findObject(Builtins::ArrayIteratorPrototypeNext)) {
        JSValue iterated = arrayIterator->internalField(JSArrayIterator::Field::IteratedObject).get();
        if (iterated.isCell() && isJSArray(iterated.asCell())) {
            switch (arrayIterator->kind()) {
            case IterationKind::Keys:
                mode = IterationMode::FastArrayKeys;
                break;
            case IterationKind::Values:
                mode = IterationMode::FastArrayValues;
                break;
            case IterationKind::Entries:
                mode = IterationMode::FastArrayEntries;
                break;
            }
        }
    }
    RELEASE_AND_RETURN(scope, JSAsyncFromSyncIterator::create(vm, globalObject->asyncFromSyncIteratorStructure(), syncIterator, nextMethod, mode));
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowNotIterable, void, (Instance* instance, EncodedJSValue encodedIterable, uint32_t isSpread))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowNotIterable);
    JSValue iterable = JSValue::decode(encodedIterable);
    if (!isSpread && iterable.isUndefinedOrNull())
        throwException(globalObject, scope, createIterationOfNullishError(globalObject, iterable));
    else if (isSpread)
        throwException(globalObject, scope, createSpreadIteratorNotFunctionError(globalObject));
    else
        throwTypeError(globalObject, scope, getIteratorErrorMessage(validateIterable(vm, iterable, jsUndefined()), iterable));
    OPERATION_RETURN(scope);
}

size_t numberOfElementsSpreadFrom(JSGlobalObject* globalObject, JSString* string)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto text = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    size_t count = text->length();
    if (text->is8Bit())
        return count;
    std::span<const char16_t> units = text->span16();
    for (size_t i = 0; i + 1 < units.size(); ++i) {
        if (U16_IS_LEAD(units[i]) && U16_IS_TRAIL(units[i + 1])) {
            --count;
            ++i;
        }
    }
    return count;
}

static void throwIfTooLongForList(JSGlobalObject* globalObject, JSCell* source)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    size_t count = 0;
    if (auto* view = dynamicDowncast<JSArrayBufferView>(source)) {
        if (!view->isOutOfBounds() && view->isIteratorProtocolFastAndNonObservable())
            count = view->length();
    } else if (globalObject->isStringPrototypeIteratorProtocolFastAndNonObservable()) {
        JSString* string = asString(source);
        count = string->length();
        if (count > IndexingHeader::maximumLength && !string->is8Bit()) {
            count = numberOfElementsSpreadFrom(globalObject, string);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
    if (count > IndexingHeader::maximumLength)
        throwOutOfMemoryError(globalObject, scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowIfTooLongForList, void, (Instance* instance, JSCell* source))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowIfTooLongForList);
    throwIfTooLongForList(globalObject, source);
    OPERATION_RETURN(scope);
}

static JSFunction* makeFunctionOfLibrary(JSGlobalObject* globalObject, BuiltinID helper)
{
    for (const RuntimeFunctionData& function : BuiltinTable::singleton().runtimeFunctions()) {
        if (std::ranges::find(function.implements, helper) != function.implements.end())
            return builtinFunction(globalObject, function.id.index());
    }
    return nullptr;
}


static std::optional<IndexingType> findIndexingTypeOfValues(JSArray* target, std::span<const EncodedJSValue> values)
{
    bool holdsInt32Alone = true;
    bool holdsNumbersAlone = true;
    for (EncodedJSValue encoded : values) {
        if (!encoded)
            return std::nullopt;
        JSValue value = JSValue::decode(encoded);
        holdsInt32Alone &= value.isInt32();
        holdsNumbersAlone &= value.isNumber() && value.asNumber() == value.asNumber();
    }
    return holdsNumbersAlone && hasDouble(target->indexingType()) ? ArrayWithDouble : holdsInt32Alone ? ArrayWithInt32 : ArrayWithContiguous;
}

static bool tryAppendElementsOfArguments(JSGlobalObject* globalObject, JSArray* target, ClonedArguments* arguments)
{
    if (!arguments->isIteratorProtocolFastAndNonObservable() || !hasContiguous(arguments->indexingType()))
        return false;
    uint32_t length = arguments->getDirect(clonedArgumentsLengthPropertyOffset).asInt32();
    Butterfly* butterfly = arguments->butterfly();
    if (length > butterfly->publicLength() || static_cast<uint64_t>(target->length()) + length > MAX_STORAGE_VECTOR_LENGTH)
        return false;
    std::span<const EncodedJSValue> elements(std::bit_cast<const EncodedJSValue*>(butterfly->contiguous().data()), length);
    std::optional<IndexingType> kind = findIndexingTypeOfValues(target, elements);
    return kind && (!length || target->appendMemcpy(globalObject, globalObject->vm(), target->length(), *kind, elements));
}

template<typename View>
static bool tryListElementsOfView(JSArrayBufferView* view, Vector<EncodedJSValue, 64>& values)
{
    size_t length = view->length();
    if (!values.tryReserveInitialCapacity(length))
        return false;
    values.grow(length);
    const auto* __restrict elements = uncheckedDowncast<View>(view)->typedVector();
    EncodedJSValue* __restrict numbers = values.mutableSpan().data();
    for (size_t index = 0; index < length; ++index)
        numbers[index] = JSValue::encode(View::Adaptor::toJSValue(nullptr, elements[index]));
    return true;
}

static bool tryListNumbersOfView(JSArrayBufferView* view, Vector<EncodedJSValue, 64>& values)
{
    switch (view->type()) {
    case Int8ArrayType:
        return tryListElementsOfView<JSInt8Array>(view, values);
    case Uint8ArrayType:
        return tryListElementsOfView<JSUint8Array>(view, values);
    case Uint8ClampedArrayType:
        return tryListElementsOfView<JSUint8ClampedArray>(view, values);
    case Int16ArrayType:
        return tryListElementsOfView<JSInt16Array>(view, values);
    case Uint16ArrayType:
        return tryListElementsOfView<JSUint16Array>(view, values);
    case Int32ArrayType:
        return tryListElementsOfView<JSInt32Array>(view, values);
    case Uint32ArrayType:
        return tryListElementsOfView<JSUint32Array>(view, values);
    case Float16ArrayType:
        return tryListElementsOfView<JSFloat16Array>(view, values);
    case Float32ArrayType:
        return tryListElementsOfView<JSFloat32Array>(view, values);
    case Float64ArrayType:
        return tryListElementsOfView<JSFloat64Array>(view, values);
    default:
        return false;
    }
}

static bool tryAppendNumbersOfView(JSGlobalObject* globalObject, JSArray* target, JSArrayBufferView* view)
{
    if (view->isOutOfBounds() || !view->isIteratorProtocolFastAndNonObservable() || static_cast<uint64_t>(target->length()) + view->length() > MAX_STORAGE_VECTOR_LENGTH)
        return false;
    Vector<EncodedJSValue, 64> values;
    if (!tryListNumbersOfView(view, values))
        return false;
    bool holdsInt32Alone = view->type() >= Int8ArrayType && view->type() <= Int32ArrayType;
    std::optional<IndexingType> kind = holdsInt32Alone ? hasDouble(target->indexingType()) ? ArrayWithDouble : ArrayWithInt32 : findIndexingTypeOfValues(target, values.span());
    RELEASE_ASSERT(kind);
    return values.isEmpty() || target->appendMemcpy(globalObject, globalObject->vm(), target->length(), *kind, values.span());
}

static void appendIterable(Instance& instance, JSGlobalObject* globalObject, JSArray* target, JSValue iterable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (iterable.isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createSpreadOfNullishError(globalObject));
        return;
    }
    if (iterable.isCell()) {
        JSCell* cell = iterable.asCell();
        if (isReadByIndexUnseen(iterable))
            RELEASE_AND_RETURN(scope, appendElementsOfList(globalObject, target, asArray(iterable)));
        if (cell->isString() || isTypedArrayType(cell->type())) {
            if (cell->isString() && globalObject->isStringPrototypeIteratorProtocolFastAndNonObservable()) {
                bool isAppended = tryAppendCharacters(instance, target, asString(cell));
                RETURN_IF_EXCEPTION(scope, void());
                if (isAppended)
                    return;
            }
            if (isTypedArrayType(cell->type())) {
                bool isAppended = tryAppendNumbersOfView(globalObject, target, uncheckedDowncast<JSArrayBufferView>(cell));
                RETURN_IF_EXCEPTION(scope, void());
                if (isAppended)
                    return;
            }
            throwIfTooLongForList(globalObject, cell);
            RETURN_IF_EXCEPTION(scope, void());
            JSCellButterfly* listed = CommonSlowPaths::trySpreadFast(globalObject, cell);
            RETURN_IF_EXCEPTION(scope, void());
            if (listed) {
                JSArray* list = CommonSlowPaths::allocateNewArrayBuffer(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(listed->indexingMode()), listed);
                RELEASE_AND_RETURN(scope, appendElementsOfList(globalObject, target, list));
            }
        } else if (!isJSArray(cell)) {
            if (cell->type() == JSMapIteratorType && uncheckedDowncast<JSMapIterator>(cell)->kind() == IterationKind::Entries && tryAppendPairsLeftIn(globalObject, target, uncheckedDowncast<JSMapIterator>(cell)))
                return;
            if (cell->type() == JSMapIteratorType && tryAppendMembersOfIteratorNobodyStepped(globalObject, target, uncheckedDowncast<JSMapIterator>(cell)))
                return;
            if (cell->type() == JSSetIteratorType && tryAppendMembersOfIteratorNobodyStepped(globalObject, target, uncheckedDowncast<JSSetIterator>(cell)))
                return;
            if (cell->type() == JSMapType && uncheckedDowncast<JSMap>(cell)->isIteratorProtocolFastAndNonObservable() && tryAppendPairsOfMap(globalObject, target, uncheckedDowncast<JSMap>(cell)))
                return;
            if (cell->type() == JSSetType && uncheckedDowncast<JSSet>(cell)->isIteratorProtocolFastAndNonObservable())
                RELEASE_AND_RETURN(scope, appendMembersOfSet(globalObject, target, uncheckedDowncast<JSSet>(cell)));
            if (cell->type() == ClonedArgumentsType) {
                bool isAppended = tryAppendElementsOfArguments(globalObject, target, uncheckedDowncast<ClonedArguments>(cell));
                RETURN_IF_EXCEPTION(scope, void());
                if (isAppended)
                    return;
            }
            JSArray* list = tryCreateArrayFromIterableFast(globalObject, iterable);
            RETURN_IF_EXCEPTION(scope, void());
            if (list) {
                if (auto* members = dynamicDowncast<JSMapIterator>(cell))
                    members->close(vm);
                else if (auto* members = dynamicDowncast<JSSetIterator>(cell))
                    members->close(vm);
                RELEASE_AND_RETURN(scope, appendElementsOfList(globalObject, target, list));
            }
        }
    }

    IterationInRunTime iteration(instance, globalObject);
    if (!mayBeSteppedByRunTime(iterable)) {
        if (JSFunction* appends = functionOfLibrary(globalObject, instance.iteration.functionThatAppendsIterable, Builtins::CompilerHelpersAppendIterable)) {
            MarkedArgumentBuffer arguments;
            arguments.append(target);
            arguments.append(iterable);
            ASSERT(!arguments.hasOverflowed());
            scope.release();
            call(globalObject, appends, JSC::getCallData(appends), jsUndefined(), arguments);
            return;
        }
    }
    bool isOpen = iteration.open(iterable, WordingOfIteration::Spread);
    EXCEPTION_ASSERT(!!scope.exception() == !isOpen);
    if (!isOpen)
        return;
    MarkedArgumentBuffer values;
    for (bool hasMore = !iteration.tryStepToEndUnseen(values); hasMore;) {
        JSValue value;
        hasMore = iteration.step(value);
        RETURN_IF_EXCEPTION(scope, void());
        if (hasMore)
            values.append(value);
    }
    if (values.hasOverflowed() || static_cast<uint64_t>(target->length()) + values.size() > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return;
    }
    if (values.isEmpty())
        return;
    ArgList listed(values);
    std::span<const EncodedJSValue> elements(listed.data(), listed.size());
    std::optional<IndexingType> kind = findIndexingTypeOfValues(target, elements);
    RELEASE_ASSERT(kind);
    bool isAppended = target->appendMemcpy(globalObject, vm, target->length(), *kind, elements);
    RETURN_IF_EXCEPTION(scope, void());
    if (isAppended)
        return;
    for (size_t index = 0; index < values.size(); ++index) {
        target->push(globalObject, values.at(index));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTAppendIterable, void, (Instance* instance, JSCell* array, EncodedJSValue iterable))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAppendIterable);
    appendIterable(*instance, globalObject, cellArgument<JSArray>(array, Entry::operationAOTAppendIterable), JSValue::decode(iterable));
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
