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
#include "JSSharedStruct.h"

#include "AtomicsSync.h"
#include "BuiltinNames.h"
#include "CustomGetterSetter.h"
#include "JSArrayBuffer.h"
#include "JSBigInt.h"
#include "JSSharedArray.h"
#include "JSCInlines.h"
#include "Symbol.h"
#include "WeakInlines.h"
#include <wtf/HashMap.h>
#include <wtf/text/SymbolRegistry.h>

namespace JSC {

const ClassInfo JSSharedStruct::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSSharedStruct) };

JSSharedStruct::JSSharedStruct(VM& vm, Structure* structure, Ref<SharedStructBlock>&& block)
    : Base(vm, structure)
    , m_block(WTF::move(block))
{
}

static JSC_DECLARE_CUSTOM_GETTER(sharedStructFieldGetter);
static JSC_DECLARE_CUSTOM_SETTER(sharedStructFieldSetter);

JSSharedStruct* JSSharedStruct::create(JSGlobalObject* globalObject, Ref<SharedStructBlock>&& block)
{
    VM& vm = globalObject->vm();
    size_t extraMemory = sizeof(SharedStructBlock) + sizeof(uint64_t) * block->fieldCount();
    auto* object = new (NotNull, allocateCell<JSSharedStruct>(vm)) JSSharedStruct(vm, globalObject->sharedStructStructure(), WTF::move(block));
    object->finishCreation(vm);
    // The block is native memory that the collector cannot see; count it so that wrappers that die free it in time.
    vm.heap.reportExtraMemoryAllocated(object, extraMemory);
    object->defineFieldProperties(globalObject);
    return object;
}

Structure* JSSharedStruct::createStructure(VM& vm, JSGlobalObject* globalObject)
{
    return Structure::create(vm, globalObject, jsNull(), TypeInfo(ObjectType, StructureFlags), info());
}

// The fields are properties whose value is one CustomGetterSetter cell, shared by all of them: the native getter
// and setter find the field from the property name. Fields are added in declaration order, so the structure of
// two structs with the same fields is the same transition chain and the inline caches stay monomorphic, and the
// offset of field i is firstOutOfLineOffset + i (fieldIndex() relies on it and checks it).
void JSSharedStruct::defineFieldProperties(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    const Vector<Identifier>& names = globalObject->sharedStructWrapperCache().fieldNames(globalObject, m_block->shape());
    CustomGetterSetter* accessor = CustomGetterSetter::create(vm, sharedStructFieldGetter, sharedStructFieldSetter);
    for (const Identifier& name : names)
        putDirectCustomAccessor(vm, name, accessor, static_cast<unsigned>(PropertyAttribute::DontDelete | PropertyAttribute::CustomValue));
    // Cannot fail for an ordinary object that has no indexed properties.
    bool prevented = JSObject::preventExtensions(this, globalObject);
    ASSERT_UNUSED(prevented, prevented);
}

// ------------------------------ Wrapper cache ------------------------------

struct SharedStructWrapperCache::Data {
    UncheckedKeyHashMap<const void*, Weak<JSObject>> map;
    unsigned pruneThreshold { 64 };
    UncheckedKeyHashMap<const SharedStructShape*, Vector<Identifier>> fieldNames;
};

SharedStructWrapperCache::SharedStructWrapperCache()
    : m_data(std::make_unique<Data>())
{
}

SharedStructWrapperCache::~SharedStructWrapperCache() = default;

JSObject* SharedStructWrapperCache::find(const void* key) const
{
    auto iterator = m_data->map.find(key);
    if (iterator == m_data->map.end())
        return nullptr;
    return iterator->value.get();
}

void SharedStructWrapperCache::add(VM&, const void* key, JSObject* object)
{
    // A wrapper that was collected leaves its entry behind, and the key may be reused by a later block, so a
    // lookup trusts an entry only while its wrapper is alive. Dead entries are dropped when the table doubles.
    if (m_data->map.size() >= m_data->pruneThreshold) {
        m_data->map.removeIf([](auto& entry) {
            return !entry.value.get();
        });
        m_data->pruneThreshold = std::max<unsigned>(64, m_data->map.size() * 2);
    }
    m_data->map.set(key, Weak<JSObject>(object));
}

static JSValue sharedSymbolToJS(JSGlobalObject* realm, SharedSymbol& shared);

// String(span) of an empty span is the null string, which is not a name or a description.
static String stringOfCharacters(std::span<const char16_t> characters)
{
    return characters.empty() ? emptyString() : String(characters);
}

const Vector<Identifier>& SharedStructWrapperCache::fieldNames(JSGlobalObject* globalObject, const SharedStructShape& shape)
{
    auto iterator = m_data->fieldNames.find(&shape);
    if (iterator != m_data->fieldNames.end())
        return iterator->value;
    VM& vm = globalObject->vm();
    Vector<Identifier> names;
    for (unsigned k = 0; k < shape.namedFieldCount(); ++k) {
        unsigned slot = shape.namedSlot(k);
        if (shape.isSymbolField(slot)) {
            JSValue symbol = sharedSymbolToJS(globalObject, *shape.fieldSymbol(slot));
            names.append(Identifier::fromUid(asSymbol(symbol)->privateName()->uid()));
        } else
            names.append(Identifier::fromString(vm, stringOfCharacters(shape.fieldName(slot))));
    }
    return m_data->fieldNames.add(&shape, WTF::move(names)).iterator->value;
}

// ------------------------------ Values ------------------------------

// The one wrapper of a mutex or condition state in `realm`. createAtomicsMutex/Condition look the state up in the
// wrapper cache before they make an object, and register what they make there.
static JSObject* sharedStructWrapSync(JSGlobalObject* realm, SharedSyncHandle& handle)
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSObject* existing = realm->sharedStructWrapperCache().find(handle.identity()))
        return existing;
    RefPtr<ArrayBuffer> buffer = handle.createBuffer();
    JSArrayBuffer* state = JSArrayBuffer::create(vm, realm->arrayBufferStructure(ArrayBufferSharingMode::Shared), WTF::move(buffer));
    JSObject* object = handle.kind() == SharedSyncHandle::Kind::Mutex ? createAtomicsMutex(realm, state) : createAtomicsCondition(realm, state);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!object) {
        throwTypeError(realm, scope, "Atomics.Mutex and Atomics.Condition are not available"_s);
        return nullptr;
    }
    return object;
}

// ------------------------------ Symbols ------------------------------

// Registered symbols (Symbol.for) and well-known symbols (Symbol.iterator) are the same symbol in every VM by key
// and by name. A unique symbol gets a SharedSymbol when it is first stored. SharedSymbol keeps, for the process, the
// pairs of a symbol (the address of its SymbolImpl, which is kept alive by a reference that is never released) and
// its SharedSymbol, and the symbol that a SharedSymbol has in each VM, so a unique symbol is one symbol in all the
// global objects of a VM as well as one SharedSymbol for all the VMs.
Ref<SharedSymbol> sharedSymbolOf(JSGlobalObject* realm, Symbol* symbol)
{
    VM& vm = realm->vm();
    SymbolImpl& uid = symbol->privateName()->uid();
    if (uid.symbolRegistry())
        return SharedSymbol::registered(StringView(static_cast<StringImpl&>(uid)));
#define SHARE_WELL_KNOWN_SYMBOL(name) \
    if (&uid == vm.propertyNames->name##Symbol.impl()) \
        return SharedSymbol::wellKnown(#name ""_s);
    JSC_COMMON_PRIVATE_IDENTIFIERS_EACH_WELL_KNOWN_SYMBOL(SHARE_WELL_KNOWN_SYMBOL)
#undef SHARE_WELL_KNOWN_SYMBOL

    if (RefPtr<SharedSymbol> known = SharedSymbol::findUnique(&uid))
        return known.releaseNonNull();
    Ref<SharedSymbol> created = SharedSymbol::createUnique(uid.isNullSymbol() ? std::optional<StringView>() : std::optional<StringView>(StringView(static_cast<StringImpl&>(uid))));
    uid.ref(); // Kept for the life of the process: the address of the SymbolImpl names the symbol.
    SharedSymbol::bind(created.get(), &vm, &uid);
    return created;
}

static JSValue sharedSymbolToJS(JSGlobalObject* realm, SharedSymbol& shared)
{
    VM& vm = realm->vm();
    switch (shared.kind()) {
    case SharedSymbol::Kind::Registered:
        return Symbol::create(vm, vm.symbolRegistry().symbolForKey(stringOfCharacters(shared.description())));
    case SharedSymbol::Kind::WellKnown: {
        String name = stringOfCharacters(shared.description());
#define UNSHARE_WELL_KNOWN_SYMBOL(symbolName) \
        if (name == #symbolName ""_s) \
            return Symbol::create(vm, static_cast<SymbolImpl&>(*vm.propertyNames->symbolName##Symbol.impl()));
        JSC_COMMON_PRIVATE_IDENTIFIERS_EACH_WELL_KNOWN_SYMBOL(UNSHARE_WELL_KNOWN_SYMBOL)
#undef UNSHARE_WELL_KNOWN_SYMBOL
        RELEASE_ASSERT_NOT_REACHED();
        return jsUndefined();
    }
    case SharedSymbol::Kind::Unique:
        break;
    }
    if (const void* known = shared.implIn(&vm))
        return Symbol::create(vm, *const_cast<SymbolImpl*>(static_cast<const SymbolImpl*>(known)));
    Symbol* symbol = shared.hasDescription() ? Symbol::createWithDescription(vm, stringOfCharacters(shared.description())) : Symbol::create(vm);
    SymbolImpl& uid = symbol->privateName()->uid();
    uid.ref(); // Kept for the life of the process, as for a symbol that is stored.
    SharedSymbol::bind(shared, &vm, &uid);
    return symbol;
}

JSValue sharedSlotToJS(JSGlobalObject* globalObject, const SharedSlotValue& value)
{
    // `globalObject` is the realm: wrappers are created in it and cached by it.
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    switch (value.kind()) {
    case SharedSlotValue::Kind::Undefined:
        return jsUndefined();
    case SharedSlotValue::Kind::Null:
        return jsNull();
    case SharedSlotValue::Kind::Boolean:
        return jsBoolean(value.boolValue());
    case SharedSlotValue::Kind::Number:
        return jsNumber(value.numberValue());
    case SharedSlotValue::Kind::String:
        return jsString(vm, String(value.stringValue().characters()));
    case SharedSlotValue::Kind::BigInt: {
        String digits(value.stringValue().characters());
        JSValue result = JSBigInt::stringToBigInt(globalObject, StringView(digits));
        RETURN_IF_EXCEPTION(scope, JSValue());
        return result;
    }
    case SharedSlotValue::Kind::Symbol:
        return sharedSymbolToJS(globalObject, value.symbolValue());
    case SharedSlotValue::Kind::Struct:
        return JSValue(wrapSharedStructBlock(globalObject, Ref<SharedStructBlock>(value.blockValue())));
    case SharedSlotValue::Kind::Sync: {
        JSObject* object = sharedStructWrapSync(globalObject, value.syncValue());
        RETURN_IF_EXCEPTION(scope, JSValue());
        return JSValue(object);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
    return jsUndefined();
}

std::optional<SharedSlotValue> sharedSlotFromJS(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (value.isUndefined())
        return SharedSlotValue::undefined();
    if (value.isNull())
        return SharedSlotValue::null();
    if (value.isBoolean())
        return SharedSlotValue::boolean(value.asBoolean());
    if (value.isNumber())
        return SharedSlotValue::number(value.asNumber());
    if (value.isString()) {
        String string = asString(value)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        return SharedSlotValue::string(SharedString::create(string));
    }
    if (value.isBigInt()) {
        JSString* digits = value.toString(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        String string = digits->value(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        return SharedSlotValue::bigInt(SharedString::create(string));
    }
    if (value.isSymbol())
        return SharedSlotValue::symbol(sharedSymbolOf(globalObject, asSymbol(value)));
    if (SharedStructBlock* block = sharedBlockOfValue(value))
        return SharedSlotValue::structBlock(Ref<SharedStructBlock>(*block));
    JSArrayBuffer* mutexState = atomicsMutexState(vm, value);
    JSArrayBuffer* conditionState = mutexState ? nullptr : atomicsConditionState(vm, value);
    if (JSArrayBuffer* state = mutexState ? mutexState : conditionState) {
        ArrayBufferContents contents;
        if (state->impl() && state->impl()->shareWith(contents))
            return SharedSlotValue::sync(SharedSyncHandle::create(mutexState ? SharedSyncHandle::Kind::Mutex : SharedSyncHandle::Kind::Condition, WTF::move(contents)));
    }
    throwTypeError(globalObject, scope, "Only primitives, symbols, shared structs, SharedArrays, Atomics.Mutex and Atomics.Condition can be shared"_s);
    return std::nullopt;
}

bool canBeShared(VM& vm, JSValue value)
{
    if (!value.isCell())
        return true;
    if (value.isString() || value.isHeapBigInt() || value.isSymbol())
        return true;
    if (!value.isObject())
        return false;
    return sharedBlockOfValue(value) || atomicsMutexState(vm, value) || atomicsConditionState(vm, value);
}

SharedStructBlock* sharedBlockOfValue(JSValue value)
{
    if (auto* sharedStruct = dynamicDowncast<JSSharedStruct>(value))
        return &sharedStruct->block();
    if (auto* sharedArray = dynamicDowncast<JSSharedArray>(value))
        return &sharedArray->block();
    return nullptr;
}

RefPtr<SharedStructBlock> sharedStructBlockOf(JSValue value)
{
    if (auto* sharedStruct = dynamicDowncast<JSSharedStruct>(value))
        return RefPtr<SharedStructBlock>(&sharedStruct->block());
    return nullptr;
}

JSObject* wrapSharedStructBlock(JSGlobalObject* globalObject, Ref<SharedStructBlock>&& block)
{
    VM& vm = globalObject->vm();
    SharedStructWrapperCache& cache = globalObject->sharedStructWrapperCache();
    const void* key = block.ptr();
    // The cache is keyed by address and also holds mutexes and conditions, so a hit counts only if it is the same
    // kind of view as the block asks for.
    if (JSObject* existing = cache.find(key)) {
        if (block->isArray() ? !!dynamicDowncast<JSSharedArray>(existing) : !!dynamicDowncast<JSSharedStruct>(existing))
            return existing;
    }
    JSObject* object;
    if (block->isArray()) {
        size_t extraMemory = sizeof(SharedStructBlock) + sizeof(uint64_t) * block->fieldCount();
        auto* array = JSSharedArray::create(vm, globalObject->sharedArrayStructure(), WTF::move(block));
        vm.heap.reportExtraMemoryAllocated(array, extraMemory);
        object = array;
    }
    else
        object = JSSharedStruct::create(globalObject, WTF::move(block));
    cache.add(vm, key, object);
    return object;
}

// ------------------------------ JSSharedStruct ------------------------------

std::optional<unsigned> JSSharedStruct::fieldIndex(PropertyName propertyName) const
{
    UniquedStringImpl* uid = propertyName.uid();
    if (!uid)
        return std::nullopt;
    SharedStructShape& shape = m_block->shape();
    bool isSymbol = uid->isSymbol();
    if (isSymbol) {
        if (static_cast<SymbolImpl*>(uid)->isPrivate())
            return std::nullopt;
    } else if (auto arrayIndex = parseIndex(propertyName))
        return shape.slotOfArrayIndex(*arrayIndex);

    // The structure knows the field: its offset says which field it is (checked against the name, so that a structure
    // that does not follow the layout falls back to a search instead of reading the wrong slot).
    PropertyOffset offset = structure()->get(vm(), propertyName);
    if (!isValidOffset(offset))
        return std::nullopt;
    unsigned k = static_cast<unsigned>(offset - firstOutOfLineOffset);
    if (!isSymbol) {
        if (k < shape.namedFieldCount()) {
            unsigned slot = shape.namedSlot(k);
            if (!shape.isSymbolField(slot) && StringView(shape.fieldName(slot)) == StringView(uid))
                return slot;
        }
        return shape.indexOf(StringView(uid));
    }
    JSGlobalObject* globalObject = structure()->globalObject();
    const Vector<Identifier>& names = globalObject->sharedStructWrapperCache().fieldNames(globalObject, shape);
    if (k < names.size() && names[k].impl() == uid)
        return shape.namedSlot(k);
    for (unsigned i = 0; i < names.size(); ++i) {
        if (names[i].impl() == uid)
            return shape.namedSlot(i);
    }
    return std::nullopt;
}

JSValue JSSharedStruct::loadField(unsigned index)
{
    return sharedSlotToJS(realm(), m_block->load(index));
}

bool JSSharedStruct::storeField(JSGlobalObject* globalObject, unsigned index, JSValue value)
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

// The native accessors of the fields. For a CustomValue property the engine (and the inline caches) pass the
// object that has the property as the this value, not the receiver of the access.
JSC_DEFINE_CUSTOM_GETTER(sharedStructFieldGetter, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName propertyName))
{
    auto* holder = dynamicDowncast<JSSharedStruct>(JSValue::decode(thisValue));
    if (!holder) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        return throwVMTypeError(globalObject, scope, "Expected a shared struct"_s);
    }
    VM& vm = holder->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto index = holder->fieldIndex(propertyName);
    RELEASE_ASSERT(index);
    JSValue value = holder->loadField(*index);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(value);
}

JSC_DEFINE_CUSTOM_SETTER(sharedStructFieldSetter, (JSGlobalObject* globalObject, EncodedJSValue thisValue, EncodedJSValue encodedValue, PropertyName propertyName))
{
    auto* holder = dynamicDowncast<JSSharedStruct>(JSValue::decode(thisValue));
    if (!holder) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        throwTypeError(globalObject, scope, "Expected a shared struct"_s);
        return false;
    }
    VM& vm = holder->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto index = holder->fieldIndex(propertyName);
    RELEASE_ASSERT(index);
    // A value that cannot be shared throws a TypeError and stores nothing.
    bool stored = holder->storeField(globalObject, *index, JSValue::decode(encodedValue));
    RETURN_IF_EXCEPTION(scope, false);
    return stored;
}

bool JSSharedStruct::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<JSSharedStruct>(object);

    auto fail = [&](ASCIILiteral message) {
        if (shouldThrow)
            throwTypeError(globalObject, scope, message);
        return false;
    };

    auto index = thisObject->fieldIndex(propertyName);
    if (!index)
        return fail(NonExtensibleObjectPropertyDefineError);
    // The fields of a shared struct stay writable, enumerable, non-configurable data properties. A descriptor that
    // asks for none of the refused changes (an accessor descriptor has no value and no writable field) changes nothing.
    if (descriptor.configurablePresent() && descriptor.configurable())
        return fail("Cannot make a shared struct field configurable"_s);
    if (descriptor.enumerablePresent() && !descriptor.enumerable())
        return fail("Cannot make a shared struct field non-enumerable"_s);
    if (descriptor.writablePresent() && !descriptor.writable())
        return fail("Cannot make a shared struct field read-only"_s);
    if (descriptor.value()) {
        thisObject->storeField(globalObject, *index, descriptor.value());
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

bool JSSharedStruct::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    if (auto arrayIndex = parseIndex(propertyName))
        return getOwnPropertySlotByIndex(object, globalObject, *arrayIndex, slot);
    return Base::getOwnPropertySlot(object, globalObject, propertyName, slot);
}

bool JSSharedStruct::getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject* globalObject, unsigned index, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<JSSharedStruct>(object);
    auto field = thisObject->m_block->shape().slotOfArrayIndex(index);
    if (!field)
        return false;
    slot.setIsTaintedByOpaqueObject();
    // A VM inquiry must not run code or allocate; the field is reported as present but its value is not read.
    if (slot.internalMethodType() == PropertySlot::InternalMethodType::VMInquiry) {
        slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), jsUndefined());
        return false;
    }
    JSValue value = thisObject->loadField(*field);
    RETURN_IF_EXCEPTION(scope, false);
    slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), value);
    return true;
}

bool JSSharedStruct::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<JSSharedStruct>(cell);
    auto field = thisObject->m_block->shape().slotOfArrayIndex(index);
    if (!field) {
        if (shouldThrow)
            throwTypeError(globalObject, scope, NonExtensibleObjectPropertyDefineError);
        return false;
    }
    thisObject->storeField(globalObject, *field, value);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

void JSSharedStruct::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    // The fields named by array indices come first, smallest first.
    for (const auto& field : uncheckedDowncast<JSSharedStruct>(object)->m_block->shape().arrayIndexFields())
        propertyNames.add(field.first);
    Base::getOwnPropertyNames(object, globalObject, propertyNames, mode);
}

bool JSSharedStruct::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<JSSharedStruct>(cell);
    auto index = thisObject->fieldIndex(propertyName);
    if (!index) {
        // The object has no such field and cannot get one.
        if (slot.isStrictMode())
            throwTypeError(globalObject, scope, NonExtensibleObjectPropertyDefineError);
        return false;
    }
    // The receiver is ignored: the own field is written, a value that cannot be shared throws a TypeError.
    thisObject->storeField(globalObject, *index, value);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

bool JSSharedStruct::deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&)
{
    return false;
}

bool JSSharedStruct::deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned)
{
    return false;
}

// ------------------------------ Atomics ------------------------------

// The steps of Atomics.load, store, exchange and compareExchange on a shared struct, in the order of the proposal: the
// field must be a property key (a String or a Symbol, which is not converted), a value to store must be shareable
// (a TypeError), and the struct must have the field (a RangeError).
static std::optional<Identifier> sharedStructAtomicKey(JSGlobalObject* globalObject, ThrowScope& scope, JSValue key)
{
    if (!key.isString() && !key.isSymbol()) {
        throwTypeError(globalObject, scope, "The field of an Atomics operation on a shared struct must be a string or a symbol"_s);
        return std::nullopt;
    }
    Identifier name = key.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return name;
}

static bool sharedStructAtomicCheckValue(JSGlobalObject* globalObject, ThrowScope& scope, JSValue value)
{
    if (canBeShared(globalObject->vm(), value))
        return true;
    throwTypeError(globalObject, scope, "Only primitives, symbols, shared structs, SharedArrays, Atomics.Mutex and Atomics.Condition can be shared"_s);
    return false;
}

static std::optional<unsigned> sharedStructAtomicField(JSGlobalObject* globalObject, ThrowScope& scope, JSSharedStruct* object, const Identifier& name)
{
    auto index = object->fieldIndex(name);
    if (!index)
        throwRangeError(globalObject, scope, "The shared struct has no such field"_s);
    return index;
}

EncodedJSValue sharedSlotAtomicLoad(JSGlobalObject* globalObject, JSGlobalObject* realm, SharedStructBlock& block, unsigned index)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = sharedSlotToJS(realm, block.load(index));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

EncodedJSValue sharedSlotAtomicStore(JSGlobalObject* globalObject, JSGlobalObject*, SharedStructBlock& block, unsigned index, JSValue newValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto value = sharedSlotFromJS(globalObject, newValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (!value)
        return { };
    block.store(index, WTF::move(*value));
    return JSValue::encode(newValue);
}

EncodedJSValue sharedSlotAtomicExchange(JSGlobalObject* globalObject, JSGlobalObject* realm, SharedStructBlock& block, unsigned index, JSValue newValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto value = sharedSlotFromJS(globalObject, newValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (!value)
        return { };
    JSValue result = sharedSlotToJS(realm, block.exchange(index, WTF::move(*value)));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

EncodedJSValue sharedSlotAtomicCompareExchange(JSGlobalObject* globalObject, JSGlobalObject* realm, SharedStructBlock& block, unsigned index, JSValue expectedValue, JSValue replacementValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The expected value can be anything: one that cannot be shared never matches, so it is not an error.
    std::optional<SharedSlotValue> expected;
    if (canBeShared(vm, expectedValue)) {
        expected = sharedSlotFromJS(globalObject, expectedValue);
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto replacement = sharedSlotFromJS(globalObject, replacementValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (!replacement)
        return { };
    JSValue result = sharedSlotToJS(realm, block.compareExchange(index, expected ? &*expected : nullptr, WTF::move(*replacement)));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

EncodedJSValue sharedStructAtomicLoad(JSGlobalObject* globalObject, JSSharedStruct* object, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto name = sharedStructAtomicKey(globalObject, scope, key);
    if (!name)
        return { };
    auto index = sharedStructAtomicField(globalObject, scope, object, *name);
    if (!index)
        return { };
    RELEASE_AND_RETURN(scope, sharedSlotAtomicLoad(globalObject, object->realm(), object->block(), *index));
}

EncodedJSValue sharedStructAtomicStore(JSGlobalObject* globalObject, JSSharedStruct* object, JSValue key, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto name = sharedStructAtomicKey(globalObject, scope, key);
    if (!name)
        return { };
    if (!sharedStructAtomicCheckValue(globalObject, scope, value))
        return { };
    auto index = sharedStructAtomicField(globalObject, scope, object, *name);
    if (!index)
        return { };
    RELEASE_AND_RETURN(scope, sharedSlotAtomicStore(globalObject, object->realm(), object->block(), *index, value));
}

EncodedJSValue sharedStructAtomicExchange(JSGlobalObject* globalObject, JSSharedStruct* object, JSValue key, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto name = sharedStructAtomicKey(globalObject, scope, key);
    if (!name)
        return { };
    if (!sharedStructAtomicCheckValue(globalObject, scope, value))
        return { };
    auto index = sharedStructAtomicField(globalObject, scope, object, *name);
    if (!index)
        return { };
    RELEASE_AND_RETURN(scope, sharedSlotAtomicExchange(globalObject, object->realm(), object->block(), *index, value));
}

EncodedJSValue sharedStructAtomicCompareExchange(JSGlobalObject* globalObject, JSSharedStruct* object, JSValue key, JSValue expected, JSValue replacement)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto name = sharedStructAtomicKey(globalObject, scope, key);
    if (!name)
        return { };
    if (!sharedStructAtomicCheckValue(globalObject, scope, replacement))
        return { };
    auto index = sharedStructAtomicField(globalObject, scope, object, *name);
    if (!index)
        return { };
    RELEASE_AND_RETURN(scope, sharedSlotAtomicCompareExchange(globalObject, object->realm(), object->block(), *index, expected, replacement));
}

} // namespace JSC
