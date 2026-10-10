/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedObject.h"
#include "JSCInlines.h"
#include "JSPropertyNameEnumeratorInlines.h"
#include "ObjectConstructor.h"

namespace JSC { namespace AOT {

// To list what it and those it inherits from have takes no code of the program.
static bool isOrdinaryWithOrdinaryPrototypes(JSObject* object)
{
    for (; object; object = object->getPrototypeDirect().getObject()) {
        TypeInfo info = object->structure()->typeInfo();
        if (info.overridesAnyFormOfGetOwnPropertyNames() || info.overridesGetPrototype() || object->type() == ProxyObjectType)
            return false;
    }
    return true;
}

// The keys `for..in` visits, each of which it asks about again when its turn comes (HasEnumerableProperty).
//
// A list that is shared until it is written to says more: the keys are the own enumerable properties of an object, all of them named by its
// Structure or, of an object of the program, by its Structure and its key order. So each is there for as long as those stay. It is the
// list Object.keys yields, which the engine keeps for the Structure and the run time for the key order.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedForInKeys, JSCell*, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isUndefinedOrNull())
        OPERATION_RETURN(scope, constructEmptyArray(globalObject, nullptr));
    JSObject* object = value.toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    bool mayShare = value.isObject() && !globalObject->isHavingABadTime();

    if (mayShare && object->structure()->compiledLayout()) {
        JSObject* prototype = object->getPrototypeDirect().getObject();
        bool inheritsNone = !prototype;
        if (prototype && isOrdinaryWithOrdinaryPrototypes(prototype)) {
            auto* inherited = propertyNameEnumerator(globalObject, prototype);
            OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
            inheritsNone = !inherited->indexedLength() && !inherited->sizeOfPropertyNames();
        }
        if (JSCellButterfly* keys = inheritsNone ? uncheckedDowncast<Typed::Object>(object)->keys(vm) : nullptr)
            OPERATION_RETURN(scope, JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(keys->indexingMode()), keys->toButterfly()));
    }

    // The engine keeps it for the Structure where it can.
    auto* enumerator = propertyNameEnumerator(globalObject, object);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    uint32_t indexedLength = enumerator->indexedLength();
    uint32_t numberOfNames = enumerator->sizeOfPropertyNames();
    if (mayShare && !indexedLength && numberOfNames && enumerator->endStructurePropertyIndex() == numberOfNames && enumerator->cachedStructureID() == object->structureID())
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::Strings, DontEnumPropertiesMode::Exclude));

    JSArray* keys = JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), indexedLength + numberOfNames);
    if (!keys) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    for (uint32_t i = 0; i < indexedLength; ++i) {
        keys->putDirectIndex(globalObject, i, jsString(vm, Identifier::from(vm, i).string()));
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    for (uint32_t i = 0; i < numberOfNames; ++i) {
        keys->putDirectIndex(globalObject, indexedLength + i, enumerator->propertyNameAtIndex(i));
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    OPERATION_RETURN(scope, keys);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedHasEnumerableProperty, size_t, (Instance* instance, EncodedJSValue encodedValue, EncodedJSValue encodedKey))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* object = JSValue::decode(encodedValue).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    auto key = JSValue::decode(encodedKey).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, object->hasEnumerableProperty(globalObject, key));
}

namespace Typed {

using namespace B3;

// The family aot/builtins/language.def.

std::optional<Writes> writesOfInlineLanguage(InlineBuiltin which, std::span<const Type>)
{
    switch (which) {
    case InlineBuiltin::This:
        return Writes::Pure;
    // The structure it compares with is looked up on first use.
    case InlineBuiltin::HasOriginalStructure:
        return Writes::RuntimeState;
    // Of anything but an array of ArrayType: HasProperty, Get.
    case InlineBuiltin::ArrayHasIndex:
    case InlineBuiltin::ArrayGetIndex:
    case InlineBuiltin::Call:
    // A proxy answers with code of the program.
    case InlineBuiltin::ForInKeys:
    case InlineBuiltin::HasEnumerableProperty:
        return Writes::Heap;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

std::optional<Operand> Lowering::lowerInlineLanguage(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    switch (which) {
    // `valueOf` and the like.
    case InlineBuiltin::This: {
        auto& receiver = typeOf(value.child(0));
        if (!receiver.hasCells())
            return yieldValue(value.child(0), type);
        return ensure(nonNil(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName)), receiver.withoutPrimitives(Primitive::UndefinedOrNull), type);
    }
    case InlineBuiltin::HasOriginalStructure: {
        if (value.engineClass() == Format::none)
            return yieldBoolean(boolean(false), type);
        auto& engineClass = m_builtins.classes()[value.engineClass()];
        Operand operand = get(arguments[0]);
        if (operand.rep == Rep::Cell)
            return yieldBoolean(hasOriginalStructure(operand.as<CellPointer>(), engineClass), type);
        if (operand.rep != Rep::JSValue)
            return yieldBoolean(boolean(false), type);
        Boxed boxed = operand.as<Boxed>();
        return yieldBoolean(either<Word32>(isCell(boxed), [&] {
            return hasOriginalStructure(asCell(boxed), engineClass);
        }, [&] {
            return boolean(false);
        }), type);
    }
    case InlineBuiltin::ArrayHasIndex:
    case InlineBuiltin::ArrayGetIndex:
        return lowerIndexIntrinsic(which, value, type);
    case InlineBuiltin::Call: {
        Arguments passed;
        passed.append(arguments.span().subspan(2));
        return ensure(callGenerally(in<Boxed>(arguments[0]), in<Boxed>(arguments[1]), passed), type);
    }
    case InlineBuiltin::ForInKeys:
        return lowerForInKeys(result, value);
    case InlineBuiltin::HasEnumerableProperty:
        return lowerHasEnumerableProperty(result, value);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

ValueID Lowering::originOf(ValueID id) const
{
    while (m_function.at(id).opcode == Opcode::Cast)
        id = m_function.at(id).child(0);
    return id;
}

// See operationAOTTypedForInKeys(). Where the list is a shared one, what the keys go by is kept for the loop.
Operand Lowering::lowerForInKeys(ValueID result, const Value& value)
{
    Boxed object = in<Boxed>(value.arguments[0]);
    CellPointer keys = listKeysForIn(object, typeOf(value.arguments[0]));
    Word32 isShared = hasBits(indexingModeOf(keys, CopyOnWrite), CopyOnWrite);
    Word32 structureID = either<Word32>(isShared, [&] {
        return loadField<Word32>(asCell(object), m_heaps.JSCell_structureID);
    }, [&] {
        return word32(0);
    });
    Word32 orderWord = either<Word32>(isShared, [&] {
        return orderWordOf(asCell(object));
    }, [&] {
        return word32(0);
    });
    m_forInStamps.append({ result, structureID, orderWord });
    return ensure(keys, Type::engineClass(m_builtins.arrayClass()), typeOf(result));
}

// With the list the key was read from, which is of the same object (Preparation::linkForInKeys()): the key is there while the object is as it
// was when the list was made.
Operand Lowering::lowerHasEnumerableProperty(ValueID result, const Value& value)
{
    auto& type = typeOf(result);
    Boxed object = in<Boxed>(value.arguments[0]);
    auto generally = [&] {
        return AOT_TYPED_OPERATION(HasEnumerableProperty, object, in<Boxed>(value.arguments[1]));
    };
    // Not where the list has come by the frame of a coroutine: what was kept with it has not.
    const ForInStamp* stamp = nullptr;
    for (auto& candidate : m_forInStamps) {
        if (value.arguments.size() == 3 && candidate.keys == originOf(value.arguments[2]))
            stamp = &candidate;
    }
    if (!stamp)
        return yieldBoolean(generally(), type);
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, Rep::Int32, Int32);
    // Then it is a cell.
    orElse(logicalNot(isZero(stamp->structureID)), slowPath);
    CellPointer cell = asCell(object);
    orElse(equal(loadField<Word32>(cell, m_heaps.JSCell_structureID), stamp->structureID), slowPath);
    orElse(equal(orderWordOf(cell), stamp->orderWord), slowPath);
    join.add(boolean(true));
    m_out.appendTo(slowPath);
    join.add(generally());
    Word32 answer = join.finish().as<Word32>();
    // The loop only asks whether it is true: lowerToBoolean() finds that here, and nothing is boxed.
    m_truthinessPhis.add(result.index(), answer.value());
    return yieldBoolean(answer, type);
}

// HasProperty(object, index) and Get(object, index). An element of an array of ArrayType is looked at in place: such an array has no
// accessors, and inherits no elements.
Operand Lowering::lowerIndexIntrinsic(InlineBuiltin which, const Value& value, const Type& type)
{
    ValueID object = value.arguments[0];
    ValueID key = value.arguments[1];
    bool isGet = which == InlineBuiltin::ArrayGetIndex;
    Rep rep = repOf(type);
    auto& booleanType = m_program.at(m_program.types.boolean);
    // A primitive by its type, once a callback is inlined. A test around this keeps control away: it only has to be code.
    if (Rep kept = get(object).rep; kept != Rep::Cell && kept != Rep::JSValue) {
        if (isGet)
            return ensure(getByValGenerally(object, key), type);
        return ensure(AOT_TYPED_OPERATION(HasKey, in<Boxed>(object), in<Boxed>(key)), booleanType, type);
    }
    LBasicBlock generic = newColdBlock();
    LBasicBlock absent = m_out.newBlock();
    Join join(m_out, rep, b3TypeOf(rep));

    Operand operand = get(object);
    if (operand.rep == Rep::JSValue)
        orElse(isCell(operand.as<Boxed>()), generic);
    CellPointer array = operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(operand.as<Boxed>());
    orElse(isPlainArray(array), generic);
    Integer index = indexOrElse(key, generic);
    Pointer butterfly = butterflyOf(array);
    orElse(below(index, publicLength(butterfly)), absent);
    Word32 shape = indexingModeOf(array, IndexingShapeMask);
    LBasicBlock isDouble = m_out.newBlock();
    LBasicBlock isNotDouble = m_out.newBlock();
    m_out.branch(equal(shape, word32(DoubleShape)).value(), unsure(isDouble), unsure(isNotDouble));

    m_out.appendTo(isDouble);
    Number asDouble(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, butterfly.value(), index.value())));
    orElse(equal(asDouble, asDouble), absent);
    if (isGet)
        join.add(ensure(asDouble, m_program.at(m_program.types.number), type));
    else
        join.add(ensure(boolean(true), booleanType, type));

    m_out.appendTo(isNotDouble);
    orElse(notEqual(shape, word32(UndecidedShape)), absent);
    orElse(logicalOr(equal(shape, word32(ContiguousShape)), equal(shape, word32(Int32Shape))), generic);
    Boxed element(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
    orElse(isNotEmpty(element), absent);
    if (isGet)
        join.add(ensure(element, type));
    else
        join.add(ensure(boolean(true), booleanType, type));

    // What is no array index may be the name of a property.
    m_out.appendTo(absent);
    orElse(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), generic);
    if (isGet)
        join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));
    else
        join.add(ensure(boolean(false), booleanType, type));

    m_out.appendTo(generic);
    if (isGet)
        join.add(ensure(getByValGenerally(object, key), type));
    else
        join.add(ensure(AOT_TYPED_OPERATION(HasKey, in<Boxed>(object), in<Boxed>(key)), booleanType, type));
    return join.finish();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
