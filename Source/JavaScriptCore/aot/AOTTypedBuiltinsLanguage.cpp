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
    if (object->structure()->isDictionary()) [[unlikely]]
        flattenOnce(vm, object);
    bool mayShare = value.isObject() && !globalObject->isHavingABadTime();

    if (mayShare && object->structure()->compiledLayout()) {
        JSObject* prototype = object->getPrototypeDirect().getObject();
        bool inheritsNone = !prototype;
        if (prototype && isOrdinaryWithOrdinaryPrototypes(prototype)) {
            auto* inherited = propertyNameEnumerator(globalObject, prototype);
            OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
            inheritsNone = !inherited->indexedLength() && !inherited->sizeOfPropertyNames();
        }
        auto* ofProgram = uncheckedDowncast<Typed::Object>(object);
        if (JSCellButterfly* keys = inheritsNone ? ofProgram->keys(vm) : nullptr) {
            if (&ofProgram->state() == instance->typed)
                instance->typedKeyLists[Typed::shapeWordOf(ofProgram->shape()) % Instance::numberOfTypedKeyLists] = { ofProgram->orderWord(), Typed::shapeWordOf(ofProgram->shape()), keys };
            OPERATION_RETURN(scope, JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(keys->indexingMode()), keys->toButterfly()));
        }
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

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIncrement, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue numeric = JSValue::decode(encodedValue).toNumeric(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (numeric.isBigInt())
        OPERATION_RETURN(scope, JSValue::encode(JSBigInt::inc(globalObject, numeric.asHeapBigInt())));
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(numeric.asNumber() + 1)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDecrement, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue numeric = JSValue::decode(encodedValue).toNumeric(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (numeric.isBigInt())
        OPERATION_RETURN(scope, JSValue::encode(JSBigInt::dec(globalObject, numeric.asHeapBigInt())));
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(numeric.asNumber() - 1)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetSuperConstructor, EncodedJSValue, (Instance* instance, JSCell* classObject))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(asObject(classObject)->getPrototype(globalObject)));
}

namespace Typed {

using namespace B3;

// The family aot/builtins/language.def.

std::optional<Writes> writesOfInlineLanguage(InlineBuiltin which, std::span<const Type>)
{
    switch (which) {
    case InlineBuiltin::This:
        return Writes::Pure;
    case InlineBuiltin::GetSuperConstructor:
        return Writes::Nothing;
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
    case InlineBuiltin::Increment:
    case InlineBuiltin::Decrement:
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
        if (m_isCarriedOutByBranch.get(result.index()))
            return yieldBoolean(boolean(false), type);
        if (value.engineClass() == Format::none)
            return yieldBoolean(boolean(false), type);
        auto& engineClass = m_builtins.classes()[value.engineClass()];
        auto holdsOf = [&](CellPointer cell) {
            Word32 isOriginal = hasOriginalStructure(cell, engineClass);
            if (!value.takesArrayAsMade())
                return isOriginal;
            return either<Word32>(isOriginal, [&] {
                return boolean(true);
            }, [&] {
                return isArrayAsMade(cell);
            });
        };
        Operand operand = get(arguments[0]);
        if (operand.rep == Rep::Cell)
            return yieldBoolean(holdsOf(operand.as<CellPointer>()), type);
        if (operand.rep != Rep::JSValue)
            return yieldBoolean(boolean(false), type);
        Boxed boxed = operand.as<Boxed>();
        return yieldBoolean(either<Word32>(isCell(boxed), [&] {
            return holdsOf(asCell(boxed));
        }, [&] {
            return boolean(false);
        }), type);
    }
    case InlineBuiltin::GetSuperConstructor: {
        CellPointer classObject = asCell(in<Boxed>(arguments[0]));
        LBasicBlock keepsItItself = newColdBlock();
        Join join(m_out, Rep::JSValue, Int64);
        Boxed parent(m_out.load64(structureOf(classObject.value()), m_heaps.Structure_prototype));
        orElse(isNotEmpty(parent), keepsItItself);
        join.add(parent);
        m_out.appendTo(keepsItItself);
        join.add(AOT_TYPED_OPERATION(GetSuperConstructor, classObject));
        return ensure(join.finish().as<Boxed>(), m_program.at(m_program.types.anything), type);
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
    case InlineBuiltin::Increment:
    case InlineBuiltin::Decrement:
        return lowerStepByOne(which, result, value);
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
    Word32 finalStructureID = word32(0);
    Integer inlineCapacity = integer(0);
    if (isAskedForFinalObject(result)) {
        Word32 isFinal = either<Word32>(isShared, [&] {
            return Word32(isCellOfType(asCell(object).value(), FinalObjectType));
        }, [&] {
            return word32(0);
        });
        finalStructureID = either<Word32>(isFinal, [&] {
            return loadField<Word32>(asCell(object), m_heaps.JSCell_structureID);
        }, [&] {
            return word32(0);
        });
        inlineCapacity = either<Integer>(isFinal, [&] {
            return Integer(m_out.zeroExtPtr(m_out.load8ZeroExt32(structureOf(asCell(object).value()), m_heaps.Structure_inlineCapacity)));
        }, [&] {
            return integer(0);
        });
    }
    m_forInStamps.append({ result, structureID, orderWord, finalStructureID, inlineCapacity });
    return ensure(keys, Type::engineClass(m_builtins.arrayClass()), typeOf(result));
}

bool Lowering::isForInKeys(const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ForInKeys;
}

bool Lowering::isTestOfForInStamp(const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin || value.arguments.size() != 3 || (m_function.at(value.arguments[1]).opcode != Opcode::Undefined && m_function.at(value.arguments[1]).opcode != Opcode::Boolean))
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::HasEnumerableProperty;
}

bool Lowering::isAskedForFinalObject(ValueID keys) const
{
    for (auto& value : m_function.values) {
        if (isTestOfForInStamp(value) && m_function.at(value.arguments[1]).opcode == Opcode::Boolean && originOf(value.arguments[2]) == keys)
            return true;
    }
    return false;
}

void Lowering::branchOnForInStamp(const Value& value, LBasicBlock isAsListed, LBasicBlock isNot)
{
    for (auto& candidate : m_forInStamps) {
        if (candidate.keys != originOf(value.arguments[2]))
            continue;
        if (m_function.at(value.arguments[1]).opcode == Opcode::Boolean) {
            orElse(logicalNot(isZero(candidate.finalStructureID)), isNot);
            orElse(equal(loadField<Word32>(asCell(in<Boxed>(value.arguments[0])), m_heaps.JSCell_structureID), candidate.finalStructureID), isNot);
        } else
            isAsListedOrElse(candidate, in<Boxed>(value.arguments[0]), isNot);
        m_out.jump(isAsListed);
        return;
    }
    m_out.jump(isNot);
}

auto Lowering::forInStampOf(ValueID object, ValueID key) const -> const ForInStamp*
{
    auto& read = m_function.at(m_program.originOf(m_function, key));
    if (read.opcode != Opcode::GetByVal)
        return nullptr;
    ValueID keys = originOf(read.child(0));
    if (!isForInKeys(m_function.at(keys)) || originOf(m_function.at(keys).arguments[0]) != originOf(object))
        return nullptr;
    for (auto& candidate : m_forInStamps) {
        if (candidate.keys == keys)
            return &candidate;
    }
    return nullptr;
}

CellPointer Lowering::isAsListedOrElse(const ForInStamp& stamp, Boxed object, LBasicBlock otherwise)
{
    orElse(logicalNot(isZero(stamp.structureID)), otherwise);
    CellPointer cell = asCell(object);
    orElse(equal(loadField<Word32>(cell, m_heaps.JSCell_structureID), stamp.structureID), otherwise);
    orElse(equal(orderWordOf(cell), stamp.orderWord), otherwise);
    return cell;
}

// With the list the key was read from, which is of the same object (Preparation::linkForInKeys()): the key is there while the object is as it
// was when the list was made.
Operand Lowering::lowerHasEnumerableProperty(ValueID result, const Value& value)
{
    auto& type = typeOf(result);
    if (m_isCarriedOutByBranch.get(result.index()))
        return yieldBoolean(boolean(false), type);
    if (isTestOfForInStamp(value)) {
        LBasicBlock isAsListed = m_out.newBlock();
        LBasicBlock isNot = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        branchOnForInStamp(value, isAsListed, isNot);
        m_out.appendTo(isAsListed);
        join.add(boolean(true));
        m_out.appendTo(isNot);
        join.add(boolean(false));
        return yieldBoolean(join.finish().as<Word32>(), type);
    }
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
    if (!stamp) {
        Word32 answer = generally();
        Operand yielded = yieldBoolean(answer, type);
        m_truthinessPhis.add(result.index(), Truthiness { yielded.value, answer.value() });
        return yielded;
    }
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, Rep::Int32, Int32);
    isAsListedOrElse(*stamp, object, slowPath);
    join.add(boolean(true));
    m_out.appendTo(slowPath);
    join.add(generally());
    Word32 answer = join.finish().as<Word32>();
    // The loop only asks whether it is true: lowerToBoolean() finds that here, and nothing is boxed.
    Operand yielded = yieldBoolean(answer, type);
    m_truthinessPhis.add(result.index(), Truthiness { yielded.value, answer.value() });
    return yielded;
}

// HasProperty(object, index) and Get(object, index). An element of an array of ArrayType is looked at in place: such an array has no
// accessors, and inherits no elements.
bool Lowering::standsInLoopOfItsOwnFunction(const Value& value) const
{
    unsigned loop = m_innermostLoop[m_currentBlockID.index()];
    if (loop == noLoop)
        return false;
    bool isOfSameFrame = false;
    m_loops[loop].blocks.forEachSetBit([&](size_t block) {
        for (ValueID id : m_function.blocks[block].values)
            isOfSameFrame |= m_function.at(id).opcode == Opcode::LoopHint && m_function.at(id).inlineFrame == value.inlineFrame;
    });
    return isOfSameFrame;
}

ValueID Lowering::getIndexBehindBranchOn(const Value& hasIndex)
{
    auto& terminator = m_currentBlock->terminator;
    if (terminator.kind != TerminatorKind::Branch || terminator.value != m_currentBlock->values[m_currentIndexInBlock] || &m_function.at(terminator.value) != &hasIndex)
        return { };
    BlockID taken = terminator.targets[0].block;
    if (numberOfPredecessorsOf(taken) != 1)
        return { };
    for (ValueID id : m_function.blocks[taken.index()].values) {
        auto& value = m_function.at(id);
        if (isInlineBuiltin(id, InlineBuiltin::ArrayGetIndex))
            return value.arguments[0] == hasIndex.arguments[0] && value.arguments[1] == hasIndex.arguments[1] && repOf(typeOf(id)) == Rep::JSValue ? id : ValueID();
        if (value.opcode != Opcode::Undefined)
            return { };
    }
    return { };
}

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
    if (auto found = m_elementsFoundByHasIndex.get(m_currentBlock->values[m_currentIndexInBlock].index()); isGet && found.element && found.object == get(object).value && found.key == get(key).value) {
        Boxed element(found.element);
        if (Options::validateAOTTypedFacts() && found.isAlwaysThere) [[unlikely]] {
            Boxed base = in<Boxed>(object);
            LBasicBlock isCellCase = m_out.newBlock();
            LBasicBlock isPlain = m_out.newBlock();
            LBasicBlock differs = newColdBlock();
            LBasicBlock continuation = m_out.newBlock();
            m_out.branch(isCell(base).value(), unsure(isCellCase), unsure(continuation));
            m_out.appendTo(isCellCase);
            m_out.branch(logicalAnd(isPlainArray(asCell(base)), below(indexingModeOf(asCell(base), IndexingShapeMask), word32(ArrayStorageShape))).value(), unsure(isPlain), unsure(continuation));
            m_out.appendTo(isPlain);
            Boxed general = getByValGenerally(object, key);
            Word32 isSame = logicalOr(equal(element, general), logicalAnd(Word32(Emitter::isNumber(element.value())), Word32(Emitter::isNumber(general.value()))));
            m_out.branch(isSame.value(), usually(continuation), rarely(differs));
            m_out.appendTo(differs);
            AOT_TYPED_OPERATION(FactDoesNotHold, element, word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            return ensure(element, type);
        }
        if (Options::validateAOTTypedFacts()) [[unlikely]] {
            Boxed general = getByValGenerally(object, key);
            LBasicBlock differs = newColdBlock();
            LBasicBlock continuation = m_out.newBlock();
            Word32 isSame = logicalOr(equal(element, general), logicalAnd(Word32(Emitter::isNumber(element.value())), Word32(Emitter::isNumber(general.value()))));
            m_out.branch(logicalOr(logicalNot(isNotEmpty(element)), isSame).value(), usually(continuation), rarely(differs));
            m_out.appendTo(differs);
            AOT_TYPED_OPERATION(FactDoesNotHold, element, word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            return ensure(general, type);
        }
        if (found.isAlwaysThere)
            return ensure(element, type);
        LBasicBlock wasNotLookedAt = newColdBlock();
        Join join(m_out, rep, b3TypeOf(rep));
        orElse(isNotEmpty(element), wasNotLookedAt);
        join.add(ensure(element, type));
        m_out.appendTo(wasNotLookedAt);
        join.add(ensure(getByValGenerally(object, key), type));
        return join.finish();
    }
    ValueID reader = isGet ? ValueID() : getIndexBehindBranchOn(value);
    if constexpr (hasStubsForFunctionsWithoutFrame) {
        static_assert(static_cast<unsigned>(Stub::ElementOrNothing) + 1 == static_cast<unsigned>(Stub::LeafElementOrNothing));
        if (get(key).rep == Rep::Int64 && (reader || (isGet && &m_function.at(m_currentBlock->values[m_currentIndexInBlock]) == &value && standsInLoopOfItsOwnFunction(value)))) {
            LValue element = callAllocationStub(Stub::ElementOrNothing, { { in<Boxed>(object).value(), GPRInfo::argumentGPR1 }, { in<Integer>(key).value(), GPRInfo::argumentGPR2 }, { isGet ? undefined().value() : m_out.int64Zero, GPRInfo::argumentGPR3 } }, effectsOfOperation<operationAOTTypedElementOrNothing>(), true);
            if (isGet)
                return ensure(Boxed(element), type);
            m_elementsFoundByHasIndex.set(reader.index(), ElementFound { get(object).value, get(key).value, element, true });
            return ensure(isNotEmpty(Boxed(element)), booleanType, type);
        }
    }
    Vector<ValueFromBlock, 5> elements;
    auto find = [&](LValue element) {
        if (reader)
            elements.append(m_out.anchor(element));
    };
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
    else {
        if (reader)
            find(ensure(asDouble, m_program.at(m_program.types.number), typeOf(reader)).value);
        join.add(ensure(boolean(true), booleanType, type));
    }

    m_out.appendTo(isNotDouble);
    orElse(notEqual(shape, word32(UndecidedShape)), absent);
    orElse(logicalOr(equal(shape, word32(ContiguousShape)), equal(shape, word32(Int32Shape))), generic);
    Boxed element(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly.value(), index.value())));
    orElse(isNotEmpty(element), absent);
    if (isGet)
        join.add(ensure(element, type));
    else {
        find(element.value());
        join.add(ensure(boolean(true), booleanType, type));
    }

    // What is no array index may be the name of a property.
    m_out.appendTo(absent);
    orElse(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), generic);
    if (isGet)
        join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));
    else {
        find(m_out.int64Zero);
        join.add(ensure(boolean(false), booleanType, type));
    }

    m_out.appendTo(generic);
    if (isGet)
        join.add(ensure(getByValGenerally(object, key), type));
    else {
        Operand has = ensure(AOT_TYPED_OPERATION(HasKey, in<Boxed>(object), in<Boxed>(key)), booleanType, type);
        find(m_out.int64Zero);
        join.add(has);
    }
    Operand joined = join.finish();
    if (reader)
        m_elementsFoundByHasIndex.set(reader.index(), ElementFound { get(object).value, get(key).value, m_out.phi(Int64, elements), false });
    return joined;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
