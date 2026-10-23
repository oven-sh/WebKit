/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinTable.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTNameClass.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include "ArgList.h"
#include "BuiltinNames.h"
#include "DeferTermination.h"
#include "GlobalObjectMethodTable.h"
#include "JSArrayBuffer.h"
#include "JSCInlines.h"
#include "JSPromise.h"
#include "MarkedVector.h"
#include "PropertyNameArray.h"
#include "PureNaN.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <bit>
#include <expected>
#include <wtf/HashMap.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static_assert(Int8ArrayType == FirstTypedArrayType && BigUint64ArrayType == LastTypedArrayTypeExcludingDataView);
static_assert(ArrayType + 1 == DerivedArrayType);

static constexpr BuiltinPlatform currentPlatform = targetOfThisBuild().platform;

using CellLookup = std::expected<JSCell*, BuiltinLookupFailure>;

struct RealmComparison {
    RealmComparison(const BuiltinTable& table, JSGlobalObject* realm)
        : table(table)
        , realm(realm)
    {
        objects.fill(realm->vm(), table.builtinCount(), [](JSObject**) { });
    }

    VM& vm() const { return realm->vm(); }
    JSObject* objectOf(BuiltinID id) const { return objects[id.index()]; }
    void report(BuiltinID id, const auto&... parts) { mismatches.append(makeString(table.textOf(id), ": "_s, parts...)); }
    void report(BuiltinClassID id, const auto&... parts) { mismatches.append(makeString(table.tokenOf(id), ": "_s, parts...)); }

    const BuiltinTable& table;
    JSGlobalObject* realm;
    MarkedVector<JSObject*> objects;
    Vector<String> mismatches;
};

static ASCIILiteral describe(BuiltinLookupFailure failure)
{
    switch (failure) {
    case BuiltinLookupFailure::HolderIsMissing:
        return "its holder is missing"_s;
    case BuiltinLookupFailure::IsMissing:
        return "is missing"_s;
    case BuiltinLookupFailure::IsAccessor:
        return "is an accessor"_s;
    case BuiltinLookupFailure::IsNotAccessor:
        return "is not an accessor"_s;
    case BuiltinLookupFailure::HasNoGetter:
        return "has no getter"_s;
    case BuiltinLookupFailure::HasNoSetter:
        return "has no setter"_s;
    case BuiltinLookupFailure::LookupThrew:
        return "cannot be looked up"_s;
    case BuiltinLookupFailure::IsNotObject:
        return "is not an object"_s;
    case BuiltinLookupFailure::IsNotSymbol:
        return "is not a symbol"_s;
    case BuiltinLookupFailure::HasOtherCallee:
        return "has another kind of callee than the table says"_s;
    case BuiltinLookupFailure::IsPerModule:
        return "exists once per module"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasHookThatCreates(JSGlobalObject* realm, BuiltinID id)
{
#if ENABLE(WEBASSEMBLY)
    if (id == Builtins::WebAssemblyCompileStreaming)
        return !!realm->globalObjectMethodTable()->compileStreaming;
    if (id == Builtins::WebAssemblyInstantiateStreaming)
        return !!realm->globalObjectMethodTable()->instantiateStreaming;
#else
    UNUSED_PARAM(realm);
    UNUSED_PARAM(id);
#endif
    return true;
}

static bool isExpectedInRealm(const RealmComparison& comparison, BuiltinID id)
{
    auto& table = comparison.table;
    auto& builtin = table.at(id);
    if (builtin.kind == BuiltinKind::NativeBinding || builtin.scope == BuiltinScope::Module || !table.existsOn(id, currentPlatform))
        return false;
    if (!hasHookThatCreates(comparison.realm, id) || table.has(BuiltinMark::IsAddedByOption, id))
        return false;
    return BuiltinBindings::provides(builtin.origin);
}

static bool hasDeclaredSetter(const BuiltinTable& table, BuiltinID id)
{
    return std::ranges::any_of(table.overloadsOf(table.canonicalID(id)), [](const BuiltinOverload& overload) {
        return overload.mode == BuiltinCallMode::Set && !overload.isUndeclared();
    });
}

static bool saysThereIsNoSetter(const BuiltinTable& table, BuiltinID id)
{
    return table.at(id).kind == BuiltinKind::Accessor && std::ranges::none_of(table.overloadsOf(table.canonicalID(id)), [](const BuiltinOverload& overload) {
        return overload.mode == BuiltinCallMode::Set;
    });
}

static void forEachCheckedKey(const BuiltinTable& table, BuiltinID id, NOESCAPE const Invocable<void(BuiltinCellKey)> auto& functor)
{
    if (BuiltinCellKey::object(id).hasCell(table))
        functor(BuiltinCellKey::object(id));
    if (table.at(id).kind != BuiltinKind::Accessor)
        return;
    functor(BuiltinCellKey::getter(id));
    if (hasDeclaredSetter(table, id))
        functor(BuiltinCellKey::setter(id));
}

static bool hasCheckedKey(const BuiltinTable& table, BuiltinID id)
{
    bool hasKey = false;
    forEachCheckedKey(table, id, [&](BuiltinCellKey) { hasKey = true; });
    return hasKey;
}

static void forEachFoundObject(const RealmComparison& comparison, NOESCAPE const Invocable<void(BuiltinID, JSObject*)> auto& functor)
{
    for (unsigned index = 0; index < comparison.table.builtinCount(); ++index) {
        if (JSObject* object = comparison.objectOf(BuiltinID(index)))
            functor(BuiltinID(index), object);
    }
}

static CellLookup findCell(const RealmComparison& comparison, BuiltinCellKey key)
{
    if (BuiltinID holder = comparison.table.at(key.builtin()).holder)
        return findInHolder(comparison.realm, key, comparison.objectOf(holder));
    auto found = findInRealm(comparison.realm, key);
    if (!found && found.error() == BuiltinLookupFailure::HolderIsMissing)
        return makeUnexpected(BuiltinLookupFailure::IsMissing);
    return found;
}

// No property path reaches a hidden root or a module namespace, and findInRealm() locates a host's only through the binding of its class.
static bool isUnreachableHostRoot(const Builtin& builtin, BuiltinLookupFailure failure)
{
    return !builtin.holder && builtin.origin == BuiltinOrigin::Host && failure == BuiltinLookupFailure::IsMissing;
}

static void checkCells(RealmComparison& comparison, BuiltinID id)
{
    auto& table = comparison.table;
    std::optional<BuiltinLookupFailure> reported;
    forEachCheckedKey(table, id, [&](BuiltinCellKey key) {
        auto found = findCell(comparison, key);
        if (found) {
            if (key.kind() == BuiltinCellKey::Kind::Object)
                comparison.objects[id.index()] = (*found)->getObject();
            return;
        }
        if (reported == found.error() || isUnreachableHostRoot(table.at(id), found.error()))
            return;
        reported = found.error();
        comparison.report(id, describe(*reported));
    });
    if (saysThereIsNoSetter(table, id) && findCell(comparison, BuiltinCellKey::setter(id)))
        comparison.report(id, "has a setter, and the table says it has none"_s);
    JSObject* object = comparison.objectOf(id);
    if (object && Typed::isCallableBuiltin(table, id) && !object->isCallable())
        comparison.report(id, "is not a function"_s);
}

static bool liesInside(const BuiltinType& type, JSValue value)
{
    if (type.isAnything())
        return true;
    if (value.isObject())
        return type.mayBeObject();
    if (value.isString())
        return type.primitives.contains(BuiltinPrimitive::String) || !type.stringLiterals.empty();
    if (value.isNumber()) {
        double number = value.asNumber();
        bool isInRange = number == std::trunc(number) && number >= static_cast<double>(type.integers.minimum) && number <= static_cast<double>(type.integers.maximum);
        return type.primitives.contains(BuiltinPrimitive::Number) || isInRange;
    }
    if (value.isBoolean())
        return type.primitives.contains(BuiltinPrimitive::Boolean);
    if (value.isUndefined())
        return type.primitives.contains(BuiltinPrimitive::Undefined);
    if (value.isNull())
        return type.primitives.contains(BuiltinPrimitive::Null);
    if (value.isSymbol())
        return type.primitives.contains(BuiltinPrimitive::Symbol);
    return value.isBigInt() && type.primitives.contains(BuiltinPrimitive::BigInt);
}

static void checkValue(RealmComparison& comparison, BuiltinID id)
{
    auto& table = comparison.table;
    auto& builtin = table.at(id);
    if (!builtin.holder)
        return;
    auto found = findValueInHolder(comparison.realm, comparison.objectOf(builtin.holder), table.identifierOf(comparison.vm(), builtin.name));
    if (!found) {
        // What changes on its own may not be there yet: the host puts some members on an object of the realm when a module is loaded.
        if (found.error() != BuiltinLookupFailure::IsMissing || builtin.mutability != BuiltinMutability::ChangesOnItsOwn)
            comparison.report(id, describe(found.error()));
        return;
    }
    for (const BuiltinOverload& overload : table.overloadsOf(id)) {
        if (overload.mode == BuiltinCallMode::Get && !liesInside(table.at(overload.result.type), *found))
            comparison.report(id, "holds a value outside the type that the table gives it"_s);
    }
    std::optional<double> number = table.numberOf(id);
    if (number && (!found->isNumber() || std::bit_cast<uint64_t>(purifyNaN(found->asNumber())) != std::bit_cast<uint64_t>(purifyNaN(*number))))
        comparison.report(id, "is not "_s, *number);
    if (std::optional<StringView> text = table.stringOf(id)) {
        String held = found->isString() ? String(asString(*found)->value(comparison.realm)) : String();
        if (held != *text)
            comparison.report(id, "is not "_s, *text);
    }
}

static void checkEntry(RealmComparison& comparison, BuiltinID id)
{
    if (comparison.table.at(id).flags.containsAny({ BuiltinFlag::IsOwnPropertyOfInstances, BuiltinFlag::IsDefaultExport }))
        return;
    if (hasCheckedKey(comparison.table, id))
        checkCells(comparison, id);
    else
        checkValue(comparison, id);
}

static void checkEntries(RealmComparison& comparison)
{
    auto& table = comparison.table;
    Vector<BuiltinID> worklist;
    for (unsigned index = 0; index < table.builtinCount(); ++index) {
        if (!table.at(BuiltinID(index)).holder)
            worklist.append(BuiltinID(index));
    }
    for (size_t position = 0; position < worklist.size(); ++position) {
        BuiltinID id = worklist[position];
        if (!isExpectedInRealm(comparison, id))
            continue;
        checkEntry(comparison, id);
        if (!comparison.objectOf(id))
            continue;
        for (BuiltinID member : table.membersOf(id))
            worklist.append(member);
    }
}

static void checkNativeFunction(RealmComparison& comparison, BuiltinID id, JSObject* object, const BuiltinNativeFunction& binding)
{
    auto* function = dynamicDowncast<JSFunction>(object);
    if (!function || !function->isHostFunction() || function->nativeFunction() != toTagged(NativeFunction(binding.function))) {
        comparison.report(id, "has another native function than the table says"_s);
        return;
    }
    auto length = findValueInHolder(comparison.realm, function, comparison.vm().propertyNames->length);
    if (!length || !length->isNumber() || length->asNumber() != binding.length)
        comparison.report(id, "has another length than the table says"_s);
}

static void checkNativeFunctions(RealmComparison& comparison)
{
    forEachFoundObject(comparison, [&](BuiltinID id, JSObject* object) {
        if (auto binding = BuiltinBindings::nativeFunctionOf(id))
            checkNativeFunction(comparison, id, object, *binding);
    });
}

static bool isSameFunction(JSValue a, JSValue b)
{
    auto* first = dynamicDowncast<JSFunction>(a);
    auto* second = dynamicDowncast<JSFunction>(b);
    if (a == b || !first || !second)
        return a == b;
    return first->isHostFunction() ? second->isHostFunction() && first->nativeFunction() == second->nativeFunction() : first->executable() == second->executable();
}

static void checkPrivateAlias(RealmComparison& comparison, BuiltinID id, JSObject* publicValue)
{
    VM& vm = comparison.vm();
    auto& builtin = comparison.table.at(id);
    auto* privateName = vm.propertyNames->builtinNames().lookUpPrivateName(comparison.table.identifierOf(vm, builtin.name));
    if (!privateName) {
        comparison.report(id, "under the private name: "_s, describe(BuiltinLookupFailure::IsMissing));
        return;
    }
    auto privateValue = findValueInHolder(comparison.realm, comparison.objectOf(builtin.holder), Identifier::fromUid(vm, privateName));
    if (!privateValue)
        comparison.report(id, "under the private name: "_s, describe(privateValue.error()));
    else if (!isSameFunction(publicValue, *privateValue))
        comparison.report(id, "under the private name is another function"_s);
}

static void checkPrivateAliases(RealmComparison& comparison)
{
    forEachFoundObject(comparison, [&](BuiltinID id, JSObject* object) {
        auto& builtin = comparison.table.at(id);
        if (builtin.holder && builtin.flags.contains(BuiltinFlag::HasPrivateAlias))
            checkPrivateAlias(comparison, id, object);
    });
}

static bool hasDeclaredPrototype(const RealmComparison& comparison, const Builtin& builtin, JSObject* object)
{
    JSValue prototype = object->getPrototypeDirect();
    switch (builtin.prototypeLink) {
    case BuiltinPrototypeLink::Unknown:
        return true;
    case BuiltinPrototypeLink::Null:
        return prototype.isNull();
    case BuiltinPrototypeLink::Builtin: {
        JSObject* expected = comparison.objectOf(builtin.inheritsFrom);
        return !expected || prototype == JSValue(expected);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void checkInheritance(RealmComparison& comparison)
{
    forEachFoundObject(comparison, [&](BuiltinID id, JSObject* object) {
        auto& builtin = comparison.table.at(id);
        // An embedder's global object may have prototypes of its own above Object.prototype.
        if (builtin.flags.contains(BuiltinFlag::IsGlobalObject))
            return;
        if (!hasDeclaredPrototype(comparison, builtin, object))
            comparison.report(id, "inherits from another object than the table says"_s);
    });
}

// A type names a constant object by its canonical id, and takes two ids for two objects.
static void checkObjectIdentities(RealmComparison& comparison)
{
    auto& table = comparison.table;
    UncheckedKeyHashMap<JSObject*, BuiltinID> canonicalIDs;
    forEachFoundObject(comparison, [&](BuiltinID id, JSObject* object) {
        BuiltinID canonicalID = table.canonicalID(id);
        BuiltinID earlierID = canonicalIDs.add(object, canonicalID).iterator->value;
        if (earlierID != canonicalID)
            comparison.report(id, "is the same object as "_s, table.textOf(earlierID), ", which the table does not say"_s);
        JSObject* original = comparison.objectOf(canonicalID);
        if (original && original != object)
            comparison.report(id, "is another object than "_s, table.textOf(canonicalID), ", which the table says it is"_s);
    });
}

static String describeMemberStep(const Identifier& key)
{
    if (key.isSymbol())
        return makeString('[', key.string(), ']');
    return makeString('.', key.string());
}

// The compiler takes a name of any other class, and every symbol, to be absent from Object.prototype.
static bool isOnObjectPrototype(const Identifier& key)
{
    if (key.isSymbol())
        return false;
    auto utf8 = key.utf8();
    return Typed::isOnObjectPrototype(Typed::classifyName(byteCast<uint8_t>(utf8.span())));
}

static void checkOwnKeys(RealmComparison& comparison, BuiltinID holder, JSObject* object)
{
    VM& vm = comparison.vm();
    auto& table = comparison.table;
    IdentifierSet members;
    for (BuiltinID member : table.membersOf(holder)) {
        if (table.existsOn(member, currentPlatform))
            members.add(table.identifierOf(vm, table.at(member).name).impl());
    }
    if (table.at(holder).flags.contains(BuiltinFlag::IsGlobalObject))
        members.add(table.identifierOf(vm, table.at(holder).name).impl());
    BuiltinKind kind = table.at(holder).kind;
    if (kind == BuiltinKind::Function || kind == BuiltinKind::Method) {
        for (BuiltinID member : table.membersOf(Builtins::FunctionPrototype)) {
            if (table.at(member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances))
                members.add(table.identifierOf(vm, table.at(member).name).impl());
        }
    }
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PropertyNameArrayBuilder keys(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, comparison.realm, keys, DontEnumPropertiesMode::Include);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        comparison.report(holder, describe(BuiltinLookupFailure::LookupThrew));
        return;
    }
    for (auto& key : keys) {
        bool isInTable = members.contains(key.impl()) && (holder != Builtins::ObjectPrototype || isOnObjectPrototype(key));
        if (!isInTable)
            comparison.mismatches.append(makeString(table.textOf(holder), describeMemberStep(key), ": is not in the table"_s));
    }
}

static void checkClosedHolders(RealmComparison& comparison)
{
    forEachFoundObject(comparison, [&](BuiltinID holder, JSObject* object) {
        auto& builtin = comparison.table.at(holder);
        // Whoever makes a realm without the host puts properties of its own on the global object.
        bool isClosedGlobalObject = builtin.flags.contains(BuiltinFlag::IsGlobalObject) && BuiltinBindings::provides(BuiltinOrigin::Host);
        bool isClosed = holder == Builtins::ObjectPrototype || builtin.flags.contains(BuiltinFlag::ListsAllMembers) || isClosedGlobalObject;
        if (isClosed && !builtin.sameObjectAs)
            checkOwnKeys(comparison, holder, object);
    });
}

// The flow analysis takes no object of the table for a promise, and none but a prototype object for a thenable.
static void checkThatNothingSettlesLater(RealmComparison& comparison)
{
    VM& vm = comparison.vm();
    forEachFoundObject(comparison, [&](BuiltinID id, JSObject* object) {
        if (object->inherits<JSPromise>())
            comparison.report(id, "is a promise"_s);
        if (comparison.table.at(id).kind == BuiltinKind::Prototype)
            return;
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool hasThen = object->getPropertySlot(comparison.realm, vm.propertyNames->then, slot);
        if (scope.exception()) [[unlikely]]
            scope.clearException();
        else if (hasThen)
            comparison.report(id, "has a property `then`"_s);
    });
}

static void checkClassInfo(RealmComparison& comparison, BuiltinClassID classID)
{
    auto& table = comparison.table;
    const ClassInfo* info = BuiltinBindings::instanceClassInfoOf(classID);
    if (!info)
        return;
    for (const ClassInfo* inherited = info; inherited; inherited = inherited->parentClass) {
        BuiltinClassID other = BuiltinBindings::classOf(inherited);
        if (!other || BuiltinBindings::instanceClassInfoOf(other) != inherited)
            continue;
        if (Typed::relate(table, classID, other) == Typed::ClassRelation::Disjoint)
            comparison.report(classID, "has a C++ class that inherits from the one of "_s, table.tokenOf(other), ", which the table says is unrelated"_s);
    }
    if (info->methodTable.getCallData != &JSCell::getCallData && !Typed::isFunctionClass(table, classID))
        comparison.report(classID, "has a C++ class with callable instances, and the table says its instances are not functions"_s);
    for (BuiltinClassID ancestor = table.at(classID).parent; ancestor; ancestor = table.at(ancestor).parent) {
        const ClassInfo* ancestorInfo = BuiltinBindings::instanceClassInfoOf(ancestor);
        if (ancestorInfo && !info->isSubClassOf(ancestorInfo))
            comparison.report(classID, "has a C++ class that does not inherit from the one of "_s, table.tokenOf(ancestor), ", which the table says it extends"_s);
    }
}

static bool hasInstanceTest(const BuiltinTable& table, BuiltinClassID classID)
{
    for (BuiltinClassID tested = classID; tested; tested = table.at(tested).parent) {
        if (table.at(tested).instanceTest != BuiltinInstanceTest::None)
            return true;
    }
    return false;
}

static bool passesInstanceTest(const BuiltinTable& table, BuiltinClassID classID, JSObject* instance)
{
    auto& builtinClass = table.at(classID);
    switch (builtinClass.instanceTest) {
    case BuiltinInstanceTest::None:
        return true;
    case BuiltinInstanceTest::JSTypeEquals:
        return instance->type() == builtinClass.type;
    case BuiltinInstanceTest::ClassInfoInherits: {
        const ClassInfo* info = BuiltinBindings::instanceClassInfoOf(classID);
        return !info || instance->inherits(info);
    }
    case BuiltinInstanceTest::SharingModeEquals: {
        auto* buffer = dynamicDowncast<JSArrayBuffer>(instance);
        return buffer && buffer->sharingMode() == builtinClass.sharingMode;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canConstructWithoutArguments(const BuiltinTable& table, BuiltinID constructor)
{
    if (table.at(constructor).flags.containsAny({ BuiltinFlag::StartsInstance, BuiltinFlag::RunsUnseenCode }))
        return false;
    return std::ranges::any_of(table.overloadsOf(constructor), [](const BuiltinOverload& overload) {
        return overload.mode == BuiltinCallMode::Construct && !overload.isUndeclared() && std::ranges::none_of(overload.parameters, [](const BuiltinParameter& parameter) {
            return parameter.mode == BuiltinParameterMode::Required;
        });
    });
}

static JSObject* constructWithoutArguments(JSGlobalObject* realm, JSObject* constructor)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(realm->vm());
    JSObject* instance = construct(realm, constructor, ArgList(), "The constructor of a built-in class cannot construct"_s);
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return nullptr;
    }
    return instance;
}

static void checkNewInstance(RealmComparison& comparison, BuiltinClassID classID)
{
    auto& table = comparison.table;
    BuiltinID constructor = table.at(classID).constructor;
    JSObject* constructorObject = constructor ? comparison.objectOf(constructor) : nullptr;
    if (!constructorObject || !hasInstanceTest(table, classID) || !canConstructWithoutArguments(table, constructor))
        return;
    JSObject* instance = constructWithoutArguments(comparison.realm, constructorObject);
    if (!instance)
        return;
    for (BuiltinClassID tested = classID; tested; tested = table.at(tested).parent) {
        if (!passesInstanceTest(table, tested, instance))
            comparison.report(classID, "has a new instance that fails the instance test of "_s, table.tokenOf(tested));
    }
}

static void checkClasses(RealmComparison& comparison)
{
    for (unsigned index = 0; index < comparison.table.classCount(); ++index) {
        checkClassInfo(comparison, BuiltinClassID(index));
        checkNewInstance(comparison, BuiltinClassID(index));
    }
}

Vector<String> BuiltinTable::mismatches(JSGlobalObject* realm) const
{
    DeferTerminationForAWhile deferTermination(realm->vm());
    RealmComparison comparison(*this, realm);
    checkEntries(comparison);
    checkNativeFunctions(comparison);
    checkPrivateAliases(comparison);
    checkInheritance(comparison);
    checkObjectIdentities(comparison);
    checkClosedHolders(comparison);
    checkThatNothingSettlesLater(comparison);
    checkClasses(comparison);
    return WTF::move(comparison.mismatches);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
