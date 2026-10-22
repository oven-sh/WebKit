/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRealmStartup.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinFamilyState.h"
#include "AOTBuiltinLocationWatch.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTErrorMessages.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRemovableFeature.h"
#include "AOTRuntimeHooks.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "DeferTermination.h"
#include "GlobalObjectMethodTable.h"
#include "ImmutablePropertyNames.h"
#include "JSCInlines.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <mutex>
#include <span>
#include <utility>
#include <wtf/DataLog.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

bool runsCompiledProgram(JSGlobalObject* realm)
{
    return realm->vm().hasCompiledCode() && realm->globalObjectMethodTable()->moduleHost;
}

static constinit bool locksBuiltinsWithoutImage = false;

bool locksBuiltins(JSGlobalObject* realm)
{
    return (realm->vm().hasCompiledCode() || locksBuiltinsWithoutImage) && realm->globalObjectMethodTable()->moduleHost;
}

void lockBuiltinsWithoutImage()
{
    locksBuiltinsWithoutImage = true;
}

static constinit RefusedChangeListener refusedChangeListener = nullptr;
static constinit Lock namesOfLockedObjectsLock;

// Only compared with, never read through: a realm may be gone.
static UncheckedKeyHashMap<JSObject*, BuiltinID>& namesOfLockedObjects() WTF_REQUIRES_LOCK(namesOfLockedObjectsLock)
{
    static NeverDestroyed<UncheckedKeyHashMap<JSObject*, BuiltinID>> names;
    return names;
}

void setRefusedChangeListener(RefusedChangeListener listener)
{
    refusedChangeListener = listener;
}

static void noteNameOfLockedObject(JSObject* object, BuiltinID id)
{
    Locker locker { namesOfLockedObjectsLock };
    namesOfLockedObjects().set(object, id);
}

void didRefuseChange(JSGlobalObject* realm, JSObject* object, UniquedStringImpl* keyOrNull, RefusedChange change)
{
    if (!refusedChangeListener) [[likely]]
        return;
    BuiltinID id;
    {
        Locker locker { namesOfLockedObjectsLock };
        id = namesOfLockedObjects().get(object);
    }
    if (!id)
        return;
    String holder = BuiltinTable::singleton().textOf(id);
    auto member = [&]() -> String {
        if (keyOrNull->isSymbol())
            return makeString(holder, "[Symbol("_s, StringView(keyOrNull), ")]"_s);
        return id == Builtins::GlobalThis ? String(keyOrNull) : makeString(holder, '.', StringView(keyOrNull));
    };
    switch (change) {
    case RefusedChange::Store:
        return refusedChangeListener(realm, makeString("Cannot assign to "_s, member()));
    case RefusedChange::Definition:
        return refusedChangeListener(realm, makeString("Cannot define "_s, member()));
    case RefusedChange::Deletion:
        return refusedChangeListener(realm, makeString("Cannot delete "_s, member()));
    case RefusedChange::Prototype:
        return refusedChangeListener(realm, makeString("Cannot change the prototype of "_s, holder));
    }
}

String messageOfRefusalByLockedObject(JSGlobalObject*, JSObject* object, UniquedStringImpl* key, RefusedChange change)
{
    JSGlobalObject* realm = object->realm();
    Instance* instance = realm ? Instance::of(realm) : nullptr;
    State* state = instance ? instance->state : nullptr;
    BuiltinID id = state && state->builtinCells ? state->builtinCells->builtinOfLockedObject(object) : BuiltinID();
    if (!id) {
        Locker locker { namesOfLockedObjectsLock };
        id = namesOfLockedObjects().get(object);
    }
    if (!id)
        return { };
    String member = memberOfLockedBuiltin(id, key);
    return change == RefusedChange::Deletion ? deletionFromLockedBuiltinMessage(member) : assignmentToLockedBuiltinMessage(member);
}

// Every realm of the VM, with an instance or without: text that is evaluated in any of them reaches the objects of the program.
void refuseEvaluationUnlessAdmitted(JSGlobalObject* realm)
{
    if (!realm->vm().aotState().image->admitsUnknownJavaScript())
        realm->setEvalEnabled(false, evaluationRefusedMessage());
}

[[noreturn]] static NEVER_INLINE void crashOnBuiltinHolder(BuiltinID holder, ASCIILiteral problem)
{
    dataLogLn("AOT: built-in holder ", BuiltinTable::singleton().textOf(holder), " ", problem);
    CRASH();
}

static void lockObject(VM& vm, BuiltinID id, JSObject* object)
{
    if (!object->makePropertiesImmutable(vm)) [[unlikely]]
        crashOnBuiltinHolder(id, "cannot be made immutable"_s);
}

static JSObject* findOrCreateObject(JSGlobalObject* realm, BuiltinID id)
{
    if (id == Builtins::GlobalThis)
        return realm;
    auto found = findInRealm(realm, BuiltinCellKey::object(id));
    return found ? (*found)->getObject() : nullptr;
}

// Both lists of members have the members of one holder side by side, and the table numbers the members of a holder in one run. So the table is read once for each holder.
template<typename Member, typename Function>
static void forEachHolderOf(const BuiltinTable& table, std::span<const Member> members, const Function& function)
{
    uint32_t firstMember = 0;
    uint32_t memberCount = 0;
    for (Member member : members) {
        BuiltinID id(member);
        if (id.index() - firstMember < memberCount)
            continue;
        BuiltinID holder = table.at(id).holder;
        if (!holder)
            continue;
        const Builtin& object = table.at(holder);
        firstMember = object.firstMember.index();
        memberCount = object.memberCount;
        function(holder);
    }
}

static bool hasMemberLockedByTable(const BuiltinTable& table, BuiltinID holder)
{
    const Builtin& object = table.at(holder);
    auto found = std::ranges::lower_bound(table.lockedMembers(), object.firstMember);
    return object.memberCount && found != table.lockedMembers().end() && found->index() - object.firstMember.index() < object.memberCount;
}

static std::span<const uint32_t> constantMembersOf(const BuiltinTable& table, const ProgramTables& tables, BuiltinID holder)
{
    auto members = std::ranges::equal_range(tables.constantBuiltinMembers(), holder, { }, [&](uint32_t member) {
        return table.at(BuiltinID(member)).holder;
    });
    return { members.begin(), members.end() };
}

// A lookup by name may take a member for a constant although nothing in the program makes an object that inherits it. The objects of a feature that was removed
// are not in the executable, so there is nothing to lock and nothing that could be changed.
static bool isRemovedWithFeature(const BuiltinTable& table, BuiltinID holder)
{
    return std::ranges::any_of(table.featureUsesOf(holder), [](const BuiltinFeatureUse& use) {
        return !use.condition && isFeatureRemoved(use.feature);
    });
}

struct BuiltinsToLock {
    BitVector builtins;
    BitVector holdersThatTellOfWrites;
};

static BuiltinsToLock findBuiltinsToLock(const BuiltinTable& table, const ProgramTables& tables)
{
    BuiltinsToLock result { BitVector(table.builtinCount()), BitVector(table.builtinCount()) };
    BitVector& builtins = result.builtins;
    auto addWithPath = [&](BuiltinID id) {
        for (; id && !builtins.quickSet(id.index()); id = table.at(id).holder) { }
    };
    for (BuiltinID id : table.lockedObjects())
        addWithPath(id);
    forEachHolderOf(table, table.lockedMembers(), addWithPath);
    forEachHolderOf(table, tables.constantBuiltinMembers(), addWithPath);
    if (tables.eagerBuiltinCells().empty())
        return result;
    // The cells of these are found at their first use, in a holder that has told of every write since the realm made it.
    NamespacesOfBuiltinModules namespaces(table);
    for (uint32_t number : tables.eagerBuiltinCells()) {
        std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(tables.builtinCells()[number], table);
        RELEASE_ASSERT(key);
        BuiltinID holder = table.at(key->builtin()).holder;
        if (!holder || result.holdersThatTellOfWrites.quickGet(holder.index()) || namespaces.holdAbove(holder))
            continue;
        result.holdersThatTellOfWrites.quickSet(holder.index());
        addWithPath(holder);
    }
    return result;
}

bool reportsCreationOfMember(JSObject* holder, UniquedStringImpl* key)
{
    if (holder->staticPropertiesReified())
        return false;
    for (const ClassInfo* info = holder->classInfo(); info; info = info->parentClass) {
        const HashTable* hashTable = info->staticPropHashTable;
        const HashTableValue* entry = hashTable ? hashTable->entry(PropertyName(key)) : nullptr;
        if (entry)
            return entry->attributes() & PropertyAttribute::BuiltinOrFunctionOrLazyProperty && !(entry->attributes() & PropertyAttribute::Accessor);
    }
    return false;
}

static bool isLockedByTable(const BuiltinTable& table, BuiltinID holder)
{
    return hasMemberLockedByTable(table, holder) || table.at(holder).flags.contains(BuiltinFlag::IsLockedObject);
}

namespace {

// Locks a built-in object that the realm has created, and what hangs on it and is there already. Most programs never look at most built-ins, also at most of those that some
// function of the executable relies on: what the realm creates later is locked when its creation is reported, before any code of the program can reach it.
// Only a holder that the program relies on and whose creation nobody reports is created here.
class BuiltinLocker {
public:
    explicit BuiltinLocker(Instance& instance)
        : BuiltinLocker(instance, instance.state->builtinCells->builtinsToLock(), nullptr)
    {
    }

    BuiltinLocker(Instance& instance, const BitVector& builtinsToLock, BitVector* lockedWithoutCells)
        : m_vm(*instance.vm)
        , m_realm(instance.globalObject)
        , m_tables(instance.tables())
        , m_cells(instance.state ? instance.state->builtinCells.get() : nullptr)
        , m_builtinsToLock(builtinsToLock)
        , m_lockedWithoutCells(lockedWithoutCells)
    {
        ASSERT(m_cells || m_lockedWithoutCells);
    }

    bool hasLocked(BuiltinID id) const { return m_cells ? m_cells->hasLocked(id) : m_lockedWithoutCells->get(id.index()); }

    void lock(BuiltinID nameOfObject, JSObject* object)
    {
        const BuiltinTable& table = BuiltinTable::singleton();
        BuiltinID id = table.canonicalID(nameOfObject);
        if (!m_builtinsToLock.get(id.index()) || !noteLocked(id, object))
            return;
        makeImmutable(table, id, object);

        for (BuiltinID member : table.membersOf(id)) {
            const Builtin& entry = table.at(member);
            BuiltinID target = entry.sameObjectAs ? entry.sameObjectAs : member;
            if (!m_builtinsToLock.quickGet(target.index()) || hasLocked(target))
                continue;
            if (JSObject* existing = findOrCreateMember(table, object, entry.name, target))
                lock(target, existing);
        }
        if (BuiltinID inherited = table.at(id).inheritsFrom) {
            if (JSObject* prototype = object->getPrototypeDirect().getObject())
                lock(inherited, prototype);
        }
    }

private:
    // Two in three of the members that are locked with their holder are its prototype object or its constructor, and the VM has those two keys at hand.
    JSObject* findOrCreateMember(const BuiltinTable& table, JSObject* holder, BuiltinName name, BuiltinID member)
    {
        if (name == BuiltinNames::prototype)
            return findExistingObject(holder, m_vm.propertyNames->prototype.impl());
        if (name == BuiltinNames::constructor)
            return findExistingObject(holder, m_vm.propertyNames->constructor.impl());
        Identifier key = table.identifierOf(m_vm, name);
        if (JSObject* existing = findExistingObject(holder, key.impl()))
            return existing;
        if (isLockedByTable(table, member) || reportsCreationOfMember(holder, key.impl()))
            return nullptr;
        return findOrCreateObject(m_realm, member);
    }

    bool noteLocked(BuiltinID id, JSObject* object)
    {
        if (!m_cells)
            return !m_lockedWithoutCells->set(id.index());
        return m_cells->didLock(id, object);
    }

    void makeImmutable(const BuiltinTable& table, BuiltinID id, JSObject* object)
    {
        if (table.at(id).flags.contains(BuiltinFlag::IsLockedObject)) {
            lockObject(m_vm, id, object);
            if (m_cells)
                m_cells->didLockEveryPropertyOf(object);
            return;
        }
        std::span<const uint32_t> constantMembers = constantMembersOf(table, m_tables, id);
        bool hasFixedNames = hasMemberLockedByTable(table, id) || !constantMembers.empty();
        bool tellsOfWrites = m_cells && m_cells->holdersThatTellOfWrites().get(id.index());
        // A holder may only lie on the path to what is locked.
        if (!hasFixedNames && !tellsOfWrites)
            return;
        if (object->makePropertiesImmutable(m_vm, immutablePropertyNamesOf(m_vm, id, constantMembers, m_cells).get())) [[likely]]
            return;
        if (hasFixedNames)
            crashOnBuiltinHolder(id, "cannot be made immutable"_s);
        m_cells->cannotHearOfWritesTo(id);
    }

    VM& m_vm;
    JSGlobalObject* m_realm;
    const ProgramTables& m_tables;
    BuiltinCells* m_cells;
    const BitVector& m_builtinsToLock;
    BitVector* m_lockedWithoutCells;
};

} // anonymous namespace

static void lockPrototypeOfClass(BuiltinLocker& locker, BuiltinClassID builtinClass, JSObject* prototype)
{
    BuiltinID id = BuiltinTable::singleton().at(builtinClass).prototype;
    if (id && prototype)
        locker.lock(id, prototype);
}

static void lockExistingBuiltins(BuiltinLocker& locker, JSGlobalObject* realm)
{
    locker.lock(Builtins::GlobalThis, realm);
    // No name need lead from the global object to a prototype whose instances exist: the name of the class is created at its first read.
    forEachHiddenPrototype(realm, [&](BuiltinClassID builtinClass, JSObject* prototype) {
        lockPrototypeOfClass(locker, builtinClass, prototype);
    });
    forEachInstanceStructure(realm, [&](BuiltinClassID builtinClass, Structure* structure) {
        lockPrototypeOfClass(locker, builtinClass, structure->storedPrototypeObject());
    });
    forEachCreatedClassOfGlobal(realm, [&](BuiltinID member, JSObject* constructor) {
        locker.lock(member, constructor);
    });
}

// Whoever makes the Structure of the instances of a class that names it reports it: the engine a LazyClassStructure, the host any other. The prototype object and the constructor are there by then.
static bool isCreatedWithReportedStructure(const BuiltinTable& table, BuiltinID holder)
{
    BuiltinClassID builtinClass = table.at(holder).instanceClass;
    if (!builtinClass || (table.at(builtinClass).prototype != holder && table.at(builtinClass).constructor != holder))
        return false;
    return std::ranges::any_of(BuiltinBindings::classesOfHost(), [&](const BuiltinClassBinding& binding) {
        return binding.builtinClass == builtinClass && binding.findInstanceStructure;
    });
}

// The creation of an object that the table locks is reported. What names lead to from the global object is locked with its holder or when its creation is reported.
// No name leads to the others that this program relies on: those whose creation nobody reports are created now.
static void lockHoldersOfProgram(BuiltinLocker& locker, JSGlobalObject* realm, const ProgramTables& tables, const BitVector& holdersThatTellOfWrites)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    auto lockCreated = [&](BuiltinID holder) {
        if (locker.hasLocked(holder) || isLockedByTable(table, holder) || table.isUnder(holder, Builtins::GlobalThis) || isCreatedWithReportedStructure(table, holder))
            return;
        if (JSObject* object = findOrCreateObject(realm, holder)) {
            locker.lock(holder, object);
            return;
        }
        if (!constantMembersOf(table, tables, holder).empty() && BuiltinBindings::provides(table.at(holder).origin) && !isRemovedWithFeature(table, holder))
            crashOnBuiltinHolder(holder, "is not in this realm"_s);
    };
    forEachHolderOf(table, tables.constantBuiltinMembers(), lockCreated);
    for (size_t holder : holdersThatTellOfWrites)
        lockCreated(BuiltinID(holder));
}

static void lockBuiltins(Instance& instance)
{
    JSGlobalObject* realm = instance.globalObject;
    BuiltinsToLock toLock = findBuiltinsToLock(BuiltinTable::singleton(), instance.tables());
    if (!instance.state) {
        BitVector locked;
        BuiltinLocker locker(instance, toLock.builtins, &locked);
        lockExistingBuiltins(locker, realm);
        lockHoldersOfProgram(locker, realm, instance.tables(), toLock.holdersThatTellOfWrites);
        return;
    }
    BuiltinCells& cells = *instance.state->builtinCells;
    cells.setBuiltinsToLock(WTF::move(toLock.builtins), WTF::move(toLock.holdersThatTellOfWrites));
    BuiltinLocker locker(instance);
    lockExistingBuiltins(locker, realm);
    lockHoldersOfProgram(locker, realm, instance.tables(), cells.holdersThatTellOfWrites());
}

void lockBuiltinsOfTable(JSGlobalObject* realm)
{
    VM& vm = realm->vm();
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    const BuiltinTable& table = BuiltinTable::singleton();
    for (BuiltinID id : table.lockedObjects()) {
        if (JSObject* object = findOrCreateObject(realm, id)) {
            lockObject(vm, id, object);
            noteNameOfLockedObject(object, id);
        }
    }
    forEachHolderOf(table, table.lockedMembers(), [&](BuiltinID holder) {
        JSObject* object = findOrCreateObject(realm, holder);
        if (!object)
            return;
        if (!object->makePropertiesImmutable(vm, immutablePropertyNamesOf(vm, holder).get())) [[unlikely]]
            crashOnBuiltinHolder(holder, "cannot be made immutable"_s);
        noteNameOfLockedObject(object, holder);
    });
    scope.assertNoException();
}

static Instance* findInstanceThatLocks(JSGlobalObject* realm)
{
    Instance* instance = Instance::of(realm);
    if (!instance || !instance->state || instance->state->builtinCells->builtinsToLock().isEmpty())
        return nullptr;
    return instance;
}

static void validateClassWithInstanceStructure(JSGlobalObject* realm, Structure* structure, BuiltinClassID answer)
{
    BuiltinClassID found;
    forEachInstanceStructure(realm, [&](BuiltinClassID builtinClass, Structure* original) {
        if (original == structure)
            found = builtinClass;
    });
    RELEASE_ASSERT(found == answer);
}

// Without the mark every test for a pristine instance of the class fails, and nothing but the speed of the program shows it.
static void markPristine(VM& vm, BuiltinClassID builtinClass, Structure* structure)
{
    if (structure->tryMarkPristine(vm) || !validatesRuntime(vm)) [[likely]]
        return;
    const BuiltinTable& table = BuiltinTable::singleton();
    if (!table.mayHavePristineInstances(builtinClass))
        return;
    dataLogLn("AOT: the Structure of the instances that inherit from ", table.textOf(table.at(builtinClass).prototype), " cannot be marked as pristine");
    crashOnValidationFailure(vm, { ValidationFailureKind::ArrayNotPristine, { }, { }, structure });
}

static BuiltinID findPrototypeMember(const BuiltinTable& table, BuiltinID constructor)
{
    for (BuiltinID member : table.membersOf(constructor)) {
        if (table.at(member).kind == BuiltinKind::Prototype && !table.at(member).sameObjectAs)
            return member;
    }
    return { };
}

void didCreateClass(JSGlobalObject* realm, const LazyClassStructure& lazyStructure, Structure* structure, JSObject* prototype, JSObject* constructor)
{
    Instance* instance = findInstanceThatLocks(realm);
    if (!instance)
        return;

    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinClassID builtinClass = findClassWithInstanceStructure(realm, structure);
    if (validatesRuntime(realm->vm())) [[unlikely]]
        validateClassWithInstanceStructure(realm, structure, builtinClass);
    if (builtinClass)
        instance->didCreateInstanceStructure(builtinClass, structure);
    BuiltinID constructorID = findGlobalCreatedWith(realm, lazyStructure);
    BuiltinID prototypeID;
    if (constructorID) {
        constructorID = table.canonicalID(constructorID);
        prototypeID = findPrototypeMember(table, constructorID);
    } else if (builtinClass) {
        constructorID = table.at(builtinClass).constructor;
        prototypeID = table.at(builtinClass).prototype;
    }
    // The realm completes the prototype while it creates the constructor.
    if (constructorID && !constructor)
        return;

    BuiltinLocker locker(*instance);
    if (prototypeID)
        locker.lock(prototypeID, prototype);
    if (constructorID)
        locker.lock(constructorID, constructor);
    if (builtinClass)
        markPristine(realm->vm(), builtinClass, structure);
}

void didCreateStaticProperty(JSObject* holder, UniquedStringImpl* key, JSValue value)
{
    if (!value || !value.isObject())
        return;
    JSGlobalObject* realm = holder->realm();
    if (Instance* created = Instance::of(realm); created && created->state) {
        created->state->builtinCells->didCreateMember(holder, key, asObject(value));
        created->state->builtinFamilies->locationWatch->didCreateMember(holder, key, asObject(value));
    }
    Instance* instance = findInstanceThatLocks(realm);
    if (!instance)
        return;

    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinID holderID = holder == realm ? Builtins::GlobalThis : instance->state->builtinCells->builtinOfLockedObject(holder);
    BuiltinName name = holderID ? table.nameOf(realm->vm(), key) : BuiltinName();
    BuiltinID member = name ? table.findOwnMember(holderID, name) : BuiltinID();
    if (!member)
        return;
    instance->state->builtinCells->didMakeMember(member, asObject(value));
    BuiltinLocker locker(*instance);
    locker.lock(member, asObject(value));
}

void hostDidCreateStructure(JSGlobalObject* realm, Structure* structure)
{
    Instance* instance = findInstanceThatLocks(realm);
    if (!instance)
        return;
    BuiltinClassID builtinClass = findClassWithInstanceStructure(realm, structure);
    if (validatesRuntime(realm->vm())) [[unlikely]]
        validateClassWithInstanceStructure(realm, structure, builtinClass);
    if (!builtinClass)
        return;
    BuiltinLocker locker(*instance);
    lockPrototypeOfClass(locker, builtinClass, structure->storedPrototypeObject());
    markPristine(realm->vm(), builtinClass, structure);
}

void hostDidCreateNodeModule(JSGlobalObject* realm, JSObject* constructor)
{
    if (Instance* instance = findInstanceThatLocks(realm))
        BuiltinLocker(*instance).lock(Builtins::NodeModule, constructor);
}

void markOriginalStructuresPristine(VM& vm, JSGlobalObject* realm)
{
    forEachInstanceStructure(realm, [&](BuiltinClassID builtinClass, Structure* structure) {
        markPristine(vm, builtinClass, structure);
    });
}

void makeBuiltinsImmutable(Instance& instance)
{
    ASSERT(!instance.state);
    // No code of the program runs before the host has installed its globals, and didInstallBuiltins() locks then.
    if (BuiltinBindings::hasHost())
        return;
    VM& vm = *instance.vm;
    JSGlobalObject* realm = instance.globalObject;
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    lockBuiltins(instance);
    markOriginalStructuresPristine(vm, realm);
    scope.assertNoException();
}

void didInstallBuiltins(Instance& instance)
{
    ASSERT(instance.state);
    VM& vm = *instance.vm;
    DeferGC deferGC(vm);
    DeferTerminationForAWhile deferTermination(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    lockBuiltins(instance);
    instance.state->builtinCells->fillCellsUnderModules();
    markOriginalStructuresPristine(vm, instance.globalObject);
    // The prototypes of a hosted realm are locked only now, so the marks before the instance was created may have failed.
    instance.captureCellHeaders();
    if (validatesRuntime(vm)) [[unlikely]]
        instance.state->builtinCells->fillCellsOfEngine();
    scope.assertNoException();
}

void hostDidCreateRealm(JSGlobalObject* realm)
{
    if (Instance* instance = Instance::of(realm))
        didInstallBuiltins(*instance);
    else if (locksBuiltins(realm))
        lockBuiltinsOfTable(realm);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
