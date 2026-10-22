/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinTableRealm.h"

#if ENABLE(AOT)

#include "AOTBuiltinModules.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTErrorMessages.h"
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
#include "Lookup.h"
#include "MapIteratorPrototype.h"
#include "ObjectConstructor.h"
#include "PropertyDescriptor.h"
#include "SetIteratorPrototype.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <array>
#include <mutex>
#include <wtf/BitVector.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>

namespace JSC::AOT {

enum class PropertyKind : uint8_t { Data, Getter, Setter };

using Lookup = std::expected<JSValue, BuiltinLookupFailure>;
using ObjectLookup = std::expected<JSObject*, BuiltinLookupFailure>;
using CellLookup = std::expected<JSCell*, BuiltinLookupFailure>;

static bool hasValueCell(const BuiltinTable& table, BuiltinID id)
{
    const Builtin& member = table.at(id);
    return member.kind == BuiltinKind::DataProperty
        && member.scope == BuiltinScope::Realm
        && member.mutability == BuiltinMutability::ConstantUnlessWritten
        && member.flags.contains(BuiltinFlag::IsLazilyCreated)
        && !member.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances);
}

std::optional<BuiltinCellKey> BuiltinCellKey::decode(uint32_t bits, const BuiltinTable& table)
{
    BuiltinID id(bits & ((1u << 24) - 1));
    if (!table.contains(id))
        return std::nullopt;
    for (Kind kind : { Kind::Object, Kind::Getter, Kind::Setter }) {
        if (BuiltinCellKey key(kind, id); key.encoded() == bits)
            return key;
    }
    if (value(id).encoded() == bits && hasValueCell(table, id))
        return value(id);
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

static bool mayCallEntryFunction(const BuiltinOverloadSummary& overload)
{
    switch (overload.kind) {
    case BuiltinImplementationKind::HostCall:
        return overload.callee != BuiltinCallee::CustomGetter;
    case BuiltinImplementationKind::Compiled:
    case BuiltinImplementationKind::CachedGetter:
        return true;
    case BuiltinImplementationKind::Inline:
        return overload.hasOtherwise;
    case BuiltinImplementationKind::Generic:
    case BuiltinImplementationKind::Number:
    case BuiltinImplementationKind::Cell:
    case BuiltinImplementationKind::OwnSlot:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasAccessorCallee(const BuiltinTable& table, BuiltinID id, BuiltinCallMode mode)
{
    return table.at(id).kind == BuiltinKind::Accessor && std::ranges::any_of(table.overloadSummariesOf(id), [&](const BuiltinOverloadSummary& overload) {
        return overload.mode == mode && mayCallEntryFunction(overload);
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
    case Kind::Value:
        return hasValueCell(table, m_builtin);
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

struct ClassesOfStaticTables {
    const ClassInfo* infoOfRealm { nullptr };
    Vector<const HashTableValue*> entries;
};

static const ClassesOfStaticTables& classesOfStaticTablesOfFirstRealm(JSGlobalObject* realm)
{
    static std::once_flag once;
    static LazyNeverDestroyed<ClassesOfStaticTables> classes;
    std::call_once(once, [&] {
        classes.construct();
        classes->infoOfRealm = realm->classInfo();
        forEachStaticTableOf(realm, [&](const HashTable& hashTable) {
            for (auto& entry : hashTable) {
                if (entry.attributes() & PropertyAttribute::ClassStructure)
                    classes->entries.append(&entry);
            }
        });
    });
    return classes.get();
}

BuiltinID findGlobalCreatedWith(JSGlobalObject* realm, const LazyClassStructure& lazyStructure)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    auto globalOf = [&](const HashTableValue& entry) -> BuiltinID {
        if (lazyClassStructureOf(realm, entry) != &lazyStructure)
            return { };
        BuiltinName name = table.nameOf(byteCast<uint8_t>(entry.m_key.span()));
        return name ? table.findGlobal(name) : BuiltinID();
    };
    const ClassesOfStaticTables& classes = classesOfStaticTablesOfFirstRealm(realm);
    if (classes.infoOfRealm == realm->classInfo()) [[likely]] {
        for (const HashTableValue* entry : classes.entries) {
            if (BuiltinID found = globalOf(*entry))
                return found;
        }
        return { };
    }
    for (const ClassInfo* info = realm->classInfo(); info; info = info->parentClass) {
        const HashTable* hashTable = info->staticPropHashTable;
        if (!hashTable)
            continue;
        for (auto& entry : *hashTable) {
            if (!(entry.attributes() & PropertyAttribute::ClassStructure))
                continue;
            if (BuiltinID found = globalOf(entry))
                return found;
        }
    }
    return { };
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

template<typename Function>
static ALWAYS_INLINE void forEachInstanceStructureOfEngine(JSGlobalObject* realm, const Function& function)
{
    auto visitIfCreated = [&](BuiltinClassID builtinClass, Structure* structure) ALWAYS_INLINE_LAMBDA {
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
}

void forEachInstanceStructure(JSGlobalObject* realm, const ScopedLambda<void(BuiltinClassID, Structure*)>& function)
{
    forEachInstanceStructureOfEngine(realm, function);
    for (const BuiltinClassBinding& binding : BuiltinBindings::classesOfHost()) {
        Structure* structure = binding.findInstanceStructure ? binding.findInstanceStructure(realm) : nullptr;
        if (structure)
            function(binding.builtinClass, structure);
    }
}

BuiltinClassID findClassWithInstanceStructure(JSGlobalObject* realm, Structure* structure)
{
    BuiltinClassID found;
    forEachInstanceStructureOfEngine(realm, [&](BuiltinClassID builtinClass, Structure* original) ALWAYS_INLINE_LAMBDA {
        if (original == structure)
            found = builtinClass;
    });
    const ClassInfo* info = structure->classInfoForCells();
    for (const BuiltinClassBinding& binding : BuiltinBindings::classesOfHost()) {
        if (binding.instanceClassInfo && info->isSubClassOf(binding.instanceClassInfo) && binding.findInstanceStructure && binding.findInstanceStructure(realm) == structure)
            found = binding.builtinClass;
    }
    return found;
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

// No name leads from the global object to the two tables of the loader: each module reads them from a require function of its own.
static ObjectLookup findTableOfLoader(JSGlobalObject* realm, JSObject* (*ModuleHost::*table)(JSGlobalObject*))
{
    const ModuleHost* host = realm->globalObjectMethodTable()->moduleHost;
    JSObject* object = host && host->*table ? (host->*table)(realm) : nullptr;
    if (!object)
        return makeUnexpected(BuiltinLookupFailure::HolderIsMissing);
    return object;
}

static ObjectLookup findHiddenRoot(JSGlobalObject* realm, BuiltinID id)
{
    if (id == Builtins::ModuleCache)
        return findTableOfLoader(realm, &ModuleHost::requireCache);
    if (id == Builtins::ModuleExtensions)
        return findTableOfLoader(realm, &ModuleHost::requireExtensions);
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
    for (const BuiltinOverloadSummary& overload : BuiltinTable::singleton().overloadSummariesOf(id)) {
        if (overload.mode == BuiltinCallMode::Call && overload.function)
            return builtinFunction(realm, overload.function.index());
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
    case BuiltinCallee::CustomGetter:
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
    case BuiltinCellKey::Kind::Value:
        return false;
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
    for (const BuiltinOverloadSummary& overload : table.overloadSummariesOf(id)) {
        if (isOverloadOfKey(key.kind(), overload.mode) && !hasExpectedCallee(overload.callee, value))
            return makeUnexpected(BuiltinLookupFailure::HasOtherCallee);
    }
    // Compiled code takes a native function to have been born with the Structure of the host functions of its realm, for what that Structure refuses.
    // A function that the library writes in JavaScript refuses the same as a closure. Where its code is not in the image another function stands for it, which nothing compiled calls.
    if (key.kind() == BuiltinCellKey::Kind::Object && table.isHostFunctionOfEngine(id)) {
        Structure* structure = value.asCell()->structure();
        if (structure->globalObject()->hostFunctionStructure()->mayHaveImmutableProperties() && !structure->mayHaveImmutableProperties())
            return makeUnexpected(BuiltinLookupFailure::HasOtherCallee);
    }
    return value.asCell();
}

static PropertyKind propertyKindOf(BuiltinCellKey::Kind kind)
{
    switch (kind) {
    case BuiltinCellKey::Kind::Object:
    case BuiltinCellKey::Kind::Value:
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

static const HashTableValue* findStaticTableEntry(JSObject* holder, const Identifier& key, const ClassInfo*& classInfo)
{
    for (classInfo = holder->classInfo(); classInfo; classInfo = classInfo->parentClass) {
        const HashTable* hashTable = classInfo->staticPropHashTable;
        if (const HashTableValue* entry = hashTable ? hashTable->entry(key) : nullptr)
            return entry;
    }
    return nullptr;
}

std::expected<JSCell*, BuiltinLookupFailure> makeMemberOfStaticTable(JSGlobalObject* realm, BuiltinCellKey key, JSObject* holder)
{
    VM& vm = realm->vm();
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto& table = BuiltinTable::singleton();
    Identifier name = table.identifierOf(vm, table.at(key.builtin()).name);
    const ClassInfo* classInfo = nullptr;
    const HashTableValue* entry = findStaticTableEntry(holder, name, classInfo);
    if (!entry)
        return makeUnexpected(BuiltinLookupFailure::IsMissing);

    unsigned attributes = entry->attributes();
    if (attributes & (PropertyAttribute::CellProperty | PropertyAttribute::ClassStructure | PropertyAttribute::PropertyCallback)) {
        if (key.kind() != BuiltinCellKey::Kind::Object)
            return makeUnexpected(BuiltinLookupFailure::IsNotAccessor);
        JSValue made;
        if (attributes & PropertyAttribute::CellProperty)
            made = std::bit_cast<LazyCellProperty*>(std::bit_cast<char*>(holder) + entry->lazyCellPropertyOffset())->get(holder);
        else if (attributes & PropertyAttribute::ClassStructure)
            made = std::bit_cast<LazyClassStructure*>(std::bit_cast<char*>(holder) + entry->lazyClassStructureOffset())->constructor(uncheckedDowncast<JSGlobalObject>(holder));
        else
            made = entry->lazyPropertyCallback()(vm, holder);
        if (scope.exception()) [[unlikely]] {
            scope.clearException();
            return makeUnexpected(BuiltinLookupFailure::LookupThrew);
        }
        if (!made)
            return makeUnexpected(BuiltinLookupFailure::IsMissing);
        return checkFound(table, key, made);
    }
    // The getter of any other entry is handed the object that it is read from.
    if (!(attributes & (PropertyAttribute::Builtin | PropertyAttribute::Function | PropertyAttribute::Accessor)))
        return makeUnexpected(BuiltinLookupFailure::IsMissing);

    JSObject* standIn = constructEmptyObject(realm);
    reifyStaticProperty(vm, classInfo, name, *entry, *standIn);
    Lookup found = findOwnProperty(realm, standIn, name, propertyKindOf(key.kind()));
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
        throwException(realm, scope, createBuiltinWithoutPathError(realm));
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
    FixedNamesOfBuiltin(VM& vm, BuiltinID holder, std::span<const uint32_t> constantMembers, WritesToBuiltinMembers* writes)
        : m_vm(vm)
        , m_holder(holder)
        , m_closedKeys(BuiltinTable::singleton().closedKeysOf(holder))
        , m_constantMembers(constantMembers)
        , m_writes(writes)
    {
    }

    void willWrite(UniquedStringImpl* uid) const final
    {
        if (!m_writes || m_vm.allowLazyMaterializationOfImmutablePropertiesCount)
            return;
        const BuiltinTable& table = BuiltinTable::singleton();
        BuiltinName name = table.nameOf(m_vm, uid);
        if (BuiltinID member = name ? table.findOwnMember(m_holder, name) : BuiltinID())
            m_writes->willWrite(member, uid);
    }

    bool contains(UniquedStringImpl* uid) const final
    {
        BuiltinName name = BuiltinTable::singleton().nameOf(m_vm, uid);
        return name && isFixed(name);
    }

    String messageOfRefusal(UniquedStringImpl* uid) const final { return assignmentToLockedBuiltinMessage(memberOfLockedBuiltin(m_holder, uid)); }
    String messageOfRefusedDeletion(UniquedStringImpl* uid) const final { return deletionFromLockedBuiltinMessage(memberOfLockedBuiltin(m_holder, uid)); }
    bool refusalAlwaysThrows() const final { return true; }

    String messageOfRefusedPrototype() const final
    {
        return prototypeChangeOfLockedBuiltinMessage(BuiltinTable::singleton().textOf(m_holder));
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
    WritesToBuiltinMembers* m_writes;
};

} // anonymous namespace

Ref<ImmutablePropertyNames> immutablePropertyNamesOf(VM& vm, BuiltinID holder, std::span<const uint32_t> constantMembers, WritesToBuiltinMembers* writes)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    OptionSet<ImmutablePropertyNames::Part> parts;
    if (table.closesIndexKeys(holder))
        parts.add(ImmutablePropertyNames::Part::NumericKeys);
    if (table.hasLockedPrototypeLink(holder))
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
    return ImmutablePropertyNames::create({ }, parts, WTF::move(openStrings), adoptRef(*new FixedNamesOfBuiltin(vm, holder, constantMembers, writes)));
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
