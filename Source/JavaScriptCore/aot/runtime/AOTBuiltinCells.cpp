/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCells.h"

#if ENABLE(AOT)

#include "AOTBuiltinFamilyState.h"
#include "AOTBuiltinLocationWatch.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTErrorMessages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTModuleHost.h"
#include "AOTModules.h"
#include "AOTOperationScope.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRealmStartup.h"
#include "AOTRemovableFeature.h"
#include "AOTRuntimeHooks.h"
#include "AOTRuntimeValidation.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "BuiltinNames.h"
#include "DeferTermination.h"
#include "GetterSetter.h"
#include "GlobalObjectMethodTable.h"
#include "ImmutablePropertyNames.h"
#include "JSCInlines.h"
#include "Options.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <bit>
#include <mutex>
#include <span>
#include <utility>
#include <wtf/DataLog.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Scope.h>
#include <wtf/StackPointer.h>
#include <wtf/Vector.h>
#include <wtf/WTFProcess.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

[[noreturn]] static void exitForWantOfHost(const BuiltinTable& table, BuiltinID builtin)
{
    dataLogLn("AOT: the program needs the host's ", table.textOf(builtin), ", and this realm has no host");
    exitProcess(4);
}

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
    m_instance.builtinBindings.directFunctions = BuiltinBindings::directFunctions().data();
    m_instance.builtinBindings.nativeFunctionsOfHost = BuiltinBindings::nativeFunctionsOfHost().data();
}

static bool isImmutableLocation(const BuiltinTable& table, const ProgramTables& tables, BuiltinID builtin)
{
    if (table.at(builtin).mutability == BuiltinMutability::AlwaysLocked)
        return !ProgramTables::isUnlocked(tables.unlockedBuiltinMembers(), builtin.index());
    std::span<const uint32_t> constantMembers = tables.constantBuiltinMembers();
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

static void lockMemberOfLockedHolder(JSGlobalObject*, JSObject* holder, UniquedStringImpl* key, JSObject* member);
static void lockObjectNamedByGlobalObject(JSGlobalObject*, BuiltinID, JSObject*);

// A holder adds some members when the program first reads them, which shows in the order of its keys. Compiled code that asks for such an object reads nothing:
// where the image has a word for the read, and it is empty, the object is made beside the holder, which takes it when the read comes.
std::expected<JSCell*, BuiltinLookupFailure> BuiltinCells::findInKnownHolder(BuiltinCellKey key, JSObject* holder)
{
    JSGlobalObject* realm = m_instance.globalObject;
    std::optional<uint32_t> numberOfRead = key.kind() == BuiltinCellKey::Kind::Object ? numberOf(BuiltinCellKey::value(key.builtin())) : std::nullopt;
    if (!numberOfRead || m_cells[*numberOfRead])
        return findInHolder(realm, key, holder);

    VM& vm = realm->vm();
    const BuiltinTable& table = BuiltinTable::singleton();
    Identifier name = table.identifierOf(vm, table.at(key.builtin()).name);
    if (!reportsCreationOfMember(holder, name.impl()) || isValidOffset(holder->getDirectOffset(vm, name)))
        return findInHolder(realm, key, holder);
    // Whichever image of the realm made it.
    for (Instance* instance = realm->aotInstance(); instance; instance = instance->above) {
        if (JSObject* member = instance->state ? instance->state->builtinCells->memberMadeAhead(holder, name.impl()) : nullptr)
            return static_cast<JSCell*>(member);
    }
    auto made = makeMemberOfStaticTable(realm, key, holder);
    if (!made || !(*made)->isObject())
        return made;
    m_state.immortalCells->add(holder);
    m_state.immortalCells->add(*made);
    m_membersMadeAhead.append(MemberMadeAhead { holder, name, asObject(*made) });
    lockMemberOfLockedHolder(realm, holder, name.impl(), asObject(*made));
    return made;
}

JSObject* BuiltinCells::memberMadeAhead(JSObject* holder, UniquedStringImpl* key) const
{
    for (const MemberMadeAhead& made : m_membersMadeAhead) {
        if (made.holder == holder && made.key.impl() == key)
            return made.member;
    }
    return nullptr;
}

JSObject* BuiltinCells::takeMemberMadeAhead(JSObject* holder, UniquedStringImpl* key)
{
    size_t index = m_membersMadeAhead.findIf([&](const MemberMadeAhead& made) {
        return made.holder == holder && made.key.impl() == key;
    });
    if (index == notFound)
        return nullptr;
    JSObject* member = m_membersMadeAhead[index].member;
    m_membersMadeAhead.removeAt(index);
    return member;
}

// The program may have replaced a step of the path, so the lookup starts at the nearest holder that was found before the program ran.
std::expected<JSCell*, BuiltinLookupFailure> BuiltinCells::find(BuiltinCellKey key)
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
    // The program reads the member here. The word says no more than that, so any cell will do.
    if (key.kind() == BuiltinCellKey::Kind::Value) {
        JSCell* holderCell = findObject(holder);
        if (!holderCell)
            return makeUnexpected(BuiltinLookupFailure::HolderIsMissing);
        auto read = findValueInHolder(realm, asObject(holderCell), table.identifierOf(realm->vm(), table.at(id).name));
        if (!read)
            return makeUnexpected(read.error());
        return read->isCell() ? read->asCell() : holderCell;
    }
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
        return findInKnownHolder(key, object);
    if (auto holderCell = find(BuiltinCellKey::object(holder)))
        return findInKnownHolder(key, asObject(*holderCell));
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
    m_lockedBuiltins.set(id.index());
    m_builtinsOfLockedObjects.add(object, id.index());
    m_state.immortalCells->add(object);
    return true;
}

BuiltinID BuiltinCells::builtinOfLockedObject(JSObject* object) const
{
    auto found = m_builtinsOfLockedObjects.find(object);
    return found == m_builtinsOfLockedObjects.end() ? BuiltinID() : BuiltinID(found->value);
}

static JSC_DECLARE_HOST_FUNCTION(exitForFunctionOfMissingHost);

JSC_DEFINE_HOST_FUNCTION(exitForFunctionOfMissingHost, (JSGlobalObject* realm, CallFrame* callFrame))
{
    std::optional<BuiltinCellKey> key = State::of(realm).builtinCells->keyOfCellHolding(callFrame->jsCallee());
    RELEASE_ASSERT(key);
    exitForWantOfHost(BuiltinTable::singleton(), key->builtin());
}

// A call of such a row enters the compiler's own function, whatever the cell holds, so compiled code asks for the cell in a realm without the host too. It also reads through the cell without a test.
static JSFunction* makeFunctionOfMissingHost(JSGlobalObject* realm, const BuiltinTable& table, BuiltinCellKey key)
{
    if (key.kind() != BuiltinCellKey::Kind::Object || BuiltinBindings::provides(table.at(key.builtin()).origin))
        return nullptr;
    bool hasFunctionOfCompiler = std::ranges::any_of(table.overloadSummariesOf(key.builtin()), [](const BuiltinOverloadSummary& overload) {
        return overload.kind == BuiltinImplementationKind::Compiled || overload.kind == BuiltinImplementationKind::Inline;
    });
    if (!hasFunctionOfCompiler)
        return nullptr;
    VM& vm = realm->vm();
    return JSFunction::create(vm, realm, 0, table.identifierOf(vm, table.at(key.builtin()).name).string(), exitForFunctionOfMissingHost, ImplementationVisibility::Public);
}

JSCell* BuiltinCells::cell(uint32_t number)
{
    std::span<const uint32_t> keys = m_tables.builtinCells();
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
    size_t softReservedZoneSize = m_instance.vm->softReservedZoneSize();
    bool usesStackKeptFromProgram = isNearEndOfStack(*m_instance.vm);
    if (usesStackKeptFromProgram && softReservedZoneSize > Options::softReservedZoneSize()) [[unlikely]]
        m_instance.vm->updateSoftReservedZoneSize(Options::softReservedZoneSize());
    auto restoreStackKeptFromProgram = makeScopeExit([&] {
        if (usesStackKeptFromProgram) [[unlikely]]
            m_instance.vm->updateSoftReservedZoneSize(softReservedZoneSize);
    });
    // What a host runs to create one of its objects is no code of the program: the operation that asks for the cell has not entered the program.
    bool didEnterVM = m_instance.vm->didEnterVM;
    loadModuleOf(key->builtin());
    auto found = find(*key);
    m_instance.vm->didEnterVM = didEnterVM;
    if (JSCell* filledDuringLookup = m_cells[number])
        return filledDuringLookup;

    bool isImmutable = isImmutableLocation(table, m_tables, key->builtin());
    if (!found) {
        // Compiled code tests what it has looked up against the cell of a built-in that the realm may lack, and reads through it only behind the test.
        if (isImmutable && BuiltinBindings::provides(table.at(key->builtin()).origin) && !table.has(BuiltinMark::MayBeAbsent, key->builtin())) {
            dataLogLn("AOT: built-in cell ", number, " (key ", key->encoded(), ", ", table.textOf(key->builtin()), ") is not in this realm: ", static_cast<unsigned>(found.error()));
            CRASH();
        }
        if (JSFunction* function = makeFunctionOfMissingHost(m_instance.globalObject, table, *key)) {
            fillAndKeepAlive(number, function);
            return function;
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
    // The host keeps its private globals on the global object, each under the private name of the engine that is spelled as the member is.
    bool isPrivateGlobal = member.holder == Builtins::PrivateGlobals;
    PrivateSymbolImpl* privateName = isPrivateGlobal ? vm.propertyNames->builtinNames().lookUpPrivateName(table.identifierOf(vm, member.name)) : nullptr;
    JSCell* holder = isPrivateGlobal ? (privateName ? realm : nullptr) : findObject(member.holder);
    if (!holder) {
        if (!BuiltinBindings::provides(member.origin))
            exitForWantOfHost(table, key.builtin());
        dataLogLn("AOT: the holder of the built-in value ", table.textOf(key.builtin()), " (key ", key.encoded(), ") is not in this realm");
        CRASH();
    }
    JSValue value = asObject(holder)->get(realm, privateName ? Identifier::fromUid(*privateName) : table.identifierOf(vm, member.name));
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

bool BuiltinCells::awaitCreation(uint32_t number, AwaitedOrigin origin)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(m_tables.builtinCells()[number], table);
    RELEASE_ASSERT(key);
    const Builtin& member = table.at(key->builtin());
    if (key->kind() != BuiltinCellKey::Kind::Object || (origin == AwaitedOrigin::Host && member.origin != BuiltinOrigin::Host) || !member.holder || !member.name)
        return false;
    // A realm need not have every holder of the language, and asking for the cell of one that it lacks ends the process.
    JSCell* holderCell = origin == AwaitedOrigin::Host ? findObject(member.holder) : find(BuiltinCellKey::object(member.holder)).value_or(nullptr);
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
    if (!held || !held.isCell() || attributes & PropertyAttribute::CustomValue)
        return;
    auto fill = [&](BuiltinCellKey cellKey, JSCell* cell) {
        std::optional<uint32_t> number = cell ? numberOf(cellKey) : std::nullopt;
        if (number && !m_isUnavailable.get(*number))
            fillAndKeepAlive(*number, cell);
    };
    // Native code without a function object: `URL.prototype.href`. The function that stands for it is made when somebody asks, out of what the holder has then.
    if (attributes & PropertyAttribute::CustomAccessor) {
        for (BuiltinCellKey cellKey : { BuiltinCellKey::getter(member), BuiltinCellKey::setter(member) }) {
            if (numberOf(cellKey))
                fill(cellKey, findInHolder(m_instance.globalObject, cellKey, holder).value_or(nullptr));
        }
        return;
    }
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

// The table leads from a module to its namespace object only.
static BitVector namespacesOfBuiltinModules(const BuiltinTable& table)
{
    BitVector namespaces(table.builtinCount());
    for (unsigned index = 0; index < table.moduleCount(); ++index) {
        if (BuiltinID root = table.namespaceOf(RuntimeModuleID(index)))
            namespaces.quickSet(root.index());
    }
    return namespaces;
}

void BuiltinCells::fillCellsUnderModules()
{
    if (m_tables.eagerBuiltinCells().empty())
        return;
    const BuiltinTable& table = BuiltinTable::singleton();
    BitVector namespaces = namespacesOfBuiltinModules(table);
    BuiltinID rootSeenLast;
    RuntimeModuleID module;
    for (uint32_t number : m_tables.eagerBuiltinCells()) {
        std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(m_tables.builtinCells()[number], table);
        RELEASE_ASSERT(key);
        BuiltinID root = rootOf(table, key->builtin());
        if (!namespaces.quickGet(root.index()))
            continue;
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

void BuiltinCells::didLoadNativeModule(RuntimeModuleID module, JSObject* exports)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinID root = table.namespaceOf(module);
    if (!root)
        return;
    // "node:process" exports the object that the global object names, and a program may come to it by the module first. Only noted as locked it would never be:
    // whoever comes to lock it finds the note.
    if (table.isUnder(root, Builtins::GlobalThis))
        lockObjectNamedByGlobalObject(m_instance.globalObject, root, exports);
    didLock(root, exports);
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
        // The first read of such a member decides where it stands among the keys of its holder.
        if (awaitCreation(number, AwaitedOrigin::Any))
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

// What happens to a realm concerns every image that it runs.
template<typename Functor>
static ALWAYS_INLINE void forEachInstanceOf(JSGlobalObject* realm, const Functor& functor)
{
    Instance* first = realm->aotInstance();
    if (!first)
        return;
    if (!first->above) [[likely]] {
        functor(*first);
        return;
    }
    Instance::forEachOf(realm, functor);
}

void hostDidLoadBuiltinModule(JSGlobalObject* realm, uint32_t module, JSValue exports)
{
    if (!exports.isObject() || module >= BuiltinTable::singleton().moduleCount())
        return;
    forEachInstanceOf(realm, [&](Instance& instance) {
        if (instance.state)
            instance.state->builtinCells->didLoadModule(RuntimeModuleID(module), asObject(exports));
    });
}

void hostDidReplaceProperty(JSGlobalObject* realm, UniquedStringImpl* key)
{
    forEachInstanceOf(realm, [&](Instance& instance) {
        if (instance.state)
            instance.state->builtinCells->forgetValuesNamed(key);
    });
}

JSC_DEFINE_JIT_OPERATION(operationAOTFillBuiltinValue, EncodedJSValue, (Instance* instance, uint32_t cellKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFillBuiltinValue);
    std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(cellKey, BuiltinTable::singleton());
    RELEASE_ASSERT(key);
    JSValue value = instance->state->builtinCells->fillValue(globalObject, *key);
    // Compiled code takes the value for what the host makes, without a test.
    if (value && value.isUndefined() && !BuiltinBindings::provides(BuiltinTable::singleton().at(key->builtin()).origin)) [[unlikely]]
        exitForWantOfHost(BuiltinTable::singleton(), key->builtin());
    OPERATION_RETURN(scope, JSValue::encode(value));
}

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

// The cells of a program know what was locked for it. Without a program the table was locked.
static BuiltinID builtinOfLockedObject(const Instance* instanceOfObject, JSObject* object)
{
    State* state = instanceOfObject ? instanceOfObject->state : nullptr;
    if (BuiltinID id = state && state->builtinCells ? state->builtinCells->builtinOfLockedObject(object) : BuiltinID())
        return id;
    Locker locker { namesOfLockedObjectsLock };
    return namesOfLockedObjects().get(object);
}

static Instance* instanceOfRealmOf(JSObject* object)
{
    JSGlobalObject* realm = object->realm();
    return realm ? Instance::of(realm) : nullptr;
}

void didRejectWrite(JSGlobalObject* realm, JSObject* object, UniquedStringImpl* keyOrNull, RejectedWrite change)
{
    if (!refusedChangeListener) [[likely]]
        return;
    BuiltinID id = builtinOfLockedObject(instanceOfRealmOf(object), object);
    if (!id)
        return;
    String holder = BuiltinTable::singleton().textOf(id);
    auto member = [&]() -> String {
        if (keyOrNull->isSymbol())
            return makeString(holder, "[Symbol("_s, StringView(keyOrNull), ")]"_s);
        return id == Builtins::GlobalThis ? String(keyOrNull) : makeString(holder, '.', StringView(keyOrNull));
    };
    switch (change) {
    case RejectedWrite::Store:
        return refusedChangeListener(realm, makeString("Cannot assign to "_s, member()));
    case RejectedWrite::Definition:
        return refusedChangeListener(realm, makeString("Cannot define "_s, member()));
    case RejectedWrite::Deletion:
        return refusedChangeListener(realm, makeString("Cannot delete "_s, member()));
    case RejectedWrite::Prototype:
        return refusedChangeListener(realm, makeString("Cannot change the prototype of "_s, holder));
    }
}

static bool admitsReplacedBuiltins(const Instance&);

String lockedObjectErrorMessage(JSGlobalObject*, JSObject* object, UniquedStringImpl* key, RejectedWrite change)
{
    Instance* instance = instanceOfRealmOf(object);
    BuiltinID id = builtinOfLockedObject(instance, object);
    if (!id)
        return { };
    LockedIn lockedIn = instance && admitsReplacedBuiltins(*instance) ? LockedIn::TestRun : LockedIn::Executable;
    VM& vm = object->vm();
    return change == RejectedWrite::Deletion ? deletionFromLockedBuiltinMessage(vm, id, key, lockedIn) : assignmentToLockedBuiltinMessage(vm, id, key, lockedIn);
}

String refusedSpyMessage(JSGlobalObject* globalObject, JSObject* object, UniquedStringImpl* key)
{
    ImmutablePropertyNames* names = object->structure()->listedImmutablePropertyNames();
    String message = names ? names->storeErrorMessage(key) : lockedObjectErrorMessage(globalObject, object, key, RejectedWrite::Store);
    constexpr ASCIILiteral start = "Cannot assign to "_s;
    if (!message.startsWith(start))
        return { };
    return makeString("Cannot spy on "_s, StringView(message).substring(start.length()));
}

static bool admitsReplacedBuiltins(const Instance& instance)
{
    return hasFlag(instance.options().admitted(), AdmittedCode::ReplacedBuiltins);
}

bool reflectThrowsOnLockedProperty(JSObject* lockedObject)
{
    JSGlobalObject* realm = lockedObject->realm();
    Instance* instance = realm ? Instance::of(realm) : nullptr;
    return instance && admitsReplacedBuiltins(*instance);
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

// A lookup by name may take a member for a constant although nothing in the program makes an object that inherits it.
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
    BitVector namespaces = namespacesOfBuiltinModules(table);
    for (uint32_t number : tables.eagerBuiltinCells()) {
        std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(tables.builtinCells()[number], table);
        RELEASE_ASSERT(key);
        BuiltinID holder = table.at(key->builtin()).holder;
        if (!holder || result.holdersThatTellOfWrites.quickGet(holder.index()) || namespaces.quickGet(rootOf(table, holder).index()))
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

// The lowest program of the realm whose build knew the run. An image that holds the library alone was built for every run: it rests on no lock that a run can open, and says nothing about what this one opens.
static Instance* firstInstanceOfRun(JSGlobalObject* realm)
{
    Instance* first = realm->aotInstance();
    return first && first->program->holdsLibraryAlone() && first->above ? first->above : first;
}

namespace {

// Locks a built-in object that the realm has created, and what hangs on it and is there already.
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
        , m_instance(instance)
        , m_isTestRun(admitsReplacedBuiltins(instance))
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

        const Builtin& holder = table.at(id);
        uint32_t memberIndex = holder.firstMember.index();
        for (const Builtin& entry : table.memberEntriesOf(id)) {
            BuiltinID target = entry.sameObjectAs ? entry.sameObjectAs : BuiltinID(memberIndex);
            ++memberIndex;
            if (!m_builtinsToLock.quickGet(target.index()) || hasLocked(target))
                continue;
            if (JSObject* existing = findOrCreateMember(table, object, entry.name, target))
                lock(target, existing);
        }
        if (BuiltinID inherited = holder.inheritsFrom) {
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

    Vector<Identifier, 2> namesOpenedOn(BuiltinID id) const
    {
        Vector<Identifier, 2> names;
        auto appendNamesOf = [&](const Instance& instance) {
            std::span<const uint32_t> openedNames = instance.tables().openedNames();
            for (size_t index = 0; index + 1 < openedNames.size(); index += 2) {
                if (openedNames[index] == (id.index() | ProgramTables::openedName))
                    names.append(Identifier::fromString(m_vm, instance.program->text().view(StringID(openedNames[index + 1] & ~ProgramTables::openedName)).toString()));
            }
        };
        appendNamesOf(m_instance);
        if (const Instance* first = firstInstanceOfRun(m_realm); first && first != &m_instance)
            appendNamesOf(*first);
        return names;
    }

    void makeImmutable(const BuiltinTable& table, BuiltinID id, JSObject* object)
    {
        Vector<Identifier, 2> openedNames = namesOpenedOn(id);
        bool isWholeObject = table.at(id).flags.contains(BuiltinFlag::IsLockedObject);
        // A test may ask whether Object.prototype can be extended. Locked by its names it answers as in plain Bun, and takes as little.
        if (isWholeObject && !m_isTestRun) {
            lockObject(m_vm, id, object);
            if (m_cells)
                m_cells->didLockEveryPropertyOf(object);
            return;
        }
        std::span<const uint32_t> constantMembers = constantMembersOf(table, m_tables, id);
        bool hasFixedNames = isWholeObject || hasMemberLockedByTable(table, id) || !constantMembers.empty();
        bool tellsOfWrites = m_cells && m_cells->holdersThatTellOfWrites().get(id.index());
        // A holder may only lie on the path to what is locked.
        if (!hasFixedNames && !tellsOfWrites)
            return;
        if (object->makePropertiesImmutable(m_vm, immutablePropertyNamesOf(m_vm, id, constantMembers, m_cells, unlockedMembers(), m_isTestRun, openedNames.span()).get())) [[likely]]
            return;
        if (hasFixedNames)
            crashOnBuiltinHolder(id, "cannot be made immutable"_s);
        m_cells->cannotHearOfWritesTo(id);
    }

    std::span<const uint32_t> unlockedMembers() const
    {
        if (m_cells && !m_cells->membersUnlockedInRealm().empty())
            return m_cells->membersUnlockedInRealm();
        return m_tables.unlockedBuiltinMembers();
    }

    VM& m_vm;
    JSGlobalObject* m_realm;
    const ProgramTables& m_tables;
    const Instance& m_instance;
    const bool m_isTestRun;
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
    locker.lock(Builtins::GlobalThis, realm);
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

// The creation of an object that the table locks is reported.
static void lockHoldersOfProgram(BuiltinLocker& locker, JSGlobalObject* realm, const ProgramTables& tables, const BitVector& holdersThatTellOfWrites)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    auto lockCreated = [&](BuiltinID holder) {
        if (locker.hasLocked(holder) || table.isUnder(holder, Builtins::GlobalThis) || isLockedByTable(table, holder) || isCreatedWithReportedStructure(table, holder))
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
    // What the first program of the realm leaves open stays open: its build knows of every file that the programs share and of Bun's own code, which a later build does not see.
    Instance* first = firstInstanceOfRun(realm);
    if (instance.program->holdsLibraryAlone()) {
        // It locks what that program locks, which comes behind it.
        if (first != &instance)
            cells.setMembersUnlockedInRealm(Vector<uint32_t>(first->tables().unlockedBuiltinMembers()));
    } else if (first && first != &instance && first->state) {
        toLock.builtins.filter(first->state->builtinCells->builtinsToLock());
        Vector<uint32_t> unlocked(instance.tables().unlockedBuiltinMembers());
        unlocked.append(first->tables().unlockedBuiltinMembers());
        std::ranges::sort(unlocked);
        unlocked.shrink(std::ranges::unique(unlocked).begin() - unlocked.begin());
        cells.setMembersUnlockedInRealm(WTF::move(unlocked));
    }
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

template<typename Functor>
static ALWAYS_INLINE void forEachInstanceThatLocks(JSGlobalObject* realm, const Functor& functor)
{
    forEachInstanceOf(realm, [&](Instance& instance) {
        if (instance.state && !instance.state->builtinCells->builtinsToLock().isEmpty())
            functor(instance);
    });
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
    const BuiltinTable& table = BuiltinTable::singleton();
    if (table.at(builtinClass).pristineTest == BuiltinPristineTest::InstanceStructure)
        return;
    if (structure->tryMarkPristine(vm) || !validatesRuntime(vm)) [[likely]]
        return;
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

static void didCreateClass(Instance*, JSGlobalObject*, const LazyClassStructure&, Structure*, JSObject* prototype, JSObject* constructor);

void didCreateClass(JSGlobalObject* realm, const LazyClassStructure& lazyStructure, Structure* structure, JSObject* prototype, JSObject* constructor)
{
    forEachInstanceThatLocks(realm, [&](Instance& instance) {
        didCreateClass(&instance, realm, lazyStructure, structure, prototype, constructor);
    });
}

static void didCreateClass(Instance* instance, JSGlobalObject* realm, const LazyClassStructure& lazyStructure, Structure* structure, JSObject* prototype, JSObject* constructor)
{
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
    forEachInstanceOf(realm, [&](Instance& created) {
        if (!created.state)
            return;
        created.state->builtinCells->didCreateMember(holder, key, asObject(value));
        created.state->builtinFamilies->locationWatch->didCreateMember(holder, key, asObject(value));
    });
    lockMemberOfLockedHolder(realm, holder, key, asObject(value));
}

static void lockMemberOfLockedHolder(JSGlobalObject* realm, JSObject* holder, UniquedStringImpl* key, JSObject* made)
{
    forEachInstanceThatLocks(realm, [&](Instance& instance) {
        const BuiltinTable& table = BuiltinTable::singleton();
        BuiltinID holderID = holder == realm ? Builtins::GlobalThis : instance.state->builtinCells->builtinOfLockedObject(holder);
        BuiltinName name = holderID ? table.nameOf(realm->vm(), key) : BuiltinName();
        BuiltinID member = name ? table.findOwnMember(holderID, name) : BuiltinID();
        if (!member)
            return;
        instance.state->builtinCells->didMakeMember(member, made);
        BuiltinLocker locker(instance);
        locker.lock(member, made);
    });
}

static void lockObjectNamedByGlobalObject(JSGlobalObject* realm, BuiltinID id, JSObject* object)
{
    forEachInstanceThatLocks(realm, [&](Instance& instance) {
        BuiltinLocker locker(instance);
        locker.lock(id, object);
    });
}

JSObject* takeStaticPropertyMadeAhead(JSObject* holder, UniquedStringImpl* key)
{
    JSObject* taken = nullptr;
    forEachInstanceOf(holder->realm(), [&](Instance& instance) {
        if (JSObject* member = instance.state ? instance.state->builtinCells->takeMemberMadeAhead(holder, key) : nullptr)
            taken = member;
    });
    return taken;
}

void hostDidCreateStructure(JSGlobalObject* realm, Structure* structure)
{
    if (!realm->aotInstance())
        return;
    BuiltinClassID builtinClass = findClassWithInstanceStructure(realm, structure);
    if (builtinClass) {
        forEachInstanceOf(realm, [&](Instance& instance) {
            instance.didCreateInstanceStructure(builtinClass, structure);
        });
    }
    forEachInstanceThatLocks(realm, [&](Instance& instance) {
        if (validatesRuntime(realm->vm())) [[unlikely]]
            validateClassWithInstanceStructure(realm, structure, builtinClass);
        if (!builtinClass)
            return;
        BuiltinLocker locker(instance);
        lockPrototypeOfClass(locker, builtinClass, structure->storedPrototypeObject());
        markPristine(realm->vm(), builtinClass, structure);
    });
}

void hostDidCreateObjectOfGlobal(JSGlobalObject* realm, UniquedStringImpl* key, JSObject* object)
{
    if (realm->aotInstance())
        lockMemberOfLockedHolder(realm, realm, key, object);
}

void hostDidCreateNodeModule(JSGlobalObject* realm, JSObject* constructor)
{
    forEachInstanceThatLocks(realm, [&](Instance& instance) {
        BuiltinLocker(instance).lock(Builtins::NodeModule, constructor);
    });
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
    if (realm->aotInstance()) {
        Instance::forEachOf(realm, [](Instance& instance) {
            didInstallBuiltins(instance);
        });
    } else if (locksBuiltins(realm))
        lockBuiltinsOfTable(realm);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
