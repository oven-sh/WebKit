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
    // The enumerator lists the own names from the structure that the object has when it is asked for.
    PropertyWalk::materializeLazyProperties(vm, object);
    // No enumerator is kept for a dictionary.
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

// Nobody can see such a list being iterated, by the engine's own test, and a read of it runs no code: what lies in a hole or behind the end is undefined.
static bool isReadByIndexUnseen(JSValue iterable)
{
    JSArray* list = iterable.isCell() ? dynamicDowncast<JSArray>(iterable.asCell()) : nullptr;
    if (!list || !list->isIteratorProtocolFastAndNonObservable())
        return false;
    switch (list->indexingType() & IndexingShapeMask) {
    case UndecidedShape:
    case Int32Shape:
    case DoubleShape:
    case ContiguousShape:
        return !list->structure()->holesMustForwardToPrototype(list);
    default:
        return false;
    }
}

namespace {

enum class WordingOfIteration : uint8_t { Pattern, Spread };

// An iteration that the run time opens, steps and closes for compiled code. What the program can see of it is what the protocol does.
// An iterator of the engine whose `next` is the engine's own is stepped without a call and without a result object: that object has `done` and `value` as plain properties of its own, and nobody but the consumer ever has it.
class IterationInRunTime {
public:
    IterationInRunTime(Instance& instance, JSGlobalObject* globalObject)
        : m_instance(instance)
        , m_globalObject(globalObject)
        , m_vm(globalObject->vm())
    {
    }

    // `iterable` is neither undefined nor null. False: it has thrown.
    bool open(JSValue iterable, WordingOfIteration);
    // The first thing that open() tries, for whoever has something else to try behind it. True: it is open, in the words of a pattern. False: nothing has happened.
    bool tryOpenUnseen(JSValue iterable);
    // False: the iterator is done, or the step has thrown.
    bool step(JSValue&);
    void close();

private:
    enum class Stepper : uint8_t { Protocol, ArrayIterator, MapIterator, SetIterator, StringIterator };

    bool isOwnFunctionOfEngine(JSValue, BuiltinID);
    Stepper findStepper();

    Instance& m_instance;
    JSGlobalObject* m_globalObject;
    VM& m_vm;
    // A spread takes whatever the method under @@iterator answers, and asks a number for `next` as it asks an object.
    JSValue m_iterator;
    JSValue m_next;
    Stepper m_stepper { Stepper::Protocol };
    WordingOfIteration m_wording { WordingOfIteration::Pattern };
};

// By the engine's own tests nobody can see that the method under @@iterator is looked for and called, and `next` read: each is what the realm was made with.
bool IterationInRunTime::tryOpenUnseen(JSValue iterable)
{
    if (!iterable.isCell())
        return false;
    JSCell* cell = iterable.asCell();
    switch (cell->type()) {
    case ArrayType:
        if (!uncheckedDowncast<JSArray>(cell)->isIteratorProtocolFastAndNonObservable())
            return false;
        m_iterator = JSArrayIterator::create(m_vm, m_globalObject->arrayIteratorStructure(), uncheckedDowncast<JSArray>(cell), IterationKind::Values);
        m_stepper = Stepper::ArrayIterator;
        return true;
    case JSMapType:
        // What its steps hand out are lists, which belong to the realm of the `next` that makes them. A Set, a list and a text hand out what they hold.
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
        // JSArrayIterator::next() steps over a list alone.
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

    // `[...map.keys()]`: the method above every iterator of the engine hands back its receiver.
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

// `return` is looked for as the protocol looks for it, whoever has stepped.
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

// builtinFunction() makes a function object each time that it is asked.
static ALWAYS_INLINE JSFunction* functionOfLibrary(JSGlobalObject* globalObject, JSCell*& kept, BuiltinID helper)
{
    if (!kept) [[unlikely]]
        kept = makeFunctionOfLibrary(globalObject, helper);
    return uncheckedDowncast<JSFunction>(kept);
}

// An iterable or an iterator of the engine: IterationInRunTime may step it without a call.
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

static void destructureIterable(Instance& instance, JSGlobalObject* globalObject, JSValue iterable, std::span<EncodedJSValue> elements)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isReadByIndexUnseen(iterable)) {
        JSArray* list = asArray(iterable);
        for (size_t index = 0; index < elements.size(); ++index) {
            JSValue element = list->tryGetIndexQuickly(static_cast<unsigned>(index));
            elements[index] = JSValue::encode(element ? element : jsUndefined());
        }
        return;
    }
    for (EncodedJSValue& element : elements)
        element = JSValue::encode(jsUndefined());

    IterationInRunTime iteration(instance, globalObject);
    // What the run time opens unseen, a Set, a Map, a text, passes through here as ever.
    if (!iteration.tryOpenUnseen(iterable)) {
        // Whatever else is taken apart is stepped and closed by code of the program or of the host. From here each call would enter it anew: the function of the library is entered once and does the same in compiled code.
        // Nothing has been looked for yet that a program could see. A pattern without a place steps nothing, and stays here.
        if (!elements.empty() && !mayBeSteppedByRunTime(iterable)) {
            if (JSFunction* takes = functionOfLibrary(globalObject, instance.iteration.functionThatTakesFromIterable, Builtins::CompilerHelpersTakeFromIterable)) {
                MarkedArgumentBuffer arguments;
                arguments.append(iterable);
                arguments.append(jsNumber(static_cast<uint32_t>(elements.size())));
                ASSERT(!arguments.hasOverflowed());
                JSValue taken = call(globalObject, takes, JSC::getCallData(takes), jsUndefined(), arguments);
                RETURN_IF_EXCEPTION(scope, void());
                // The list is the library's own, and no code of the program has seen it.
                JSArray* list = asArray(taken);
                // What quotes the pattern is thrown from here, where the pattern stands.
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

// An array without holes and without named properties, on it or on its prototype chain, has an enumerator that only records the number of indexes. Arrays with the same Structure and length share it.
static JSPropertyNameEnumerator* enumeratorOfListWithoutNames(Instance& instance, JSArray* list)
{
    VM& vm = *instance.vm;
    Structure* structure = list->structure();
    uint32_t structureID = structure->id().bits();
    uint32_t place = placeAmongListsWithoutNames(structure);
    if (instance.arrays.structuresOfListsWithoutNames[place] != structureID || instance.arrays.chainsAboveListsWithoutNames[place] != structure->prototypeChain(vm, instance.globalObject, list))
        return nullptr;
    uint32_t length = list->getEnumerableLength();
    if (!length || length != list->getArrayLength())
        return nullptr;
    EnumeratorCacheEntry& entry = instance.objects.enumerators[indexOfCachedEnumerator(structureID, length)];
    if (entry.structureID == structureID && entry.orderWord == length)
        return uncheckedDowncast<JSPropertyNameEnumerator>(entry.enumerator);
    JSPropertyNameEnumerator* enumerator = JSPropertyNameEnumerator::tryCreate(vm, structure, length, 0, PropertyNameArrayBuilder(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude));
    if (enumerator)
        entry = { length, structureID, enumerator };
    return enumerator;
}

// An object's enumerated keys only change with its Structure, unless it is a dictionary, has elements, or implements its own property lookup. An array only does that for `length`, which no loop enumerates.
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
    if (!enumerator->indexedLength() || enumerator->sizeOfPropertyNames() || enumerator->cachedStructureID() != structure->id() || structure->isDictionary())
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

JSC_DEFINE_JIT_OPERATION(operationAOTDestructureIterable, void, (Instance* instance, EncodedJSValue iterable, uint32_t count, EncodedJSValue* results))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDestructureIterable);
    destructureIterable(*instance, globalObject, JSValue::decode(iterable), unsafeMakeSpan(results, count));
    OPERATION_RETURN(scope);
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
    if (isSpread)
        throwException(globalObject, scope, createSpreadIteratorNotFunctionError(globalObject));
    else
        throwTypeError(globalObject, scope, getIteratorErrorMessage(validateIterable(vm, iterable, jsUndefined()), iterable));
    OPERATION_RETURN(scope);
}

// A surrogate pair makes one element. The width of the characters is that of their storage now.
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

// What the engine's spread lists in one piece: a text, or a view whose iteration nothing has replaced.
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

// appendIterable() of the library (src/aot/prelude/iterator-consumers.ts in Bun), which goes by the protocol as a spread does, in compiled code. Null: the image does not hold it.
static JSFunction* makeFunctionOfLibrary(JSGlobalObject* globalObject, BuiltinID helper)
{
    // Not by overloadsOf(): the first question unpacks every overload of the table, a megabyte that stays, and the packed array would otherwise leave the executable with the compiler.
    for (const RuntimeFunctionData& function : BuiltinTable::singleton().runtimeFunctions()) {
        if (std::ranges::find(function.implements, helper) != function.implements.end())
            return builtinFunction(globalObject, function.id.index());
    }
    return nullptr;
}


// `[...items]`, `f(...items)`: what the iteration hands out goes behind the elements of the list that the spread fills, which no code of the program has seen. `iterable` is neither undefined nor null.
// Nothing closes the iterator of a spread.
static void appendIterable(Instance& instance, JSGlobalObject* globalObject, JSArray* target, JSValue iterable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // What the engine's own spread lists in one piece, by its own tests.
    if (iterable.isCell()) {
        JSCell* cell = iterable.asCell();
        if (isReadByIndexUnseen(iterable))
            RELEASE_AND_RETURN(scope, appendElementsOfList(globalObject, target, asArray(iterable)));
        if (cell->isString() || isTypedArrayType(cell->type())) {
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
            if (cell->type() == JSMapType && uncheckedDowncast<JSMap>(cell)->isIteratorProtocolFastAndNonObservable() && tryAppendPairsOfMap(globalObject, target, uncheckedDowncast<JSMap>(cell)))
                return;
            // `arguments`, a Set, the keys or the values of a Map or a Set.
            JSArray* list = tryCreateArrayFromIterableFast(globalObject, iterable);
            RETURN_IF_EXCEPTION(scope, void());
            if (list)
                RELEASE_AND_RETURN(scope, appendElementsOfList(globalObject, target, list));
        }
    }

    IterationInRunTime iteration(instance, globalObject);
    // Whatever else is spread is stepped by code of the program or of the host. From here each step would enter it anew, and `done` and `value` would be read as native code reads them:
    // the function of the library is entered once and does the same in compiled code. Nothing has been looked for yet that a program could see.
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
    // No code of the program has seen the list, so nobody can tell when it gets what the steps hand out.
    MarkedArgumentBuffer values;
    while (true) {
        JSValue value;
        bool hasValue = iteration.step(value);
        RETURN_IF_EXCEPTION(scope, void());
        if (!hasValue)
            break;
        values.append(value);
    }
    if (values.hasOverflowed() || static_cast<uint64_t>(target->length()) + values.size() > MAX_STORAGE_VECTOR_LENGTH) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return;
    }
    if (values.isEmpty())
        return;
    ArgList listed(values);
    bool isAppended = target->appendMemcpy(globalObject, vm, target->length(), ArrayWithContiguous, std::span<const EncodedJSValue>(listed.data(), listed.size()));
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
