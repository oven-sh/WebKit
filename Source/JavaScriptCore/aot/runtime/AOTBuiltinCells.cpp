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
#include "AOTOperationScope.h"
#include "AOTRealmStartup.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "DeferTermination.h"
#include "GetterSetter.h"
#include "GlobalObjectMethodTable.h"
#include "JSCInlines.h"
#include "Options.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <bit>
#include <utility>
#include <wtf/DataLog.h>
#include <wtf/StackPointer.h>
#include <wtf/WTFProcess.h>

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

static constexpr int exitCodeOfRealmWithoutHost = 4;

static bool isNearEndOfStack(VM& vm)
{
    return std::bit_cast<uintptr_t>(currentStackPointer()) < std::bit_cast<uintptr_t>(vm.softStackLimit()) + BuiltinCells::stackKeptFromProgram;
}

// The kind is the highest byte of a key, so the keys of the values are the last of the sorted list.
static_assert(BuiltinCellKey::Kind::Value > BuiltinCellKey::Kind::Object && BuiltinCellKey::Kind::Value > BuiltinCellKey::Kind::Getter && BuiltinCellKey::Kind::Value > BuiltinCellKey::Kind::Setter);

static uint32_t findFirstValue(std::span<const uint32_t> keys)
{
    return static_cast<uint32_t>(std::ranges::lower_bound(keys, BuiltinCellKey::value(BuiltinID(0)).encoded()) - keys.begin());
}

static JSCell** allocateCells(State& state)
{
    size_t numberOfKeys = state.tables.builtinCells().size();
    if (InstanceLayout::holdsBuiltinCells(numberOfKeys))
        return state.instance.tableAt<JSCell*>(InstanceLayout::offsetOfBuiltinCells);
    return state.tableArena.allocate<JSCell*>(numberOfKeys);
}

static std::span<EncodedJSValue> allocateValues(State& state, size_t numberOfFirstValue)
{
    size_t numberOfKeys = state.tables.builtinCells().size();
    size_t count = numberOfKeys - numberOfFirstValue;
    if (InstanceLayout::holdsBuiltinCells(numberOfKeys))
        return unsafeMakeSpan(state.instance.tableAt<EncodedJSValue>(InstanceLayout::offsetOfBuiltinValues) + numberOfFirstValue, count);
    return unsafeMakeSpan(state.tableArena.allocate<EncodedJSValue>(count), count);
}

BuiltinCells::BuiltinCells(State& state)
    : m_state(state)
    , m_instance(state.instance)
    , m_tables(state.tables)
    , m_cells(allocateCells(state))
    , m_numberOfFirstValue(findFirstValue(state.tables.builtinCells()))
    , m_values(allocateValues(state, m_numberOfFirstValue))
    , m_writtenMembers(BuiltinTable::singleton().builtinCount())
{
    m_instance.prefix().builtinCells = m_cells;
    // Compiled code finds a value by the number of its key, as it finds a cell. There are words for the keys of values only, so the address it starts from lies in front of them.
    m_instance.builtins.hostValues = std::bit_cast<EncodedJSValue*>(std::bit_cast<uintptr_t>(m_values.data()) - m_numberOfFirstValue * sizeof(EncodedJSValue));
    m_instance.builtins.directFunctions = BuiltinBindings::directFunctions().data();
    m_instance.builtins.nativeFunctionsOfHost = BuiltinBindings::nativeFunctionsOfHost().data();
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
    if (m_writtenMembers.quickGet(id.index())) {
        JSObject* object = holder == Builtins::GlobalThis ? realm : findKnownObject(holder);
        if (!object)
            return makeUnexpected(BuiltinLookupFailure::HolderIsMissing);
        if (JSObject* made = key.kind() == BuiltinCellKey::Kind::Object ? m_membersMadeBehindAnnouncedWrite.get(id.index()) : nullptr)
            return static_cast<JSCell*>(made);
        // Where the write was announced and not made, the holder still makes the member itself.
        if (isValidOffset(object->getDirectOffset(realm->vm(), table.identifierOf(realm->vm(), table.at(id).name))) || object->staticPropertiesReified())
            return makeMemberOfStaticTable(realm, key, object);
    }
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
        didLock(root, exports);
}

bool BuiltinCells::didLock(BuiltinID id, JSObject* object)
{
    if (!m_lockedObjects.add(id.index(), object).isNewEntry)
        return false;
    m_builtinsOfLockedObjects.add(object, id.index());
    m_state.immortalCells->add(object);
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
    RELEASE_ASSERT(number < m_numberOfFirstValue);
    if (JSCell* filled = m_cells[number])
        return filled;
    if (m_isUnavailable.get(number))
        return nullptr;
    size_t awaited = m_awaitedMembers.findIf([&](const AwaitedMember& member) {
        return member.number == number;
    });
    // The creation would have been reported, so what is there is what the program has put there.
    if (awaited != notFound && isValidOffset(m_awaitedMembers[awaited].holder->getDirectOffset(*m_instance.vm, m_awaitedMembers[awaited].key))) {
        m_awaitedMembers.removeAt(awaited);
        m_isUnavailable.ensureSizeAndSet(number, keys.size());
        return nullptr;
    }

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
        // Compiled code reads through the cell without a test.
        if (!BuiltinBindings::provides(table.at(key->builtin()).origin)) {
            dataLogLn("AOT: the program needs the host's ", table.textOf(key->builtin()), ", and this realm has no host");
            exitProcess(exitCodeOfRealmWithoutHost);
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

std::optional<BuiltinCellKey> BuiltinCells::keyOfCellHolding(JSCell* object) const
{
    std::span<const uint32_t> keys = m_tables.builtinCells().first(m_numberOfFirstValue);
    for (size_t number = 0; number < keys.size(); ++number) {
        if (m_cells[number] == object)
            return BuiltinCellKey::decode(keys[number], BuiltinTable::singleton());
    }
    return std::nullopt;
}

EncodedJSValue* BuiltinCells::addressOfValue(BuiltinCellKey key)
{
    RELEASE_ASSERT(key.kind() == BuiltinCellKey::Kind::Value);
    std::optional<uint32_t> number = numberOf(key);
    RELEASE_ASSERT(number);
    return &m_values[*number - m_numberOfFirstValue];
}

// The read is the one that the program wrote: the host makes the value now, by code that may throw and that the program may observe.
JSValue BuiltinCells::fillValue(JSGlobalObject* realm, BuiltinCellKey key)
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    EncodedJSValue* word = addressOfValue(key);
    if (JSValue filled = JSValue::decode(*word))
        return filled;

    const BuiltinTable& table = BuiltinTable::singleton();
    const Builtin& member = table.at(key.builtin());
    JSCell* holder = findObject(member.holder);
    if (!holder) {
        dataLogLn("AOT: the holder of the built-in value ", table.textOf(key.builtin()), " (key ", key.encoded(), ") is not in this realm");
        CRASH();
    }
    JSValue value = asObject(holder)->get(realm, table.identifierOf(vm, member.name));
    RETURN_IF_EXCEPTION(scope, { });
    // What the host ran may have read the member too.
    if (JSValue filledDuringRead = JSValue::decode(*word))
        return filledDuringRead;

    // The program may drop the last path to the holder.
    if (value.isCell())
        m_state.immortalCells->add(value.asCell());
    *word = JSValue::encode(value);
    return value;
}

// An empty word is always right: the next read asks the holder. So nobody looks which holder the host stored to.
void BuiltinCells::forgetValuesNamed(UniquedStringImpl* key)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinName name = table.nameOf(*m_instance.vm, key);
    if (!name)
        return;
    std::span<const uint32_t> keys = m_tables.builtinCells().subspan(m_numberOfFirstValue);
    for (size_t index = 0; index < keys.size(); ++index) {
        std::optional<BuiltinCellKey> valueKey = BuiltinCellKey::decode(keys[index], table);
        RELEASE_ASSERT(valueKey);
        if (table.at(valueKey->builtin()).name == name)
            m_values[index] = JSValue::encode(JSValue());
    }
}

bool BuiltinCells::awaitCreation(uint32_t number)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(m_tables.builtinCells()[number], table);
    RELEASE_ASSERT(key);
    const Builtin& member = table.at(key->builtin());
    if (key->kind() != BuiltinCellKey::Kind::Object || member.origin != BuiltinOrigin::Host || !member.holder || !member.name)
        return false;
    JSCell* holderCell = findObject(member.holder);
    JSObject* holder = holderCell ? holderCell->getObject() : nullptr;
    if (!holder)
        return false;

    VM& vm = *m_instance.vm;
    Identifier name = table.identifierOf(vm, member.name);
    if (!reportsCreationOfMember(holder, name.impl()) || isValidOffset(holder->getDirectOffset(vm, name)))
        return false;
    m_state.immortalCells->add(holder);
    m_awaitedMembers.append(AwaitedMember { holder, WTF::move(name), number });
    return true;
}

void BuiltinCells::willWrite(BuiltinID member, UniquedStringImpl* key)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    // Whoever made a realm without the host defines properties of its own under the names of the host's members.
    if (!BuiltinBindings::provides(table.at(member).origin) || m_writtenMembers.quickSet(member.index()))
        return;
    JSObject* holder = m_lockedObjects.get(table.at(member).holder.index());
    if (!holder)
        return;
    unsigned attributes = 0;
    JSValue held = holder->getDirect(*m_instance.vm, key, attributes);
    if (!held || !held.isCell() || attributes & PropertyAttribute::CustomAccessorOrValue)
        return;
    auto fill = [&](BuiltinCellKey cellKey, JSCell* cell) {
        std::optional<uint32_t> number = cell ? numberOf(cellKey) : std::nullopt;
        if (number && !m_isUnavailable.get(*number))
            fillAndKeepAlive(*number, cell);
    };
    if (!(attributes & PropertyAttribute::Accessor)) {
        fill(BuiltinCellKey::object(member), held.asCell());
        return;
    }
    auto* accessors = uncheckedDowncast<GetterSetter>(held.asCell());
    fill(BuiltinCellKey::getter(member), accessors->isGetterNull() ? nullptr : accessors->getter());
    fill(BuiltinCellKey::setter(member), accessors->isSetterNull() ? nullptr : accessors->setter());
}

void BuiltinCells::didMakeMember(BuiltinID member, JSObject* made)
{
    if (!m_writtenMembers.quickGet(member.index()) || !m_membersMadeBehindAnnouncedWrite.add(member.index(), made).isNewEntry)
        return;
    m_state.immortalCells->add(made);
    std::optional<uint32_t> number = numberOf(BuiltinCellKey::object(member));
    if (number && !m_isUnavailable.get(*number))
        fillAndKeepAlive(*number, made);
}

NamespacesOfBuiltinModules::NamespacesOfBuiltinModules(const BuiltinTable& table)
    : m_table(table)
    , m_namespaces(table.builtinCount())
{
    for (unsigned index = 0; index < table.moduleCount(); ++index) {
        if (BuiltinID root = table.namespaceOf(RuntimeModuleID(index)))
            m_namespaces.quickSet(root.index());
    }
}

bool NamespacesOfBuiltinModules::holdAbove(BuiltinID id) const
{
    return m_namespaces.quickGet(rootOf(m_table, id).index());
}

void BuiltinCells::fillCellsUnderModules()
{
    if (m_tables.eagerBuiltinCells().empty())
        return;
    const BuiltinTable& table = BuiltinTable::singleton();
    NamespacesOfBuiltinModules namespaces(table);
    BuiltinID rootSeenLast;
    RuntimeModuleID module;
    for (uint32_t number : m_tables.eagerBuiltinCells()) {
        std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(m_tables.builtinCells()[number], table);
        RELEASE_ASSERT(key);
        if (!namespaces.holdAbove(key->builtin()))
            continue;
        BuiltinID root = rootOf(table, key->builtin());
        if (root != rootSeenLast) {
            rootSeenLast = root;
            module = findModuleWithNamespace(table, root);
        }
        if (!m_lockedObjects.contains(root.index()) && table.at(module).flags.contains(RuntimeModuleFlag::HasSource)) {
            m_cellsAwaitingModules.append({ root.index(), number });
            continue;
        }
        if (!awaitCreation(number))
            cell(number);
    }
}

void BuiltinCells::cannotHearOfWritesTo(BuiltinID holder)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    for (uint32_t number : m_tables.eagerBuiltinCells()) {
        std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(m_tables.builtinCells()[number], table);
        RELEASE_ASSERT(key);
        if (table.at(key->builtin()).holder == holder && !awaitCreation(number))
            cell(number);
    }
    for (BuiltinID member : table.membersOf(holder))
        m_writtenMembers.quickSet(member.index());
}

void BuiltinCells::didLoadModule(RuntimeModuleID module, JSObject* exports)
{
    BuiltinID root = BuiltinTable::singleton().namespaceOf(module);
    if (!root || m_cellsAwaitingModules.isEmpty())
        return;
    Vector<uint32_t, 16> numbers;
    m_cellsAwaitingModules.removeAllMatching([&](const CellAwaitingModule& awaiting) {
        if (awaiting.root != root.index())
            return false;
        numbers.append(awaiting.number);
        return true;
    });
    if (numbers.isEmpty())
        return;
    didLock(root, exports);
    for (uint32_t number : numbers) {
        if (!awaitCreation(number))
            cell(number);
    }
}

void BuiltinCells::didCreateMember(JSObject* holder, UniquedStringImpl* key, JSObject* created)
{
    size_t awaited = m_awaitedMembers.findIf([&](const AwaitedMember& member) {
        return member.holder == holder && member.key.impl() == key;
    });
    if (awaited == notFound)
        return;
    uint32_t number = m_awaitedMembers[awaited].number;
    m_awaitedMembers.removeAt(awaited);
    if (!m_cells[number])
        fillAndKeepAlive(number, created);
}

// A host creates some of its objects at their first use, by code the program can observe: it loads a module, or sets up the queue behind process.nextTick.
void BuiltinCells::fillCellsOfEngine()
{
    const BuiltinTable& table = BuiltinTable::singleton();
    std::span<const uint32_t> keys = m_tables.builtinCells().first(m_numberOfFirstValue);
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

void hostDidLoadBuiltinModule(JSGlobalObject* realm, uint32_t module, JSValue exports)
{
    Instance* instance = Instance::of(realm);
    if (!instance || !instance->state || !exports.isObject() || module >= BuiltinTable::singleton().moduleCount())
        return;
    instance->state->builtinCells->didLoadModule(RuntimeModuleID(module), asObject(exports));
}

void hostDidReplaceProperty(JSGlobalObject* realm, UniquedStringImpl* key)
{
    Instance* instance = Instance::of(realm);
    if (instance && instance->state)
        instance->state->builtinCells->forgetValuesNamed(key);
}

JSC_DEFINE_JIT_OPERATION(operationAOTFillBuiltinValue, EncodedJSValue, (Instance* instance, uint32_t cellKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFillBuiltinValue);
    std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(cellKey, BuiltinTable::singleton());
    RELEASE_ASSERT(key);
    OPERATION_RETURN(scope, JSValue::encode(instance->state->builtinCells->fillValue(globalObject, *key)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
