/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRealmStartup.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRemovableFeature.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "DeferTermination.h"
#include "GlobalObjectMethodTable.h"
#include "ImmutablePropertyNames.h"
#include "JSCInlines.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <span>
#include <utility>
#include <wtf/DataLog.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

bool runsCompiledProgram(JSGlobalObject* realm)
{
    return realm->vm().hasCompiledCode() && realm->globalObjectMethodTable()->moduleHost;
}

ASCIILiteral evaluationRefusedMessage()
{
    return "Code generation from strings disallowed for this context: this executable was compiled without support for evaluating text at run time. Compile with --admit=runtime-code"_s;
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

// Both lists of members have the members of one holder side by side.
template<typename Member, typename Function>
static void forEachHolderOf(const BuiltinTable& table, std::span<const Member> members, const Function& function)
{
    BuiltinID previous;
    for (Member member : members) {
        BuiltinID holder = table.at(BuiltinID(member)).holder;
        if (holder != previous)
            function(holder);
        previous = holder;
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

namespace {

// Nothing is interned and nothing is read at start-up: a name is looked up in the image's table of identifiers, by its hash, when somebody is about to define it.
class ClosedAbsentNames final : public ImmutablePropertyNames::OutsideNames {
public:
    static RefPtr<const ImmutablePropertyNames::OutsideNames> find(VM& vm, const ProgramTables& tables, BuiltinID holder)
    {
        auto holders = tables.holdersOfClosedAbsentNames();
        auto found = std::ranges::lower_bound(holders, holder.index(), { }, &ClosedAbsentNamesOfHolder::holder);
        if (found == holders.end() || found->holder != holder.index() || !vm.aotState().identifiers)
            return nullptr;
        return adoptRef(*new ClosedAbsentNames(*vm.aotState().identifiers, tables.closedAbsentNames().subspan(found->firstName, found->numberOfNames)));
    }

    bool contains(UniquedStringImpl* uid) const final
    {
        std::optional<uint32_t> number = m_identifiers.numberOf(uid);
        return number && std::ranges::binary_search(m_numbers, *number);
    }

private:
    ClosedAbsentNames(const ProgramIdentifiers& identifiers, std::span<const uint32_t> numbers)
        : m_identifiers(identifiers)
        , m_numbers(numbers)
    {
    }

    const ProgramIdentifiers& m_identifiers;
    std::span<const uint32_t> m_numbers;
};

} // anonymous namespace

static BitVector findBuiltinsToLock(const BuiltinTable& table, const ProgramTables& tables)
{
    BitVector builtins(table.builtinCount());
    auto addWithPath = [&](BuiltinID id) {
        for (; id && !builtins.quickSet(id.index()); id = table.at(id).holder) { }
    };
    for (BuiltinID id : table.lockedObjects())
        addWithPath(id);
    forEachHolderOf(table, table.lockedMembers(), addWithPath);
    forEachHolderOf(table, tables.constantBuiltinMembers(), addWithPath);
    for (const ClosedAbsentNamesOfHolder& listed : tables.holdersOfClosedAbsentNames())
        addWithPath(BuiltinID(listed.holder));
    return builtins;
}

namespace {

// Locks a built-in object that the realm has created, and what hangs on it and is there already. It creates nothing: most programs never look at most built-ins.
// What the realm creates later is locked when its creation is reported, before any code of the program can reach it.
class BuiltinLocker {
public:
    explicit BuiltinLocker(Instance& instance)
        : BuiltinLocker(instance, instance.state->builtinCells->builtinsToLock(), nullptr)
    {
    }

    BuiltinLocker(Instance& instance, const BitVector& builtinsToLock, BitVector* lockedWithoutCells)
        : m_vm(*instance.vm)
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
        if (!m_builtinsToLock.get(id.index()) || !noteLocked(table, id, object))
            return;
        makeImmutable(table, id, object);

        for (BuiltinID member : table.membersOf(id)) {
            BuiltinID target = table.canonicalID(member);
            if (!m_builtinsToLock.get(target.index()) || hasLocked(target))
                continue;
            if (JSObject* existing = findExistingObject(object, table.identifierOf(m_vm, table.at(member).name).impl()))
                lock(target, existing);
        }
        if (BuiltinID inherited = table.at(id).inheritsFrom) {
            if (JSObject* prototype = object->getPrototypeDirect().getObject())
                lock(inherited, prototype);
        }
    }

private:
    bool noteLocked(const BuiltinTable& table, BuiltinID id, JSObject* object)
    {
        if (!m_cells)
            return !m_lockedWithoutCells->set(id.index());
        bool isLockedObject = table.at(id).flags.contains(BuiltinFlag::IsLockedObject);
        return m_cells->didLock(id, object, isLockedObject ? BuiltinCells::LockedProperties::All : BuiltinCells::LockedProperties::Listed);
    }

    void makeImmutable(const BuiltinTable& table, BuiltinID id, JSObject* object)
    {
        if (table.at(id).flags.contains(BuiltinFlag::IsLockedObject)) {
            lockObject(m_vm, id, object);
            return;
        }
        std::span<const uint32_t> constantMembers = constantMembersOf(table, m_tables, id);
        RefPtr<const ImmutablePropertyNames::OutsideNames> closedAbsentNames = ClosedAbsentNames::find(m_vm, m_tables, id);
        // A holder may only lie on the path to what is locked.
        if (!hasMemberLockedByTable(table, id) && constantMembers.empty() && !closedAbsentNames)
            return;
        if (!object->makePropertiesImmutable(m_vm, immutablePropertyNamesOf(m_vm, id, constantMembers, WTF::move(closedAbsentNames)).get())) [[unlikely]]
            crashOnBuiltinHolder(id, "cannot be made immutable"_s);
    }

    VM& m_vm;
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

// The creation of an object that the table locks is reported. Of the others only those count that this program relies on, and it most likely uses them: they are created now.
static void lockHoldersOfProgram(BuiltinLocker& locker, JSGlobalObject* realm, const ProgramTables& tables)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    auto lockCreated = [&](BuiltinID holder) {
        if (locker.hasLocked(holder) || hasMemberLockedByTable(table, holder) || table.at(holder).flags.contains(BuiltinFlag::IsLockedObject))
            return;
        if (JSObject* object = findOrCreateObject(realm, holder)) {
            locker.lock(holder, object);
            return;
        }
        if (!constantMembersOf(table, tables, holder).empty() && BuiltinBindings::provides(table.at(holder).origin) && !isRemovedWithFeature(table, holder))
            crashOnBuiltinHolder(holder, "is not in this realm"_s);
    };
    forEachHolderOf(table, tables.constantBuiltinMembers(), lockCreated);
    for (const ClosedAbsentNamesOfHolder& listed : tables.holdersOfClosedAbsentNames())
        lockCreated(BuiltinID(listed.holder));
}

static void lockBuiltins(Instance& instance)
{
    JSGlobalObject* realm = instance.globalObject;
    BitVector builtinsToLock = findBuiltinsToLock(BuiltinTable::singleton(), instance.tables());
    if (!instance.state) {
        BitVector locked;
        BuiltinLocker locker(instance, builtinsToLock, &locked);
        lockExistingBuiltins(locker, realm);
        lockHoldersOfProgram(locker, realm, instance.tables());
        return;
    }
    instance.state->builtinCells->setBuiltinsToLock(WTF::move(builtinsToLock));
    BuiltinLocker locker(instance);
    lockExistingBuiltins(locker, realm);
    lockHoldersOfProgram(locker, realm, instance.tables());
}

static Instance* findInstanceThatLocks(JSGlobalObject* realm)
{
    Instance* instance = Instance::of(realm);
    if (!instance || !instance->state || instance->state->builtinCells->builtinsToLock().isEmpty())
        return nullptr;
    return instance;
}

static BuiltinClassID findClassWithInstanceStructure(JSGlobalObject* realm, Structure* structure)
{
    BuiltinClassID found;
    forEachInstanceStructure(realm, [&](BuiltinClassID builtinClass, Structure* original) {
        if (original == structure)
            found = builtinClass;
    });
    return found;
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
        structure->tryMarkPristine(realm->vm());
}

void didCreateStaticProperty(JSObject* holder, UniquedStringImpl* key, JSValue value)
{
    if (!value || !value.isObject())
        return;
    JSGlobalObject* realm = holder->realm();
    Instance* instance = findInstanceThatLocks(realm);
    if (!instance)
        return;

    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinID holderID = holder == realm ? Builtins::GlobalThis : instance->state->builtinCells->builtinOfLockedObject(holder);
    BuiltinName name = holderID ? table.nameOf(realm->vm(), key) : BuiltinName();
    BuiltinID member = name ? table.findOwnMember(holderID, name) : BuiltinID();
    if (!member)
        return;
    BuiltinLocker locker(*instance);
    locker.lock(member, asObject(value));
}

void hostDidCreateStructure(JSGlobalObject* realm, Structure* structure)
{
    Instance* instance = findInstanceThatLocks(realm);
    BuiltinClassID builtinClass = instance ? findClassWithInstanceStructure(realm, structure) : BuiltinClassID();
    if (!builtinClass)
        return;
    BuiltinLocker locker(*instance);
    lockPrototypeOfClass(locker, builtinClass, structure->storedPrototypeObject());
    structure->tryMarkPristine(realm->vm());
}

void markOriginalStructuresPristine(VM& vm, JSGlobalObject* realm)
{
    forEachInstanceStructure(realm, [&](BuiltinClassID, Structure* structure) {
        structure->tryMarkPristine(vm);
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
    instance.state->builtinCells->fillEagerCells();
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
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
