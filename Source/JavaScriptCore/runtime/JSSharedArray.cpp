/*
 * Copyright (C) 2026 Jordan Hall. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "JSSharedArray.h"

#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSSharedStruct.h"
#include "PropertyNameArray.h"
#include <limits>

namespace JSC {

const ClassInfo JSSharedArray::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSSharedArray) };

static constexpr unsigned maxSharedArrayLength = std::numeric_limits<uint32_t>::max();

JSSharedArray::JSSharedArray(VM& vm, Structure* structure, Ref<SharedStructBlock>&& block)
    : Base(vm, structure)
    , m_block(WTF::move(block))
{
}

JSSharedArray* JSSharedArray::create(VM& vm, Structure* structure, Ref<SharedStructBlock>&& block)
{
    ASSERT(block->isArray());
    auto* object = new (NotNull, allocateCell<JSSharedArray>(vm)) JSSharedArray(vm, structure, WTF::move(block));
    object->finishCreation(vm);
    return object;
}

Structure* JSSharedArray::createStructure(VM& vm, JSGlobalObject* globalObject)
{
    Structure* structure = Structure::create(vm, globalObject, jsNull(), TypeInfo(ObjectType, StructureFlags), info());
    return Structure::preventExtensionsTransition(vm, structure);
}

JSValue JSSharedArray::loadElement(unsigned index)
{
    return sharedSlotToJS(realm(), m_block->load(index));
}

bool JSSharedArray::storeElement(JSGlobalObject* globalObject, unsigned index, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto converted = sharedSlotFromJS(globalObject, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (!converted)
        return false;
    m_block->store(index, WTF::move(*converted));
    return true;
}

bool JSSharedArray::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<JSSharedArray>(object);

    if (propertyName == vm.propertyNames->length) {
        slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly), jsNumber(thisObject->length()));
        return true;
    }

    auto index = parseIndex(propertyName);
    if (!index || *index >= thisObject->length())
        return false;

    slot.setIsTaintedByOpaqueObject();
    // A VM inquiry must not run code or allocate; the element is reported as present but its value is not read.
    if (slot.internalMethodType() == PropertySlot::InternalMethodType::VMInquiry) {
        slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), jsUndefined());
        return false;
    }

    JSValue value = thisObject->loadElement(*index);
    RETURN_IF_EXCEPTION(scope, false);
    slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), value);
    return true;
}

bool JSSharedArray::getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject* globalObject, unsigned index, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<JSSharedArray>(object);
    if (index >= thisObject->length())
        return false;

    slot.setIsTaintedByOpaqueObject();
    if (slot.internalMethodType() == PropertySlot::InternalMethodType::VMInquiry) {
        slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), jsUndefined());
        return false;
    }

    JSValue value = thisObject->loadElement(index);
    RETURN_IF_EXCEPTION(scope, false);
    slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), value);
    return true;
}

bool JSSharedArray::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<JSSharedArray>(cell);

    if (propertyName == vm.propertyNames->length) {
        if (slot.isStrictMode())
            throwTypeError(globalObject, scope, ReadonlyPropertyWriteError);
        return false;
    }

    auto index = parseIndex(propertyName);
    if (!index || *index >= thisObject->length()) {
        // The object is not extensible and its prototype is null, so a key that is not an element cannot be added.
        if (slot.isStrictMode())
            throwTypeError(globalObject, scope, NonExtensibleObjectPropertyDefineError);
        return false;
    }
    // [[Set]] of a shared struct ignores the receiver and writes the element.
    thisObject->storeElement(globalObject, *index, value);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

bool JSSharedArray::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<JSSharedArray>(cell);
    if (index >= thisObject->length()) {
        if (shouldThrow)
            throwTypeError(globalObject, scope, NonExtensibleObjectPropertyDefineError);
        return false;
    }
    thisObject->storeElement(globalObject, index, value);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

// [[Delete]] of a shared struct returns false for every key, also one that the object does not have.
bool JSSharedArray::deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&)
{
    return false;
}

bool JSSharedArray::deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned)
{
    return false;
}

void JSSharedArray::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    unsigned length = uncheckedDowncast<JSSharedArray>(object)->length();
    for (unsigned i = 0; i < length; ++i)
        propertyNames.add(i);
    if (mode == DontEnumPropertiesMode::Include)
        propertyNames.add(vm.propertyNames->length);
}

bool JSSharedArray::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<JSSharedArray>(object);

    auto fail = [&](ASCIILiteral message) {
        if (shouldThrow)
            throwTypeError(globalObject, scope, message);
        return false;
    };

    if (propertyName == vm.propertyNames->length) {
        if (descriptor.isAccessorDescriptor())
            return fail("Cannot change the length of a SharedArray to an accessor"_s);
        if (descriptor.configurablePresent() && descriptor.configurable())
            return fail("Cannot make the length of a SharedArray configurable"_s);
        if (descriptor.enumerablePresent() && descriptor.enumerable())
            return fail("Cannot make the length of a SharedArray enumerable"_s);
        if (descriptor.writablePresent() && descriptor.writable())
            return fail("Cannot make the length of a SharedArray writable"_s);
        if (descriptor.value() && !sameValue(globalObject, descriptor.value(), jsNumber(thisObject->length())))
            return fail(ReadonlyPropertyWriteError);
        RETURN_IF_EXCEPTION(scope, false);
        return true;
    }

    auto index = parseIndex(propertyName);
    if (!index || *index >= thisObject->length())
        return fail(NonExtensibleObjectPropertyDefineError);
    // The elements of a SharedArray stay writable, enumerable, non-configurable data properties. A descriptor that
    // asks for none of the refused changes (an accessor descriptor has no value and no writable field) changes nothing.
    if (descriptor.configurablePresent() && descriptor.configurable())
        return fail("Cannot make a SharedArray element configurable"_s);
    if (descriptor.enumerablePresent() && !descriptor.enumerable())
        return fail("Cannot make a SharedArray element non-enumerable"_s);
    if (descriptor.writablePresent() && !descriptor.writable())
        return fail("Cannot make a SharedArray element read-only"_s);
    if (descriptor.value()) {
        thisObject->storeElement(globalObject, *index, descriptor.value());
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

// ------------------------------ The constructor ------------------------------

static JSC_DECLARE_HOST_FUNCTION(sharedArrayCall);
static JSC_DECLARE_HOST_FUNCTION(sharedArrayConstruct);

JSC_DEFINE_HOST_FUNCTION(sharedArrayCall, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "SharedArray must be called with new"_s);
}

JSC_DEFINE_HOST_FUNCTION(sharedArrayConstruct, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // SharedArray is final (https://github.com/tc39/proposal-structs): `class extends SharedArray` already fails
    // when the class is defined, because the constructor has no `prototype`. The constructor does not look at
    // new.target, which is never undefined here because the call without new throws.
    // SharedArray(...values): no argument is an empty array, one argument is the length, and two or more are the elements.
    unsigned argumentCount = callFrame->argumentCount();
    double length = argumentCount;
    if (argumentCount == 0)
        length = 0;
    else if (argumentCount == 1) {
        JSValue lengthValue = callFrame->argument(0);
        if (!lengthValue.isNumber() || !std::isfinite(lengthValue.asNumber()) || std::trunc(lengthValue.asNumber()) != lengthValue.asNumber())
            return throwVMTypeError(globalObject, scope, "SharedArray length must be an integral Number"_s);
        length = lengthValue.asNumber();
        if (length < 0 || length > maxSharedArrayLength)
            return throwVMRangeError(globalObject, scope, "Invalid SharedArray length"_s);
    }
    RefPtr<SharedStructBlock> block = SharedStructBlock::createArray(static_cast<unsigned>(length));
    if (!block) {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    JSObject* result = wrapSharedArrayBlock(globalObject, block.releaseNonNull());
    if (argumentCount >= 2) {
        auto* array = dynamicDowncast<JSSharedArray>(result);
        RELEASE_ASSERT(array);
        for (unsigned i = 0; i < argumentCount; ++i) {
            array->storeElement(globalObject, i, callFrame->uncheckedArgument(i));
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    return JSValue::encode(result);
}

JSObject* createSharedArrayConstructor(VM& vm, JSGlobalObject* globalObject)
{
    return JSFunction::create(vm, globalObject, 0, "SharedArray"_s, sharedArrayCall, ImplementationVisibility::Public, NoIntrinsic, sharedArrayConstruct);
}

// ------------------------------ The embedder API ------------------------------

RefPtr<SharedStructBlock> sharedArrayBlockOf(JSValue value)
{
    if (auto* sharedArray = dynamicDowncast<JSSharedArray>(value))
        return RefPtr<SharedStructBlock>(&sharedArray->block());
    return nullptr;
}

JSObject* wrapSharedArrayBlock(JSGlobalObject* globalObject, Ref<SharedStructBlock>&& block)
{
    RELEASE_ASSERT(block->isArray());
    return wrapSharedStructBlock(globalObject, WTF::move(block));
}

// ------------------------------ Atomics ------------------------------

// The steps of Atomics.load, store, exchange and compareExchange on a SharedArray, which is a shared struct: the
// key must be a property key (a TypeError otherwise), a value to store must be shareable (a TypeError), and the array
// must have an own property with that key (a RangeError): "length" or an element ("0", "1", ...). A Number is not a
// property key, so it is a TypeError, as the proposal says.
struct SharedArrayAtomicKey {
    bool isLength;
    std::optional<unsigned> index;
};

static std::optional<SharedArrayAtomicKey> sharedArrayAtomicKey(JSGlobalObject* globalObject, ThrowScope& scope, JSValue key)
{
    VM& vm = globalObject->vm();
    if (key.isSymbol())
        return SharedArrayAtomicKey { false, std::nullopt };
    if (!key.isString()) {
        throwTypeError(globalObject, scope, "The element of an Atomics operation on a SharedArray must be a property key"_s);
        return std::nullopt;
    }
    Identifier name = key.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (name == vm.propertyNames->length)
        return SharedArrayAtomicKey { true, std::nullopt };
    return SharedArrayAtomicKey { false, parseIndex(name) };
}

static bool sharedArrayAtomicCheckValue(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value)
{
    if (canBeShared(globalObject->vm(), value))
        return true;
    throwTypeError(globalObject, scope, "Only primitives, symbols, shared structs, SharedArrays, Atomics.Mutex and Atomics.Condition can be shared"_s);
    return false;
}

static std::optional<unsigned> sharedArrayAtomicElement(JSGlobalObject* globalObject, ThrowScope& scope, JSSharedArray* array, const SharedArrayAtomicKey& key)
{
    if (!key.index || *key.index >= array->length()) {
        throwRangeError(globalObject, scope, "The SharedArray has no such element."_s);
        return std::nullopt;
    }
    return key.index;
}

// The length is an own property that cannot be written.
static EncodedJSValue sharedArrayAtomicWriteLength(JSGlobalObject* globalObject, ThrowScope& scope)
{
    return throwVMTypeError(globalObject, scope, "The length of a SharedArray is read-only"_s);
}

EncodedJSValue sharedArrayAtomicLoad(JSGlobalObject* globalObject, JSSharedArray* array, JSValue keyValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto key = sharedArrayAtomicKey(globalObject, scope, keyValue);
    if (!key)
        return { };
    if (key->isLength)
        return JSValue::encode(jsNumber(array->length()));
    auto index = sharedArrayAtomicElement(globalObject, scope, array, *key);
    if (!index)
        return { };
    RELEASE_AND_RETURN(scope, sharedSlotAtomicLoad(globalObject, array->realm(), array->block(), *index));
}

EncodedJSValue sharedArrayAtomicStore(JSGlobalObject* globalObject, JSSharedArray* array, JSValue keyValue, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto key = sharedArrayAtomicKey(globalObject, scope, keyValue);
    if (!key)
        return { };
    if (!sharedArrayAtomicCheckValue(globalObject, scope, value))
        return { };
    if (key->isLength)
        return sharedArrayAtomicWriteLength(globalObject, scope);
    auto index = sharedArrayAtomicElement(globalObject, scope, array, *key);
    if (!index)
        return { };
    RELEASE_AND_RETURN(scope, sharedSlotAtomicStore(globalObject, array->realm(), array->block(), *index, value));
}

EncodedJSValue sharedArrayAtomicExchange(JSGlobalObject* globalObject, JSSharedArray* array, JSValue keyValue, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto key = sharedArrayAtomicKey(globalObject, scope, keyValue);
    if (!key)
        return { };
    if (!sharedArrayAtomicCheckValue(globalObject, scope, value))
        return { };
    if (key->isLength)
        return sharedArrayAtomicWriteLength(globalObject, scope);
    auto index = sharedArrayAtomicElement(globalObject, scope, array, *key);
    if (!index)
        return { };
    RELEASE_AND_RETURN(scope, sharedSlotAtomicExchange(globalObject, array->realm(), array->block(), *index, value));
}

EncodedJSValue sharedArrayAtomicCompareExchange(JSGlobalObject* globalObject, JSSharedArray* array, JSValue keyValue, JSValue expected, JSValue replacement)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto key = sharedArrayAtomicKey(globalObject, scope, keyValue);
    if (!key)
        return { };
    if (!sharedArrayAtomicCheckValue(globalObject, scope, replacement))
        return { };
    if (key->isLength)
        return sharedArrayAtomicWriteLength(globalObject, scope);
    auto index = sharedArrayAtomicElement(globalObject, scope, array, *key);
    if (!index)
        return { };
    RELEASE_AND_RETURN(scope, sharedSlotAtomicCompareExchange(globalObject, array->realm(), array->block(), *index, expected, replacement));
}

} // namespace JSC
