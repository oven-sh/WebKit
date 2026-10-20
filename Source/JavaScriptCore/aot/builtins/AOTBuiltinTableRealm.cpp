/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinTableRealm.h"

#if ENABLE(AOT)

#include "AOTBuiltinModules.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTModuleHost.h"
#include "ArrayIteratorPrototype.h"
#include "AsyncFunctionPrototype.h"
#include "AsyncGeneratorFunctionPrototype.h"
#include "AsyncGeneratorPrototype.h"
#include "AsyncIteratorPrototype.h"
#include "BuiltinNames.h"
#include "DeferTermination.h"
#include "GeneratorFunctionPrototype.h"
#include "GeneratorPrototype.h"
#include "GlobalObjectMethodTable.h"
#include "ImmutablePropertyNames.h"
#include "JSCInlines.h"
#include "JSIteratorHelperPrototype.h"
#include "JSIteratorPrototype.h"
#include "MapIteratorPrototype.h"
#include "PropertyDescriptor.h"
#include "SetIteratorPrototype.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <array>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>

namespace JSC::AOT {

enum class PropertyKind : uint8_t { Data, Getter, Setter };

using Lookup = std::expected<JSValue, BuiltinLookupFailure>;
using ObjectLookup = std::expected<JSObject*, BuiltinLookupFailure>;
using CellLookup = std::expected<JSCell*, BuiltinLookupFailure>;

std::optional<BuiltinCellKey> BuiltinCellKey::decode(uint32_t bits, const BuiltinTable& table)
{
    BuiltinID id(bits & ((1u << 24) - 1));
    if (!table.contains(id))
        return std::nullopt;
    for (Kind kind : { Kind::Object, Kind::Getter, Kind::Setter }) {
        if (BuiltinCellKey key(kind, id); key.encoded() == bits)
            return key;
    }
    return std::nullopt;
}

// Only compiled code calls the members of these holders, and no realm has an object for the holders themselves.
static bool groupsFunctionsOfCompiledCode(BuiltinID id)
{
    return id == Builtins::CompilerHelpers || id == Builtins::JSONText || id == Builtins::PrivateGlobals;
}

static bool hasObjectCell(const BuiltinTable& table, BuiltinID id)
{
    if (groupsFunctionsOfCompiledCode(id))
        return false;
    BuiltinKind kind = table.at(id).kind;
    return isObjectKind(kind) || (kind == BuiltinKind::Constant && table.nameOfSymbol(id));
}

static bool mayCallEntryFunction(const BuiltinImplementation& implementation)
{
    switch (implementation.kind) {
    case BuiltinImplementationKind::HostCall:
    case BuiltinImplementationKind::Compiled:
    case BuiltinImplementationKind::CachedGetter:
        return true;
    case BuiltinImplementationKind::Inline:
        return implementation.otherwise.has_value();
    case BuiltinImplementationKind::Generic:
    case BuiltinImplementationKind::Number:
    case BuiltinImplementationKind::Cell:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasAccessorCallee(const BuiltinTable& table, BuiltinID id, BuiltinCallMode mode)
{
    return table.at(id).kind == BuiltinKind::Accessor && std::ranges::any_of(table.overloadsOf(id), [&](const BuiltinOverload& overload) {
        return overload.mode == mode && mayCallEntryFunction(overload.code.implementation);
    });
}

bool BuiltinCellKey::hasCell(const BuiltinTable& table) const
{
    if (table.at(m_builtin).scope == BuiltinScope::Module)
        return false;
    switch (m_kind) {
    case Kind::Object:
        return hasObjectCell(table, m_builtin);
    case Kind::Getter:
        return hasAccessorCallee(table, m_builtin, BuiltinCallMode::Get);
    case Kind::Setter:
        return hasAccessorCallee(table, m_builtin, BuiltinCallMode::Set);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The function that Object.getOwnPropertyDescriptor() returns. A custom accessor has none until a descriptor is created.
static Lookup findAccessorFunction(JSGlobalObject* realm, const Identifier& key, const PropertySlot& slot, PropertyKind kind)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(realm->vm());
    PropertyDescriptor descriptor;
    descriptor.setPropertySlot(realm, key, slot);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return makeUnexpected(BuiltinLookupFailure::LookupThrew);
    }
    switch (kind) {
    case PropertyKind::Getter:
        if (!descriptor.getter().isObject())
            return makeUnexpected(BuiltinLookupFailure::HasNoGetter);
        return descriptor.getter();
    case PropertyKind::Setter:
        if (!descriptor.setter().isObject())
            return makeUnexpected(BuiltinLookupFailure::HasNoSetter);
        return descriptor.setter();
    case PropertyKind::Data:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Lookup findOwnProperty(JSGlobalObject* realm, JSObject* holder, const Identifier& key, PropertyKind kind)
{
    VM& vm = realm->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertySlot slot(holder, PropertySlot::InternalMethodType::GetOwnProperty);
    bool isFound = holder->methodTable()->getOwnPropertySlot(holder, realm, key, slot);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return makeUnexpected(BuiltinLookupFailure::LookupThrew);
    }
    if (!isFound)
        return makeUnexpected(BuiltinLookupFailure::IsMissing);
    bool isAccessor = slot.isAccessor() || (slot.attributes() & PropertyAttribute::CustomAccessor);
    if (isAccessor != (kind != PropertyKind::Data))
        return makeUnexpected(isAccessor ? BuiltinLookupFailure::IsAccessor : BuiltinLookupFailure::IsNotAccessor);
    if (isAccessor)
        return findAccessorFunction(realm, key, slot, kind);
    JSValue value = slot.getValue(realm, key);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return makeUnexpected(BuiltinLookupFailure::LookupThrew);
    }
    return value;
}

static Lookup findPropertyOnPrototypeChain(JSGlobalObject* realm, JSObject* holder, const Identifier& key, PropertyKind kind)
{
    for (JSObject* object = holder; object; object = object->getPrototypeDirect().getObject()) {
        Lookup found = findOwnProperty(realm, object, key, kind);
        if (found || found.error() != BuiltinLookupFailure::IsMissing)
            return found;
    }
    return makeUnexpected(BuiltinLookupFailure::IsMissing);
}

static BuiltinID findSubclassConstructor(const BuiltinTable& table, BuiltinID hiddenRoot)
{
    BuiltinClassID rootClass = table.at(hiddenRoot).instanceClass;
    if (!rootClass)
        return { };
    for (unsigned index = 0; index < table.classCount(); ++index) {
        auto& candidate = table.at(BuiltinClassID(index));
        if (candidate.parent != rootClass || !candidate.constructor)
            continue;
        if (table.at(candidate.constructor).inheritsFrom == hiddenRoot && table.isUnder(candidate.constructor, Builtins::GlobalThis))
            return candidate.constructor;
    }
    return { };
}

struct HiddenPrototype {
    BuiltinClassID builtinClass;
    JSObject* object;
};

static auto hiddenPrototypesOf(JSGlobalObject* realm)
{
    return std::to_array<HiddenPrototype>({
        { BuiltinClasses::Iterator, realm->iteratorPrototype() },
        { BuiltinClasses::IteratorHelper, realm->iteratorHelperPrototype() },
        { BuiltinClasses::AsyncIterator, realm->asyncIteratorPrototype() },
        { BuiltinClasses::ArrayIterator, realm->arrayIteratorPrototype() },
        { BuiltinClasses::MapIterator, realm->mapIteratorPrototype() },
        { BuiltinClasses::SetIterator, realm->setIteratorPrototype() },
        { BuiltinClasses::StringIterator, realm->stringIteratorStructure()->storedPrototypeObject() },
        { BuiltinClasses::RegExpStringIterator, realm->regExpStringIteratorStructure()->storedPrototypeObject() },
        { BuiltinClasses::WrapForValidIterator, realm->wrapForValidIteratorStructure()->storedPrototypeObject() },
        { BuiltinClasses::AsyncFromSyncIterator, realm->asyncFromSyncIteratorStructure()->storedPrototypeObject() },
        { BuiltinClasses::GeneratorFunction, realm->generatorFunctionPrototype() },
        { BuiltinClasses::AsyncFunction, realm->asyncFunctionPrototype() },
        { BuiltinClasses::AsyncGeneratorFunction, realm->asyncGeneratorFunctionPrototype() },
        { BuiltinClasses::Generator, realm->generatorPrototype() },
        { BuiltinClasses::AsyncGenerator, realm->asyncGeneratorPrototype() },
    });
}

void forEachHiddenPrototype(JSGlobalObject* realm, const ScopedLambda<void(JSObject*)>& function)
{
    for (auto& prototype : hiddenPrototypesOf(realm))
        function(prototype.object);
}

void forEachHiddenPrototype(JSGlobalObject* realm, const ScopedLambda<void(BuiltinClassID, JSObject*)>& function)
{
    for (auto& prototype : hiddenPrototypesOf(realm))
        function(prototype.builtinClass, prototype.object);
}

template<typename Function>
static void forEachStaticTableOf(JSGlobalObject* realm, const Function& function)
{
    for (const ClassInfo* info = realm->classInfo(); info; info = info->parentClass) {
        if (const HashTable* hashTable = info->staticPropHashTable)
            function(*hashTable);
    }
}

static LazyClassStructure* lazyClassStructureOf(JSGlobalObject* realm, const HashTableValue& entry)
{
    return std::bit_cast<LazyClassStructure*>(std::bit_cast<char*>(realm) + entry.lazyClassStructureOffset());
}

BuiltinID findGlobalCreatedWith(JSGlobalObject* realm, const LazyClassStructure& lazyStructure)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinID found;
    forEachStaticTableOf(realm, [&](const HashTable& hashTable) {
        for (auto& entry : hashTable) {
            if (found || !(entry.attributes() & PropertyAttribute::ClassStructure) || lazyClassStructureOf(realm, entry) != &lazyStructure)
                continue;
            if (BuiltinName name = table.nameOf(byteCast<uint8_t>(entry.m_key.span())))
                found = table.findGlobal(name);
        }
    });
    return found;
}

void forEachCreatedClassOfGlobal(JSGlobalObject* realm, const ScopedLambda<void(BuiltinID, JSObject* constructor)>& function)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    forEachStaticTableOf(realm, [&](const HashTable& hashTable) {
        for (auto& entry : hashTable) {
            if (!(entry.attributes() & PropertyAttribute::ClassStructure) || !lazyClassStructureOf(realm, entry)->getConcurrently())
                continue;
            JSObject* constructor = lazyClassStructureOf(realm, entry)->constructorConcurrently();
            BuiltinName name = table.nameOf(byteCast<uint8_t>(entry.m_key.span()));
            BuiltinID member = name ? table.findGlobal(name) : BuiltinID();
            if (constructor && member)
                function(member, constructor);
        }
    });
}

// The program may have put something else under the name before anybody read it. The class answers what the realm creates.
static JSObject* findConstructorOfLazyClass(JSGlobalObject* realm, const Identifier& name)
{
    JSObject* constructor = nullptr;
    bool isListed = false;
    forEachStaticTableOf(realm, [&](const HashTable& hashTable) {
        const HashTableValue* entry = isListed ? nullptr : hashTable.entry(name);
        if (!entry)
            return;
        isListed = true;
        if (entry->attributes() & PropertyAttribute::ClassStructure)
            constructor = lazyClassStructureOf(realm, *entry)->constructor(realm);
    });
    return constructor;
}

void forEachInstanceStructure(JSGlobalObject* realm, const ScopedLambda<void(BuiltinClassID, Structure*)>& function)
{
    auto visitIfCreated = [&](BuiltinClassID builtinClass, Structure* structure) {
        if (structure)
            function(builtinClass, structure);
    };
    for (IndexingType indexingType : { ArrayWithUndecided, ArrayWithInt32, ArrayWithDouble, ArrayWithContiguous, ArrayWithArrayStorage, CopyOnWriteArrayWithInt32, CopyOnWriteArrayWithDouble, CopyOnWriteArrayWithContiguous })
        visitIfCreated(BuiltinClasses::Array, realm->originalArrayStructureForIndexingType(indexingType));
    visitIfCreated(BuiltinClasses::Map, realm->mapStructureConcurrently());
    visitIfCreated(BuiltinClasses::Set, realm->setStructureConcurrently());
    visitIfCreated(BuiltinClasses::WeakMap, realm->weakMapStructureConcurrently());
    visitIfCreated(BuiltinClasses::WeakSet, realm->weakSetStructureConcurrently());
    visitIfCreated(BuiltinClasses::ArrayBuffer, realm->arrayBufferStructureConcurrently(ArrayBufferSharingMode::Default));
    visitIfCreated(BuiltinClasses::SharedArrayBuffer, realm->arrayBufferStructureConcurrently(ArrayBufferSharingMode::Shared));
    visitIfCreated(BuiltinClasses::DataView, realm->typedArrayStructureConcurrently(TypeDataView, false));
    visitIfCreated(BuiltinClasses::RegExp, realm->regExpStructure());
    visitIfCreated(BuiltinClasses::Promise, realm->promiseStructure());
    visitIfCreated(BuiltinClasses::String, realm->stringObjectStructure());
    visitIfCreated(BuiltinClasses::Error, realm->errorStructureConcurrently());
    visitIfCreated(BuiltinClasses::Date, realm->dateStructureConcurrently());
#define AOT_VISIT_TYPED_ARRAY_STRUCTURE(name) visitIfCreated(BuiltinClasses::name##Array, realm->typedArrayStructureConcurrently(Type##name, false));
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(AOT_VISIT_TYPED_ARRAY_STRUCTURE)
#undef AOT_VISIT_TYPED_ARRAY_STRUCTURE
    visitIfCreated(BuiltinClasses::ArrayIterator, realm->arrayIteratorStructure());
    visitIfCreated(BuiltinClasses::MapIterator, realm->mapIteratorStructure());
    visitIfCreated(BuiltinClasses::SetIterator, realm->setIteratorStructure());
    visitIfCreated(BuiltinClasses::StringIterator, realm->stringIteratorStructure());
    visitIfCreated(BuiltinClasses::RegExpStringIterator, realm->regExpStringIteratorStructure());
    visitIfCreated(BuiltinClasses::IteratorHelper, realm->iteratorHelperStructure());
    visitIfCreated(BuiltinClasses::WrapForValidIterator, realm->wrapForValidIteratorStructure());
    visitIfCreated(BuiltinClasses::AsyncFromSyncIterator, realm->asyncFromSyncIteratorStructure());
    for (const BuiltinClassBinding& binding : BuiltinBindings::classesOfHost()) {
        if (binding.findInstanceStructure)
            visitIfCreated(binding.builtinClass, binding.findInstanceStructure(realm));
    }
}

static JSObject* findExistingHiddenPrototype(JSGlobalObject* realm, BuiltinClassID builtinClass)
{
    for (auto& prototype : hiddenPrototypesOf(realm)) {
        if (prototype.builtinClass == builtinClass)
            return prototype.object;
    }
    return nullptr;
}

static JSObject* findHiddenPrototype(JSGlobalObject* realm, BuiltinClassID builtinClass)
{
    if (builtinClass == BuiltinClasses::IntlSegments)
        return realm->segmentsStructure()->storedPrototypeObject();
    if (builtinClass == BuiltinClasses::IntlSegmentIterator)
        return realm->segmentIteratorStructure()->storedPrototypeObject();
    if (JSObject* prototype = findExistingHiddenPrototype(realm, builtinClass))
        return prototype;
    return BuiltinBindings::findPrototype(realm, builtinClass);
}

static BuiltinID findClassPrototype(const BuiltinTable& table, BuiltinID hiddenRoot)
{
    BuiltinClassID rootClass = table.at(hiddenRoot).instanceClass;
    if (!rootClass)
        return { };
    auto& builtinClass = table.at(rootClass);
    bool isConstructorOrPrototype = hiddenRoot == builtinClass.constructor || hiddenRoot == builtinClass.prototype;
    return isConstructorOrPrototype ? builtinClass.prototype : BuiltinID();
}

static ObjectLookup findHiddenRootOfClass(JSGlobalObject* realm, BuiltinID id)
{
    auto& table = BuiltinTable::singleton();
    BuiltinID prototypeID = findClassPrototype(table, id);
    JSObject* prototype = prototypeID ? findHiddenPrototype(realm, table.at(id).instanceClass) : nullptr;
    if (!prototype)
        return makeUnexpected(BuiltinLookupFailure::HolderIsMissing);
    if (id == prototypeID)
        return prototype;

    auto constructor = findOwnProperty(realm, prototype, realm->vm().propertyNames->constructor, PropertyKind::Data);
    if (!constructor)
        return makeUnexpected(constructor.error());
    if (!constructor->isObject())
        return makeUnexpected(BuiltinLookupFailure::IsNotObject);
    return asObject(*constructor);
}

static ObjectLookup findHolderObject(JSGlobalObject*, BuiltinID);

// No name leads from the global object to the cache of modules: each module reads it from a require function of its own.
static ObjectLookup findModuleCache(JSGlobalObject* realm)
{
    const ModuleHost* host = realm->globalObjectMethodTable()->moduleHost;
    JSObject* cache = host && host->requireCache ? host->requireCache(realm) : nullptr;
    if (!cache)
        return makeUnexpected(BuiltinLookupFailure::HolderIsMissing);
    return cache;
}

static ObjectLookup findHiddenRoot(JSGlobalObject* realm, BuiltinID id)
{
    if (id == Builtins::ModuleCache)
        return findModuleCache(realm);
    if (std::optional<LinkTimeConstant> constant = BuiltinBindings::linkTimeConstantOf(id))
        return asObject(realm->linkTimeConstant(*constant));
    BuiltinID subclassConstructor = findSubclassConstructor(BuiltinTable::singleton(), id);
    if (!subclassConstructor)
        return findHiddenRootOfClass(realm, id);
    auto constructor = findHolderObject(realm, subclassConstructor);
    if (!constructor)
        return constructor;
    JSValue prototype = (*constructor)->getPrototypeDirect();
    if (!prototype.isObject())
        return makeUnexpected(BuiltinLookupFailure::IsNotObject);
    return asObject(prototype);
}

// The global object has it under the private name that goes with the name.
static Lookup findPrivateGlobal(JSGlobalObject* realm, const Identifier& name, PropertyKind kind)
{
    PrivateSymbolImpl* privateName = realm->vm().propertyNames->builtinNames().lookUpPrivateName(name);
    if (!privateName)
        return makeUnexpected(BuiltinLookupFailure::IsMissing);
    return findOwnProperty(realm, realm, Identifier::fromUid(*privateName), kind);
}

static Lookup findDeclaredProperty(JSGlobalObject* realm, BuiltinID id, PropertyKind kind)
{
    auto& table = BuiltinTable::singleton();
    auto& builtin = table.at(id);
    if (!builtin.holder)
        return makeUnexpected(BuiltinLookupFailure::HolderIsMissing);
    if (builtin.holder == Builtins::PrivateGlobals)
        return findPrivateGlobal(realm, table.identifierOf(realm->vm(), builtin.name), kind);
    if (builtin.holder == Builtins::GlobalThis && kind == PropertyKind::Data) {
        if (JSObject* constructor = findConstructorOfLazyClass(realm, table.identifierOf(realm->vm(), builtin.name)))
            return JSValue(constructor);
    }
    auto holder = findHolderObject(realm, builtin.holder);
    if (!holder)
        return makeUnexpected(BuiltinLookupFailure::HolderIsMissing);
    return findOwnProperty(realm, *holder, table.identifierOf(realm->vm(), builtin.name), kind);
}

static JSFunction* createLibraryFunction(JSGlobalObject* realm, BuiltinID id)
{
    for (const BuiltinOverload& overload : BuiltinTable::singleton().overloadsOf(id)) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        RuntimeFunctionID function = overload.libraryFunction() ? overload.libraryFunction() : overload.slowPathFunction();
        if (function)
            return builtinFunction(realm, function.index());
    }
    return nullptr;
}

// The constructor of a host class need not have a property "prototype". The binding of the class leads to the object all the same.
static JSObject* findPrototypeWithoutProperty(JSGlobalObject* realm, BuiltinID id)
{
    auto& table = BuiltinTable::singleton();
    return findClassPrototype(table, id) == id ? findHiddenPrototype(realm, table.at(id).instanceClass) : nullptr;
}

// A function that only compiled code calls is a member of a holder that no object stands for.
static Lookup findOrCreateMember(JSGlobalObject* realm, BuiltinID id)
{
    Lookup found = findDeclaredProperty(realm, id, PropertyKind::Data);
    if (!found && found.error() == BuiltinLookupFailure::IsMissing) {
        if (JSObject* prototype = findPrototypeWithoutProperty(realm, id))
            return JSValue(prototype);
    }
    if (found || found.error() != BuiltinLookupFailure::HolderIsMissing)
        return found;
    if (auto* binding = BuiltinBindings::nativeFunctionOf(id))
        return JSValue(JSFunction::create(realm->vm(), realm, binding->length, String(), binding->function, ImplementationVisibility::Private));
    if (JSFunction* function = createLibraryFunction(realm, id))
        return JSValue(function);
    return found;
}

static Lookup findMemberValue(JSGlobalObject* realm, BuiltinID id)
{
    if (id == Builtins::GlobalThis)
        return JSValue(realm);
    if (BuiltinTable::singleton().at(id).holder)
        return findOrCreateMember(realm, id);
    auto root = findHiddenRoot(realm, id);
    if (!root)
        return makeUnexpected(root.error());
    return JSValue(*root);
}

static ObjectLookup findHolderObject(JSGlobalObject* realm, BuiltinID id)
{
    auto value = findMemberValue(realm, id);
    if (!value)
        return makeUnexpected(value.error());
    if (!value->isObject())
        return makeUnexpected(BuiltinLookupFailure::IsNotObject);
    return asObject(*value);
}

static BuiltinCallee calleeKindOf(JSValue value)
{
    if (!value.isCell())
        return BuiltinCallee::None;
    JSCell* cell = value.asCell();
    if (auto* function = dynamicDowncast<JSFunction>(cell))
        return function->isHostFunction() ? BuiltinCallee::HostFunction : BuiltinCallee::Function;
    if (cell->type() == InternalFunctionType)
        return BuiltinCallee::InternalFunction;
    return cell->isCallable() ? BuiltinCallee::Function : BuiltinCallee::None;
}

static bool hasExpectedCallee(BuiltinCallee expected, JSValue value)
{
    switch (expected) {
    case BuiltinCallee::None:
        return true;
    case BuiltinCallee::Function:
        return calleeKindOf(value) != BuiltinCallee::None;
    case BuiltinCallee::HostFunction:
    case BuiltinCallee::InternalFunction:
        return calleeKindOf(value) == expected;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isOverloadOfKey(BuiltinCellKey::Kind kind, BuiltinCallMode mode)
{
    switch (kind) {
    case BuiltinCellKey::Kind::Object:
        return mode == BuiltinCallMode::Call || mode == BuiltinCallMode::Construct;
    case BuiltinCellKey::Kind::Getter:
        return mode == BuiltinCallMode::Get;
    case BuiltinCellKey::Kind::Setter:
        return mode == BuiltinCallMode::Set;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static CellLookup checkFound(const BuiltinTable& table, BuiltinCellKey key, JSValue value)
{
    BuiltinID id = key.builtin();
    if (table.at(id).kind == BuiltinKind::Constant) {
        if (!value.isCell())
            return makeUnexpected(BuiltinLookupFailure::IsNotObject);
        if (!value.isSymbol())
            return makeUnexpected(BuiltinLookupFailure::IsNotSymbol);
    } else if (!value.isObject())
        return makeUnexpected(BuiltinLookupFailure::IsNotObject);
    for (auto& overload : table.overloadsOf(id)) {
        if (isOverloadOfKey(key.kind(), overload.mode) && !hasExpectedCallee(overload.code.implementation.callee, value))
            return makeUnexpected(BuiltinLookupFailure::HasOtherCallee);
    }
    return value.asCell();
}

static PropertyKind propertyKindOf(BuiltinCellKey::Kind kind)
{
    switch (kind) {
    case BuiltinCellKey::Kind::Object:
        return PropertyKind::Data;
    case BuiltinCellKey::Kind::Getter:
        return PropertyKind::Getter;
    case BuiltinCellKey::Kind::Setter:
        return PropertyKind::Setter;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::expected<JSCell*, BuiltinLookupFailure> findInRealm(JSGlobalObject* realm, BuiltinCellKey key)
{
    DeferTerminationForAWhile deferTermination(realm->vm());
    auto& table = BuiltinTable::singleton();
    BuiltinID id = key.builtin();
    if (!table.contains(id))
        return makeUnexpected(BuiltinLookupFailure::IsMissing);
    if (table.at(id).scope == BuiltinScope::Module)
        return makeUnexpected(BuiltinLookupFailure::IsPerModule);
    PropertyKind kind = propertyKindOf(key.kind());
    if (kind == PropertyKind::Data && id == Builtins::GlobalThis)
        return static_cast<JSCell*>(realm->globalThis());
    Lookup found = kind == PropertyKind::Data ? findMemberValue(realm, id) : findDeclaredProperty(realm, id, kind);
    if (!found)
        return makeUnexpected(found.error());
    return checkFound(table, key, *found);
}

std::expected<JSCell*, BuiltinLookupFailure> findInHolder(JSGlobalObject* realm, BuiltinCellKey key, JSObject* holder)
{
    DeferTerminationForAWhile deferTermination(realm->vm());
    auto& table = BuiltinTable::singleton();
    BuiltinID id = key.builtin();
    if (!table.contains(id))
        return makeUnexpected(BuiltinLookupFailure::IsMissing);
    // The host keeps what every module object, require function and import.meta has in common on their prototypes.
    auto& find = table.at(id).scope == BuiltinScope::Module ? findPropertyOnPrototypeChain : findOwnProperty;
    Lookup found = find(realm, holder, table.identifierOf(realm->vm(), table.at(id).name), propertyKindOf(key.kind()));
    if (!found)
        return makeUnexpected(found.error());
    return checkFound(table, key, *found);
}

std::expected<JSValue, BuiltinLookupFailure> findValueInHolder(JSGlobalObject* realm, JSObject* holder, const Identifier& key)
{
    DeferTerminationForAWhile deferTermination(realm->vm());
    return findOwnProperty(realm, holder, key, PropertyKind::Data);
}

JSValue getByPath(JSGlobalObject* realm, BuiltinID id)
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& table = BuiltinTable::singleton();
    auto throwNoPath = [&] {
        throwTypeError(realm, scope, "Built-in has no path from the global object"_s);
        return JSValue();
    };
    if (!table.contains(id))
        return throwNoPath();
    if (id == Builtins::GlobalThis)
        return realm->globalThis();
    auto& builtin = table.at(id);
    if (!builtin.holder) {
        DeferTerminationForAWhile deferTermination(vm);
        auto root = findHiddenRoot(realm, id);
        return root ? JSValue(*root) : throwNoPath();
    }
    JSValue base = getByPath(realm, builtin.holder);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = base.get(realm, table.identifierOf(vm, builtin.name));
    RETURN_IF_EXCEPTION(scope, { });
    return value;
}

static JSObject* objectAt(JSObject* holder, PropertyOffset offset)
{
    JSValue value = holder->getDirect(offset);
    return value ? value.getObject() : nullptr;
}

JSObject* findExistingObject(JSObject* holder, UniquedStringImpl* key)
{
    PropertyOffset offset = holder->structure()->getConcurrently(key);
    return isValidOffset(offset) ? objectAt(holder, offset) : nullptr;
}

// Also runs at the end of a collection, on the collector's thread: it creates no atom, no cell and no property table.
class ExistingBuiltins {
public:
    explicit ExistingBuiltins(JSGlobalObject* realm)
        : m_realm(realm)
        , m_table(BuiltinTable::singleton())
        , m_objects(FillWith { }, m_table.builtinCount(), nullptr)
        , m_holdersRead(m_table.builtinCount())
    {
    }

    JSObject* find(BuiltinID);

private:
    JSObject* findHiddenRoot(BuiltinID);
    JSObject* findSymbolMember(const Builtin&);
    void readTextMembers(BuiltinID holder, JSObject*);

    JSGlobalObject* m_realm;
    const BuiltinTable& m_table;
    Vector<JSObject*> m_objects;
    BitVector m_holdersRead;
};

JSObject* ExistingBuiltins::find(BuiltinID id)
{
    if (id == Builtins::GlobalThis)
        return m_realm;
    auto& builtin = m_table.at(id);
    if (!builtin.holder)
        return findHiddenRoot(id);
    if (m_table.isSymbolName(builtin.name))
        return findSymbolMember(builtin);
    if (m_holdersRead.add(builtin.holder.index())) {
        if (JSObject* holder = find(builtin.holder))
            readTextMembers(builtin.holder, holder);
    }
    return m_objects[id.index()];
}

JSObject* ExistingBuiltins::findHiddenRoot(BuiltinID id)
{
    if (BuiltinID subclassConstructor = findSubclassConstructor(m_table, id)) {
        JSObject* constructor = find(subclassConstructor);
        return constructor ? constructor->getPrototypeDirect().getObject() : nullptr;
    }
    BuiltinID prototypeID = findClassPrototype(m_table, id);
    JSObject* prototype = prototypeID ? findExistingHiddenPrototype(m_realm, m_table.at(id).instanceClass) : nullptr;
    if (!prototype || id == prototypeID)
        return prototype;
    return findExistingObject(prototype, m_realm->vm().propertyNames->constructor.impl());
}

JSObject* ExistingBuiltins::findSymbolMember(const Builtin& builtin)
{
    JSObject* holder = m_table.wellKnownSymbolOf(builtin.name) ? find(builtin.holder) : nullptr;
    return holder ? findExistingObject(holder, m_table.identifierOf(m_realm->vm(), builtin.name).impl()) : nullptr;
}

void ExistingBuiltins::readTextMembers(BuiltinID holder, JSObject* object)
{
    object->structure()->forEachPropertyConcurrently([&](const PropertyTableEntry& entry) {
        BuiltinName name = m_table.textNameOf(entry.key());
        BuiltinID member = name ? m_table.findOwnMember(holder, name) : BuiltinID();
        // The property globalThis may hold a proxy of the global object.
        if (member && member != holder)
            m_objects[member.index()] = objectAt(object, entry.offset());
        return true;
    });
}

void forEachLockedObject(JSGlobalObject* realm, const ScopedLambda<void(BuiltinID, JSObject*)>& function)
{
    ExistingBuiltins existing(realm);
    for (BuiltinID id : BuiltinTable::singleton().lockedObjects()) {
        if (JSObject* object = existing.find(id))
            function(id, object);
    }
}

namespace {

class FixedNamesOfBuiltin final : public ImmutablePropertyNames::OutsideNames {
public:
    FixedNamesOfBuiltin(VM& vm, BuiltinID holder, std::span<const uint32_t> constantMembers, RefPtr<const OutsideNames>&& namesOfProgram)
        : m_vm(vm)
        , m_holder(holder)
        , m_closedKeys(BuiltinTable::singleton().closedKeysOf(holder))
        , m_constantMembers(constantMembers)
        , m_namesOfProgram(WTF::move(namesOfProgram))
    {
    }

    bool contains(UniquedStringImpl* uid) const final
    {
        BuiltinName name = BuiltinTable::singleton().nameOf(m_vm, uid);
        if (name && isFixed(name))
            return true;
        return m_namesOfProgram && m_namesOfProgram->contains(uid);
    }

    String messageOfRefusal(UniquedStringImpl* uid) const final
    {
        String holder = BuiltinTable::singleton().textOf(m_holder);
        String property = uid->isSymbol() ? makeString(holder, "[Symbol("_s, StringView(uid), ")]"_s) : makeString(holder, '.', StringView(uid));
        if (m_holder == Builtins::GlobalThis && !uid->isSymbol())
            property = String(uid);
        BuiltinName name = BuiltinTable::singleton().nameOf(m_vm, uid);
        if (!(name && isFixed(name)) && m_namesOfProgram && m_namesOfProgram->contains(uid))
            return withAOTErrorNote(makeString("Cannot add "_s, property, ": the compiled program looks for this property, and it was compiled to find that "_s, holder, " does not have it. Add it in code that is compiled with the program"_s));
        return withAOTErrorNote(makeString("Cannot assign to "_s, property, " because built-in objects are read-only in a compiled executable"_s));
    }

    String messageOfRefusedPrototype() const final
    {
        return withAOTErrorNote(makeString("Cannot change the prototype of "_s, BuiltinTable::singleton().textOf(m_holder), " because built-in objects are read-only in a compiled executable"_s));
    }

private:
    bool isFixed(BuiltinName name) const
    {
        if (std::ranges::binary_search(m_closedKeys, name))
            return true;
        const BuiltinTable& table = BuiltinTable::singleton();
        BuiltinID member = table.findOwnMember(m_holder, name);
        if (!member)
            return false;
        // The global object is its own member `globalThis`, which no list of members holds.
        if (member == m_holder || std::ranges::binary_search(table.lockedMembers(), member))
            return true;
        const Builtin& entry = table.at(member);
        if (entry.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances) && table.at(m_holder).flags.contains(BuiltinFlag::HasLockedMembers))
            return true;
        // The global object of a realm without the host has none of the host's members, and whoever made the realm defines properties of its own under their names.
        if (m_holder == Builtins::GlobalThis && !BuiltinBindings::provides(entry.origin))
            return false;
        return std::ranges::binary_search(m_constantMembers, member.index());
    }

    VM& m_vm;
    BuiltinID m_holder;
    std::span<const BuiltinName> m_closedKeys;
    std::span<const uint32_t> m_constantMembers;
    RefPtr<const OutsideNames> m_namesOfProgram;
};

} // anonymous namespace

Ref<ImmutablePropertyNames> immutablePropertyNamesOf(VM& vm, BuiltinID holder, std::span<const uint32_t> constantMembers, RefPtr<const ImmutablePropertyNames::OutsideNames>&& namesOfProgram)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    OptionSet<ImmutablePropertyNames::Part> parts;
    if (table.closesIndexKeys(holder))
        parts.add(ImmutablePropertyNames::Part::NumericKeys);
    if (table.hasLockedPrototypeLink(holder) || namesOfProgram)
        parts.add(ImmutablePropertyNames::Part::Prototype);

    IdentifierSet openStrings;
    if (table.at(holder).flags.contains(BuiltinFlag::IsClosedToOtherNames)) {
        parts.add(ImmutablePropertyNames::Part::OtherStrings);
        for (BuiltinName name : table.openNamesOf(holder))
            openStrings.add(table.identifierOf(vm, name).impl());
        for (BuiltinID member : table.membersOf(holder)) {
            if (table.at(member).mutability != BuiltinMutability::AlwaysLocked && !table.at(member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances))
                openStrings.add(table.identifierOf(vm, table.at(member).name).impl());
        }
    }
    return ImmutablePropertyNames::create({ }, parts, WTF::move(openStrings), adoptRef(*new FixedNamesOfBuiltin(vm, holder, constantMembers, WTF::move(namesOfProgram))));
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
