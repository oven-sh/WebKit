/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCells.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTImmortalCells.h"
#include "AOTModuleHost.h"
#include "AOTModules.h"
#include "AOTState.h"
#include "DeferTermination.h"
#include "GlobalObjectMethodTable.h"
#include "JSCInlines.h"
#include "Options.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <bit>
#include <utility>
#include <wtf/DataLog.h>
#include <wtf/StackPointer.h>

namespace JSC { namespace AOT {

namespace {

class StackKeptFromProgramScope {
    WTF_MAKE_NONCOPYABLE(StackKeptFromProgramScope);
public:
    explicit StackKeptFromProgramScope(VM& vm)
        : m_vm(vm)
        , m_savedSoftReservedZoneSize(vm.softReservedZoneSize())
    {
        if (m_savedSoftReservedZoneSize > Options::softReservedZoneSize())
            m_vm.updateSoftReservedZoneSize(Options::softReservedZoneSize());
    }

    ~StackKeptFromProgramScope()
    {
        m_vm.updateSoftReservedZoneSize(m_savedSoftReservedZoneSize);
    }

private:
    VM& m_vm;
    size_t m_savedSoftReservedZoneSize;
};

} // anonymous namespace

static bool isNearEndOfStack(VM& vm)
{
    return std::bit_cast<uintptr_t>(currentStackPointer()) < std::bit_cast<uintptr_t>(vm.softStackLimit()) + BuiltinCells::stackKeptFromProgram;
}

BuiltinCells::BuiltinCells(State& state)
    : m_state(state)
    , m_instance(state.instance)
    , m_tables(state.tables)
    , m_cells(state.tableArena.allocate<JSCell*>(state.tables.builtinCells().size()))
{
    m_instance.prefix().builtinCells = m_cells;
}

static bool isImmutableLocation(const BuiltinTable& table, std::span<const uint32_t> constantMembers, BuiltinID builtin)
{
    if (table.at(builtin).mutability == BuiltinMutability::AlwaysLocked)
        return true;
    auto holderAndID = [&](BuiltinID id) {
        return std::pair { table.at(id).holder, id };
    };
    return std::ranges::binary_search(constantMembers, holderAndID(builtin), { }, [&](uint32_t index) {
        return holderAndID(BuiltinID(index));
    });
}

static BuiltinID rootOf(const BuiltinTable& table, BuiltinID id)
{
    while (BuiltinID holder = table.at(id).holder)
        id = holder;
    return id;
}

static RuntimeModuleID findModuleWithNamespace(const BuiltinTable& table, BuiltinID root)
{
    for (unsigned index = 0; index < table.moduleCount(); ++index) {
        if (table.namespaceOf(RuntimeModuleID(index)) == root)
            return RuntimeModuleID(index);
    }
    return { };
}

static JSObject* requireModule(Modules& modules, const BuiltinTable& table, RuntimeModuleID module)
{
    JSGlobalObject* realm = modules.instance().globalObject;
    VM& vm = realm->vm();
    const GlobalObjectMethodTable* methodTable = realm->globalObjectMethodTable();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    RequestToHostScope requestToHost(modules);
    JSValue exports;
    if (table.at(module).flags.contains(RuntimeModuleFlag::HasSource)) {
        if (auto* requireBuiltinModule = methodTable->requireBuiltinModule)
            exports = requireBuiltinModule(realm, module.index());
    } else if (auto* requireNativeModule = methodTable->moduleHost ? methodTable->moduleHost->requireNativeModule : nullptr)
        exports = requireNativeModule(realm, jsString(vm, table.textOf(table.namespaceOf(module))));
    if (scope.exception()) [[unlikely]] {
        scope.clearException();
        return nullptr;
    }
    return exports ? exports.getObject() : nullptr;
}

std::optional<uint32_t> BuiltinCells::numberOf(BuiltinCellKey key) const
{
    std::span<const uint32_t> keys = m_tables.builtinCells();
    auto position = std::ranges::lower_bound(keys, key.encoded());
    if (position == keys.end() || *position != key.encoded())
        return std::nullopt;
    return static_cast<uint32_t>(position - keys.begin());
}

JSObject* BuiltinCells::findKnownObject(BuiltinID id) const
{
    if (JSObject* object = m_lockedObjects.get(id.index()))
        return object;
    std::optional<uint32_t> number = numberOf(BuiltinCellKey::object(id));
    JSCell* filled = number ? m_cells[*number] : nullptr;
    return filled ? filled->getObject() : nullptr;
}

// The program may have replaced a step of the path, so the lookup starts at the nearest holder that was found before the program ran.
std::expected<JSCell*, BuiltinLookupFailure> BuiltinCells::find(BuiltinCellKey key) const
{
    JSGlobalObject* realm = m_instance.globalObject;
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinID id = key.builtin();
    if (id == Builtins::GlobalThis)
        return findInRealm(realm, key);
    if (key.kind() == BuiltinCellKey::Kind::Object) {
        if (JSObject* object = findKnownObject(id))
            return static_cast<JSCell*>(object);
    }
    BuiltinID holder = table.at(id).holder;
    if (!holder || holder == Builtins::GlobalThis)
        return findInRealm(realm, key);
    if (JSObject* object = findKnownObject(holder))
        return findInHolder(realm, key, object);
    if (auto holderCell = find(BuiltinCellKey::object(holder)))
        return findInHolder(realm, key, asObject(*holderCell));
    // No path from the global object leads into a built-in module.
    if (findModuleWithNamespace(table, rootOf(table, holder)))
        return makeUnexpected(BuiltinLookupFailure::HolderIsMissing);
    return findInRealm(realm, key);
}

// Start-up loads no built-in module: the first use of one of its objects in compiled code does.
void BuiltinCells::loadModuleOf(BuiltinID id)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinID root = rootOf(table, id);
    if (root == Builtins::GlobalThis || m_lockedObjects.contains(root.index()))
        return;
    RuntimeModuleID module = findModuleWithNamespace(table, root);
    if (!module)
        return;
    if (JSObject* exports = requireModule(*m_state.modules, table, module))
        didLock(root, exports, LockedProperties::Listed);
}

bool BuiltinCells::didLock(BuiltinID id, JSObject* object, LockedProperties lockedProperties)
{
    if (!m_lockedObjects.add(id.index(), object).isNewEntry)
        return false;
    m_builtinsOfLockedObjects.add(object, id.index());
    m_state.immortalCells->add(object);
    if (lockedProperties == LockedProperties::All)
        m_objectsWithEveryPropertyLocked.append(object);
    return true;
}

BuiltinID BuiltinCells::builtinOfLockedObject(JSObject* object) const
{
    auto found = m_builtinsOfLockedObjects.find(object);
    return found == m_builtinsOfLockedObjects.end() ? BuiltinID() : BuiltinID(found->value);
}

JSCell* BuiltinCells::cell(uint32_t number)
{
    std::span<const uint32_t> keys = m_tables.builtinCells();
    RELEASE_ASSERT(number < keys.size());
    if (JSCell* filled = m_cells[number])
        return filled;
    if (m_isUnavailable.get(number))
        return nullptr;

    const BuiltinTable& table = BuiltinTable::singleton();
    std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(keys[number], table);
    RELEASE_ASSERT(key);
    DeferTerminationForAWhile deferTermination(*m_instance.vm);
    std::optional<StackKeptFromProgramScope> stackKeptFromProgramScope;
    if (isNearEndOfStack(*m_instance.vm)) [[unlikely]]
        stackKeptFromProgramScope.emplace(*m_instance.vm);
    // What a host runs to create one of its objects is no code of the program: the operation that asks for the cell has not entered the program.
    bool didEnterVM = m_instance.vm->didEnterVM;
    loadModuleOf(key->builtin());
    auto found = find(*key);
    m_instance.vm->didEnterVM = didEnterVM;
    if (JSCell* filledDuringLookup = m_cells[number])
        return filledDuringLookup;

    bool isImmutable = isImmutableLocation(table, m_tables.constantBuiltinMembers(), key->builtin());
    if (!found) {
        if (isImmutable && BuiltinBindings::provides(table.at(key->builtin()).origin)) {
            dataLogLn("AOT: built-in cell ", number, " (key ", key->encoded(), ", ", table.textOf(key->builtin()), ") is not in this realm: ", static_cast<unsigned>(found.error()));
            CRASH();
        }
        m_isUnavailable.ensureSizeAndSet(number, keys.size());
        return nullptr;
    }

    // An immutable property keeps its value only for as long as its holder lives, and the program may drop the last path to the holder.
    fillAndKeepAlive(number, *found);
    return *found;
}

JSCell* BuiltinCells::findObject(BuiltinID id)
{
    if (std::optional<uint32_t> number = numberOf(BuiltinCellKey::object(id)))
        return cell(*number);
    return find(BuiltinCellKey::object(id)).value_or(nullptr);
}

JSCell* BuiltinCells::readMember(JSGlobalObject* realm, JSObject* holder, BuiltinID member)
{
    return findInHolder(realm, BuiltinCellKey::object(member), holder).value_or(nullptr);
}

void BuiltinCells::fillEagerCells()
{
    for (uint32_t number : m_tables.eagerBuiltinCells())
        cell(number);
}

// A host creates some of its objects at their first use, by code the program can observe: it loads a module, or sets up the queue behind process.nextTick.
// Such a cell waits for its first use.
void BuiltinCells::fillCellsOfEngine()
{
    const BuiltinTable& table = BuiltinTable::singleton();
    std::span<const uint32_t> keys = m_tables.builtinCells();
    for (uint32_t number = 0; number < keys.size(); ++number) {
        std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(keys[number], table);
        RELEASE_ASSERT(key);
        if (table.at(key->builtin()).origin == BuiltinOrigin::Host || findModuleWithNamespace(table, rootOf(table, key->builtin())))
            continue;
        // Where any code may run, cells are listed that nothing compiled asks for and that the realm does not have: the setter of Map[Symbol.species].
        if (find(*key))
            cell(number);
    }
}

void BuiltinCells::fillAndKeepAlive(uint32_t number, JSCell* cell)
{
    if (m_cells[number])
        return;
    m_state.immortalCells->add(cell);
    m_cells[number] = cell;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
