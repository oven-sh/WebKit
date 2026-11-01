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
#include "AOTProgramTables.h"
#include "AOTVector.h"
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
#include <wtf/FastBitVector.h>
#include <mutex>
#include <wtf/BitVector.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringView.h>

namespace JSC::AOT {

enum class PropertyKind : uint8_t { Data, Getter, Setter };

using Lookup = std::expected<JSValue, BuiltinLookupFailure>;
using ObjectLookup = std::expected<JSObject*, BuiltinLookupFailure>;
using CellLookup = std::expected<JSCell*, BuiltinLookupFailure>;

static bool hasValueCell(const BuiltinTable& table, BuiltinID id)
{
    const Builtin& member = table.at(id);
    if (member.scope != BuiltinScope::Realm)
        return false;
    // Its holder adds it when the program first reads it. Compiled code may have the object, or know the value, long before.
    if (member.isAddedAtFirstRead)
        return true;
    return member.kind == BuiltinKind::DataProperty
        && member.mutability != BuiltinMutability::ChangesOnItsOwn
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

bool groupsFunctionsOfCompiledCode(BuiltinID id)
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

BuiltinID findMemberOfNamespaceCreatedWith(JSGlobalObject* realm, const LazyClassStructure& lazyStructure)
{
    struct ClassOfNamespace {
        const LazyClassStructure* lazyStructure;
        ASCIILiteral holder;
        ASCIILiteral name;
    };
    const ClassOfNamespace classes[] = {
        { &realm->m_durationStructure, "Temporal"_s, "Duration"_s },
        { &realm->m_instantStructure, "Temporal"_s, "Instant"_s },
        { &realm->m_plainDateStructure, "Temporal"_s, "PlainDate"_s },
        { &realm->m_plainDateTimeStructure, "Temporal"_s, "PlainDateTime"_s },
        { &realm->m_plainMonthDayStructure, "Temporal"_s, "PlainMonthDay"_s },
        { &realm->m_plainTimeStructure, "Temporal"_s, "PlainTime"_s },
        { &realm->m_plainYearMonthStructure, "Temporal"_s, "PlainYearMonth"_s },
        { &realm->m_zonedDateTimeStructure, "Temporal"_s, "ZonedDateTime"_s },
#define AOT_CLASS_OF_WEBASSEMBLY(capitalName, lowerName, properName, instanceType, jsName, prototypeBase, featureFlag) { &realm->m_ ## properName ## Structure, "WebAssembly"_s, #jsName ## _s },
        FOR_EACH_WEBASSEMBLY_CONSTRUCTOR_TYPE(AOT_CLASS_OF_WEBASSEMBLY)
#undef AOT_CLASS_OF_WEBASSEMBLY
    };
    const BuiltinTable& table = BuiltinTable::singleton();
    for (const ClassOfNamespace& entry : classes) {
        if (entry.lazyStructure != &lazyStructure)
            continue;
        BuiltinName holderName = table.nameOf(byteCast<uint8_t>(entry.holder.span()));
        BuiltinID holder = holderName ? table.findGlobal(holderName) : BuiltinID();
        BuiltinName name = holder ? table.nameOf(byteCast<uint8_t>(entry.name.span())) : BuiltinName();
        return name ? table.findOwnMember(table.canonicalID(holder), name) : BuiltinID();
    }
    return { };
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

// The second argument of the visitor answers the Structure, or null while the realm has not made it.
template<typename Visitor>
static ALWAYS_INLINE void visitInstanceStructuresOfEngine(JSGlobalObject* realm, const Visitor& visit)
{
#define AOT_VISIT_INSTANCE_STRUCTURE(name, structure) visit(BuiltinClasses::name, [&]() ALWAYS_INLINE_LAMBDA { return structure; });
    for (IndexingType indexingType : { ArrayWithUndecided, ArrayWithInt32, ArrayWithDouble, ArrayWithContiguous, ArrayWithArrayStorage, CopyOnWriteArrayWithInt32, CopyOnWriteArrayWithDouble, CopyOnWriteArrayWithContiguous })
        AOT_VISIT_INSTANCE_STRUCTURE(Array, realm->originalArrayStructureForIndexingType(indexingType))
    AOT_VISIT_INSTANCE_STRUCTURE(Map, realm->mapStructureConcurrently())
    AOT_VISIT_INSTANCE_STRUCTURE(Set, realm->setStructureConcurrently())
    AOT_VISIT_INSTANCE_STRUCTURE(WeakMap, realm->weakMapStructureConcurrently())
    AOT_VISIT_INSTANCE_STRUCTURE(WeakSet, realm->weakSetStructureConcurrently())
    AOT_VISIT_INSTANCE_STRUCTURE(ArrayBuffer, realm->arrayBufferStructureConcurrently(ArrayBufferSharingMode::Default))
    AOT_VISIT_INSTANCE_STRUCTURE(SharedArrayBuffer, realm->arrayBufferStructureConcurrently(ArrayBufferSharingMode::Shared))
    AOT_VISIT_INSTANCE_STRUCTURE(DataView, realm->typedArrayStructureConcurrently(TypeDataView, false))
    AOT_VISIT_INSTANCE_STRUCTURE(RegExp, realm->regExpStructure())
    AOT_VISIT_INSTANCE_STRUCTURE(Promise, realm->promiseStructure())
    AOT_VISIT_INSTANCE_STRUCTURE(String, realm->stringObjectStructure())
    AOT_VISIT_INSTANCE_STRUCTURE(Error, realm->errorStructureConcurrently())
    AOT_VISIT_INSTANCE_STRUCTURE(Date, realm->dateStructureConcurrently())
    AOT_VISIT_INSTANCE_STRUCTURE(IntlDateTimeFormat, realm->m_dateTimeFormatStructure.getConcurrently())
    AOT_VISIT_INSTANCE_STRUCTURE(IntlNumberFormat, realm->m_numberFormatStructure.getConcurrently())
#define AOT_VISIT_TYPED_ARRAY_STRUCTURE(name) AOT_VISIT_INSTANCE_STRUCTURE(name##Array, realm->typedArrayStructureConcurrently(Type##name, false))
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(AOT_VISIT_TYPED_ARRAY_STRUCTURE)
#undef AOT_VISIT_TYPED_ARRAY_STRUCTURE
    AOT_VISIT_INSTANCE_STRUCTURE(ArrayIterator, realm->arrayIteratorStructure())
    AOT_VISIT_INSTANCE_STRUCTURE(MapIterator, realm->mapIteratorStructure())
    AOT_VISIT_INSTANCE_STRUCTURE(SetIterator, realm->setIteratorStructure())
    AOT_VISIT_INSTANCE_STRUCTURE(StringIterator, realm->stringIteratorStructure())
    AOT_VISIT_INSTANCE_STRUCTURE(RegExpStringIterator, realm->regExpStringIteratorStructure())
    AOT_VISIT_INSTANCE_STRUCTURE(IteratorHelper, realm->iteratorHelperStructure())
    AOT_VISIT_INSTANCE_STRUCTURE(WrapForValidIterator, realm->wrapForValidIteratorStructure())
    AOT_VISIT_INSTANCE_STRUCTURE(AsyncFromSyncIterator, realm->asyncFromSyncIteratorStructure())
#undef AOT_VISIT_INSTANCE_STRUCTURE
}

template<typename Function>
static ALWAYS_INLINE void forEachInstanceStructureOfEngine(JSGlobalObject* realm, const Function& function)
{
    visitInstanceStructuresOfEngine(realm, [&](BuiltinClassID builtinClass, const auto& findStructure) ALWAYS_INLINE_LAMBDA {
        if (Structure* structure = findStructure())
            function(builtinClass, structure);
    });
}

bool engineNamesInstanceStructureOf(BuiltinClassID builtinClass)
{
    // Not a BitVector: that keeps the address of its words shifted, and a leak checker takes words that nothing points to for lost.
    static NeverDestroyed<FastBitVector> namedClasses = [] {
        FastBitVector classes(BuiltinTable::singleton().classCount());
        visitInstanceStructuresOfEngine(nullptr, [&](BuiltinClassID named, const auto&) ALWAYS_INLINE_LAMBDA {
            classes[named.index()] = true;
        });
        return classes;
    }();
    return namedClasses->at(builtinClass.index());
}

void forEachInstanceStructure(JSGlobalObject* realm, const ScopedLambda<void(BuiltinClassID, Structure*)>& function)
{
    forEachInstanceStructureOfEngine(realm, function);
    BuiltinBindings::forEachInstanceStructureOfHost(realm, [&](const BuiltinClassBinding& binding, Structure* structure) {
        function(binding.builtinClass, structure);
    });
}

BuiltinClassID findClassWithInstanceStructure(JSGlobalObject* realm, Structure* structure)
{
    BuiltinClassID found;
    forEachInstanceStructureOfEngine(realm, [&](BuiltinClassID builtinClass, Structure* original) ALWAYS_INLINE_LAMBDA {
        if (original == structure)
            found = builtinClass;
    });
    const ClassInfo* info = structure->classInfoForCells();
    BuiltinBindings::forEachInstanceStructureOfHost(realm, [&](const BuiltinClassBinding& binding, Structure* original) {
        if (original == structure && (!binding.instanceClassInfo || info->isSubClassOf(binding.instanceClassInfo)))
            found = binding.builtinClass;
    });
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
    if (id == Builtins::ModuleRequirePrototype)
        return findTableOfLoader(realm, &ModuleHost::requirePrototype);
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

static JSObject* findPrototypeWithoutProperty(JSGlobalObject* realm, BuiltinID id)
{
    auto& table = BuiltinTable::singleton();
    return findClassPrototype(table, id) == id ? findHiddenPrototype(realm, table.at(id).instanceClass) : nullptr;
}

static Lookup findOrCreateMember(JSGlobalObject* realm, BuiltinID id)
{
    Lookup found = findDeclaredProperty(realm, id, PropertyKind::Data);
    if (!found && (found.error() == BuiltinLookupFailure::IsMissing || found.error() == BuiltinLookupFailure::HolderIsMissing)) {
        if (JSObject* prototype = findPrototypeWithoutProperty(realm, id))
            return JSValue(prototype);
    }
    if (found || found.error() != BuiltinLookupFailure::HolderIsMissing)
        return found;
    if (auto binding = BuiltinBindings::nativeFunctionOf(id))
        return JSValue(JSFunction::create(realm->vm(), realm, binding->length, binding->name, binding->function, binding->name.isNull() ? ImplementationVisibility::Private : ImplementationVisibility::Public));
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
    if (kind == PropertyKind::Data && id == Builtins::GlobalThis) {
        if (JSObject* globalThis = realm->globalThis())
            return static_cast<JSCell*>(globalThis);
        return makeUnexpected(BuiltinLookupFailure::IsMissing);
    }
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

JSObject* findExistingBuiltin(JSGlobalObject* realm, BuiltinID id)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    if (!table.contains(id) || table.at(id).scope == BuiltinScope::Module)
        return nullptr;
    if (id == Builtins::GlobalThis)
        return realm;
    VM& vm = realm->vm();
    const Builtin& builtin = table.at(id);
    if (builtin.holder) {
        if (table.isSymbolName(builtin.name) && !table.wellKnownSymbolOf(builtin.name))
            return nullptr;
        JSObject* holder = findExistingBuiltin(realm, builtin.holder);
        return holder ? findExistingObject(holder, table.identifierOf(vm, builtin.name).impl()) : nullptr;
    }
    if (BuiltinID subclassConstructor = findSubclassConstructor(table, id)) {
        JSObject* constructor = findExistingBuiltin(realm, subclassConstructor);
        return constructor ? constructor->getPrototypeDirect().getObject() : nullptr;
    }
    BuiltinID prototypeID = findClassPrototype(table, id);
    JSObject* prototype = prototypeID ? findExistingHiddenPrototype(realm, builtin.instanceClass) : nullptr;
    if (!prototype || id == prototypeID)
        return prototype;
    return findExistingObject(prototype, vm.propertyNames->constructor.impl());
}

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
    FixedNamesOfBuiltin(VM& vm, BuiltinID holder, std::span<const uint32_t> constantMembers, WritesToBuiltinMembers* writes, std::span<const uint32_t> unlockedMembers, bool reflectThrows)
        : m_vm(vm)
        , m_holder(holder)
        , m_closedKeys(BuiltinTable::singleton().closedKeysOf(holder))
        , m_constantMembers(constantMembers)
        , m_unlockedMembers(unlockedMembers)
        , m_reflectThrows(reflectThrows)
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
        const BuiltinTable& table = BuiltinTable::singleton();
        BuiltinName name = table.nameOf(m_vm, uid);
        // The strings of such an object are closed by Part::OtherStrings.
        if (table.at(m_holder).flags.contains(BuiltinFlag::IsLockedObject)) {
            if (!uid->isSymbol() || static_cast<SymbolImpl*>(uid)->isPrivate())
                return false;
            BuiltinID member = name ? table.findOwnMember(m_holder, name) : BuiltinID();
            return !member || !ProgramTables::isUnlocked(m_unlockedMembers, member.index());
        }
        return name && isFixed(name);
    }

    String storeErrorMessage(UniquedStringImpl* uid) const final { return assignmentToLockedBuiltinMessage(m_vm, m_holder, uid, lockedIn()); }
    String deleteErrorMessage(UniquedStringImpl* uid) const final { return deletionFromLockedBuiltinMessage(m_vm, m_holder, uid, lockedIn()); }
    bool throwsInSloppyMode() const final { return true; }
    bool throwsFromReflect() const final { return lockedIn() == LockedIn::TestRun; }

    String setPrototypeErrorMessage() const final
    {
        return prototypeChangeOfLockedBuiltinMessage(BuiltinTable::singleton().textOf(m_holder), lockedIn());
    }

private:
    // Reflect is told of a refusal in a test run, and nowhere else.
    LockedIn lockedIn() const { return m_reflectThrows && !isWrittenByFileOfProduct(m_vm) ? LockedIn::TestRun : LockedIn::Executable; }

    bool isFixed(BuiltinName name) const
    {
        if (std::ranges::binary_search(m_closedKeys, name))
            return true;
        const BuiltinTable& table = BuiltinTable::singleton();
        BuiltinID member = table.findOwnMember(m_holder, name);
        if (!member)
            return false;
        const Builtin& entry = table.at(member);
        if (m_holder == Builtins::GlobalThis && !BuiltinBindings::provides(entry.origin))
            return false;
        if (member == m_holder || (std::ranges::binary_search(table.lockedMembers(), member) && !ProgramTables::isUnlocked(m_unlockedMembers, member.index())))
            return true;
        if (entry.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances) && table.at(m_holder).flags.contains(BuiltinFlag::HasLockedMembers))
            return true;
        return std::ranges::binary_search(m_constantMembers, member.index());
    }

    VM& m_vm;
    BuiltinID m_holder;
    std::span<const BuiltinName> m_closedKeys;
    std::span<const uint32_t> m_constantMembers;
    std::span<const uint32_t> m_unlockedMembers;
    bool m_reflectThrows;
    WritesToBuiltinMembers* m_writes;
};

} // anonymous namespace

Ref<ImmutablePropertyNames> immutablePropertyNamesOf(VM& vm, BuiltinID holder, std::span<const uint32_t> constantMembers, WritesToBuiltinMembers* writes, std::span<const uint32_t> unlockedMembers, bool reflectThrows, std::span<const Identifier> openedNames)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    OptionSet<ImmutablePropertyNames::Part> parts;
    if (table.closesIndexKeys(holder))
        parts.add(ImmutablePropertyNames::Part::NumericKeys);
    if (table.hasLockedPrototypeLink(holder))
        parts.add(ImmutablePropertyNames::Part::Prototype);

    IdentifierSet openStrings;
    for (const Identifier& name : openedNames)
        openStrings.add(name.impl());
    if (table.at(holder).flags.containsAny({ BuiltinFlag::IsClosedToOtherNames, BuiltinFlag::IsLockedObject })) {
        parts.add(ImmutablePropertyNames::Part::OtherStrings);
        for (BuiltinName name : table.openNamesOf(holder))
            openStrings.add(table.identifierOf(vm, name).impl());
        for (BuiltinID member : table.membersOf(holder)) {
            bool isLocked = table.at(member).mutability == BuiltinMutability::AlwaysLocked && !ProgramTables::isUnlocked(unlockedMembers, member.index());
            if (!isLocked && !table.at(member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances))
                openStrings.add(table.identifierOf(vm, table.at(member).name).impl());
        }
    }
    return ImmutablePropertyNames::create({ }, parts, WTF::move(openStrings), adoptRef(*new FixedNamesOfBuiltin(vm, holder, constantMembers, writes, unlockedMembers, reflectThrows)));
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
