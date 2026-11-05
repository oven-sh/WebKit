/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCells.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTClosure.h"
#include "AOTErrorMessages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTModuleHost.h"
#include "AOTModuleNamespace.h"
#include "AOTModules.h"
#include "AOTOperationScope.h"
#include "AOTProgramIdentifiers.h"
#include "AOTRealmStartup.h"
#include "AOTRemovableFeature.h"
#include "AOTRuntimeHooks.h"
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

class UseOfStackKeptFromProgram {
    WTF_MAKE_NONCOPYABLE(UseOfStackKeptFromProgram);
public:
    explicit UseOfStackKeptFromProgram(VM& vm)
        : m_vm(vm)
        , m_softReservedZoneSize(vm.softReservedZoneSize())
        , m_isNearEndOfStack(isNearEndOfStack(vm))
    {
        if (m_isNearEndOfStack && m_softReservedZoneSize > Options::softReservedZoneSize()) [[unlikely]]
            m_vm.updateSoftReservedZoneSize(Options::softReservedZoneSize());
    }

    ~UseOfStackKeptFromProgram()
    {
        if (m_isNearEndOfStack) [[unlikely]]
            m_vm.updateSoftReservedZoneSize(m_softReservedZoneSize);
    }

private:
    VM& m_vm;
    size_t m_softReservedZoneSize;
    bool m_isNearEndOfStack;
};

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
    size_t numberOfClasses = BuiltinTable::singleton().classCount();
    m_classesWrittenAbove = unsafeMakeSpan(state.tableArena.allocate<uint8_t>(numberOfClasses), numberOfClasses);
    zeroSpan(m_classesWrittenAbove);
    m_instance.builtins.classesWrittenAbove = m_classesWrittenAbove.data();
    if (hasFlag(m_instance.options().admitted(), AdmittedCode::ReplacedBuiltins)) {
        size_t numberOfBuiltins = BuiltinTable::singleton().builtinCount();
        m_membersWritten = unsafeMakeSpan(state.tableArena.allocate<uint8_t>(numberOfBuiltins), numberOfBuiltins);
        memsetSpan(m_membersWritten, 1);
        m_namesOnHolders = unsafeMakeSpan(state.tableArena.allocate<NamesOnHolder>(numberOfBuiltins), numberOfBuiltins);
        std::ranges::fill(m_namesOnHolders, NamesOnHolder::AreNotHeardOf);
    }
    m_instance.builtins.membersWritten = m_membersWritten.data();
    m_instance.builtins.namesOnHolders = std::bit_cast<uint8_t*>(m_namesOnHolders.data());
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
    if (JSObject* member = memberMadeAhead(holder, name.impl()))
        return static_cast<JSCell*>(member);
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
        if (isValidOffset(object->getDirectOffset(realm->vm(), table.identifierOf(realm->vm(), table.at(id).name))) || object->staticPropertiesReified())
            return makeMemberOfStaticTable(realm, key, object);
    }
    if (!holder || holder == Builtins::GlobalThis)
        return findInRealm(realm, key);
    if (JSObject* object = findKnownObject(holder))
        return findInKnownHolder(key, object);
    if (auto holderCell = find(BuiltinCellKey::object(holder)))
        return findInKnownHolder(key, asObject(*holderCell));
    if (findModuleWithNamespace(table, rootOf(table, holder)))
        return makeUnexpected(BuiltinLookupFailure::HolderIsMissing);
    return findInRealm(realm, key);
}

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
    if (awaited != notFound && isValidOffset(m_awaitedMembers[awaited].holder->getDirectOffset(*m_instance.vm, m_awaitedMembers[awaited].key))) {
        m_awaitedMembers.removeAt(awaited);
        m_isUnavailable.ensureSizeAndSet(number, keys.size());
        return nullptr;
    }

    const BuiltinTable& table = BuiltinTable::singleton();
    std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(keys[number], table);
    RELEASE_ASSERT(key);
    DeferTerminationForAWhile deferTermination(*m_instance.vm);
    UseOfStackKeptFromProgram stackKeptFromProgram(*m_instance.vm);
    bool didEnterVM = m_instance.vm->didEnterVM;
    loadModuleOf(key->builtin());
    auto found = find(*key);
    m_instance.vm->didEnterVM = didEnterVM;
    if (JSCell* filledDuringLookup = m_cells[number])
        return filledDuringLookup;

    bool isImmutable = isImmutableLocation(table, m_tables, key->builtin());
    if (!found) {
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

    fillAndKeepAlive(number, *found);
    return *found;
}

static JSC_DECLARE_HOST_FUNCTION(refuseCallOfBuiltinChangedBeforeFirstUse);

JSC_DEFINE_HOST_FUNCTION(refuseCallOfBuiltinChangedBeforeFirstUse, (JSGlobalObject* realm, CallFrame* callFrame))
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Instance* instance = realm->aotInstance();
    std::optional<BuiltinCellKey> key = instance && instance->state ? instance->state->builtinCells->keyOfCellHolding(callFrame->jsCallee()) : std::nullopt;
    if (key)
        return throwVMTypeError(realm, scope, callOfBuiltinChangedBeforeFirstUseMessage(BuiltinTable::singleton().textOf(key->builtin())));
    return throwVMTypeError(realm, scope, callOfBuiltinChangedBeforeFirstUseMessage(uncheckedDowncast<JSFunction>(callFrame->jsCallee())->name(vm)));
}

JSCell* BuiltinCells::cellReadWithoutTest(uint32_t number)
{
    if (JSCell* found = cell(number))
        return found;
    const BuiltinTable& table = BuiltinTable::singleton();
    std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(m_tables.builtinCells()[number], table);
    RELEASE_ASSERT(key);
    const Builtin& entry = table.at(key->builtin());
    VM& vm = *m_instance.vm;
    if (key->kind() != BuiltinCellKey::Kind::Object || !BuiltinBindings::provides(entry.origin) || table.overloadSummariesOf(key->builtin()).empty())
        return nullptr;
    JSFunction* refusing = JSFunction::create(vm, m_instance.globalObject, 0, table.identifierOf(vm, entry.name).string(), refuseCallOfBuiltinChangedBeforeFirstUse, ImplementationVisibility::Public);
    fillAndKeepAlive(number, refusing);
    return refusing;
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

JSValue BuiltinCells::fillValue(JSGlobalObject* realm, BuiltinCellKey key)
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    EncodedJSValue* word = addressOfValue(key);
    if (JSValue filled = JSValue::decode(*word))
        return filled;

    const BuiltinTable& table = BuiltinTable::singleton();
    const Builtin& member = table.at(key.builtin());
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
    if (JSValue filledDuringRead = JSValue::decode(*word))
        return filledDuringRead;

    if (value.isCell())
        m_state.immortalCells->add(value.asCell());
    *word = JSValue::encode(value);
    return value;
}

JSValue BuiltinCells::fillWatchedValue(JSGlobalObject* realm, BuiltinCellKey key)
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
        if (!BuiltinBindings::provides(member.origin))
            exitForWantOfHost(table, key.builtin());
        dataLogLn("AOT: the holder of the built-in value ", table.textOf(key.builtin()), " (key ", key.encoded(), ") is not in this realm");
        CRASH();
    }
    Identifier name = table.identifierOf(vm, member.name);
    JSValue value;
    {
        UseOfStackKeptFromProgram stackKeptFromProgram(vm);
        value = asObject(holder)->get(realm, name);
    }
    RETURN_IF_EXCEPTION(scope, { });

    size_t place = word - m_values.data();
    auto isValue = [&](JSValue held) { return held == value; };
    auto watch = PropertyValueWatchpoint::tryWatch(vm, asObject(holder), name.impl(), isValue, [](PropertyValueWatchpoint& fired) {
        fired.owner<BuiltinCells>().m_values[fired.number()] = JSValue::encode(JSValue());
    }, this, static_cast<uint32_t>(place));
    if (!watch)
        return value;
    if (m_watchesOfValues.size() <= place)
        m_watchesOfValues.grow(m_values.size());
    m_watchesOfValues[place] = WTF::move(watch);
    *word = JSValue::encode(value);
    return value;
}

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

void BuiltinCells::noteWriteAboveInstances(BuiltinID holder)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    holder = table.canonicalID(holder);
    if (table.at(holder).kind != BuiltinKind::Prototype || m_holdersWhoseWriteIsNoted.set(holder.index()))
        return;
    for (unsigned index = 0; index < table.classCount(); ++index) {
        BuiltinID above;
        for (BuiltinClassID current(index); current && !above; current = table.at(current).parent)
            above = table.at(current).prototype;
        while (above && table.canonicalID(above) != holder) {
            const Builtin& object = table.at(table.canonicalID(above));
            above = object.prototypeLink == BuiltinPrototypeLink::Builtin ? object.inheritsFrom : BuiltinID();
        }
        if (!above)
            continue;
        m_classesWrittenAbove[index] = 1;
        if (isTypedView(table.typedArrayTypeOf(BuiltinClassID(index))))
            m_instance.builtins.someClassOfViewsWasWrittenAbove = 1;
    }
}

void BuiltinCells::willAddNameTo(BuiltinID holder)
{
    if (!m_namesOnHolders.empty())
        m_namesOnHolders[BuiltinTable::singleton().canonicalID(holder).index()] = NamesOnHolder::WereAdded;
    noteWriteAboveInstances(holder);
}

void BuiltinCells::willWrite(BuiltinID member, UniquedStringImpl* key)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    if (!m_membersWritten.empty())
        m_membersWritten[member.index()] = 1;
    noteWriteAboveInstances(table.at(member).holder);
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

void BuiltinCells::hearsOfWritesTo(BuiltinID holder)
{
    if (m_membersWritten.empty())
        return;
    NamesOnHolder& names = m_namesOnHolders[BuiltinTable::singleton().canonicalID(holder).index()];
    if (names == NamesOnHolder::AreNotHeardOf)
        names = NamesOnHolder::AreAsBuilt;
    for (BuiltinID member : BuiltinTable::singleton().membersOf(holder)) {
        if (!m_writtenMembers.quickGet(member.index()))
            m_membersWritten[member.index()] = 0;
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
    noteWriteAboveInstances(holder);
    if (!m_namesOnHolders.empty())
        m_namesOnHolders[table.canonicalID(holder).index()] = NamesOnHolder::WereAdded;
    for (BuiltinID member : table.membersOf(holder)) {
        if (!m_membersWritten.empty())
            m_membersWritten[member.index()] = 1;
        m_writtenMembers.quickSet(member.index());
    }
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

void BuiltinCells::fillAndKeepAlive(uint32_t number, JSCell* cell)
{
    if (m_cells[number])
        return;
    m_state.immortalCells->add(cell);
    m_cells[number] = cell;
}

static BuiltinCells* builtinCellsOfRealm(JSGlobalObject* realm)
{
    Instance* instance = realm->aotInstance();
    return instance && instance->state ? instance->state->builtinCells.get() : nullptr;
}

void hostDidLoadBuiltinModule(JSGlobalObject* realm, uint32_t module, JSValue exports)
{
    if (!exports.isObject() || module >= BuiltinTable::singleton().moduleCount())
        return;
    if (BuiltinCells* cells = builtinCellsOfRealm(realm))
        cells->didLoadModule(RuntimeModuleID(module), asObject(exports));
}

void hostDidReplaceProperty(JSGlobalObject* realm, UniquedStringImpl* key)
{
    if (BuiltinCells* cells = builtinCellsOfRealm(realm))
        cells->forgetValuesNamed(key);
}

JSC_DEFINE_JIT_OPERATION(operationAOTFillBuiltinValue, EncodedJSValue, (Instance* instance, uint32_t cellKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFillBuiltinValue);
    std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(cellKey, BuiltinTable::singleton());
    RELEASE_ASSERT(key);
    JSValue value = instance->state->builtinCells->fillValue(globalObject, *key);
    if (value && value.isUndefined() && !BuiltinBindings::provides(BuiltinTable::singleton().at(key->builtin()).origin)) [[unlikely]]
        exitForWantOfHost(BuiltinTable::singleton(), key->builtin());
    OPERATION_RETURN(scope, JSValue::encode(value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTFillWatchedBuiltinValue, EncodedJSValue, (Instance* instance, uint32_t cellKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFillWatchedBuiltinValue);
    std::optional<BuiltinCellKey> key = BuiltinCellKey::decode(cellKey, BuiltinTable::singleton());
    RELEASE_ASSERT(key);
    OPERATION_RETURN(scope, JSValue::encode(instance->state->builtinCells->fillWatchedValue(globalObject, *key)));
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

static Vector<std::pair<uint32_t, Vector<uint8_t>>>& namesOpenedWithoutImage()
{
    static NeverDestroyed<Vector<std::pair<uint32_t, Vector<uint8_t>>>> names;
    return names;
}

void openNameWithoutImage(uint32_t holder, std::span<const uint8_t> name)
{
    namesOpenedWithoutImage().append({ holder, Vector<uint8_t>(name) });
}

static constinit RefusedChangeListener refusedChangeListener = nullptr;
static constinit Lock namesOfLockedObjectsLock;

static UncheckedKeyHashMap<JSObject*, BuiltinID>& namesOfLockedObjects() WTF_REQUIRES_LOCK(namesOfLockedObjectsLock)
{
    static NeverDestroyed<UncheckedKeyHashMap<JSObject*, BuiltinID>> names;
    return names;
}

void setRefusedChangeListener(RefusedChangeListener listener)
{
    refusedChangeListener = listener;
}

void tellRefusedChangeListener(JSGlobalObject* realm, const String& refusal)
{
    if (refusedChangeListener) [[unlikely]]
        refusedChangeListener(realm, refusal);
}

static void noteNameOfLockedObject(JSObject* object, BuiltinID id)
{
    Locker locker { namesOfLockedObjectsLock };
    namesOfLockedObjects().set(object, id);
}

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
    LockedIn lockedIn = instance && admitsReplacedBuiltins(*instance) && !isWrittenByFileOfProduct(object->vm()) ? LockedIn::TestRun : LockedIn::Executable;
    if (change == RejectedWrite::Prototype)
        return prototypeChangeOfLockedBuiltinMessage(BuiltinTable::singleton().textOf(id), lockedIn);
    VM& vm = object->vm();
    return change == RejectedWrite::Deletion ? deletionFromLockedBuiltinMessage(vm, id, key, lockedIn) : assignmentToLockedBuiltinMessage(vm, id, key, lockedIn);
}

bool isNameRefusedByLibraryFunction(JSObject* object, UniquedStringImpl* key)
{
    auto* closure = dynamicDowncast<Closure>(object);
    return closure && closure->refusesOwnProperty(object->vm(), PropertyName(key));
}

String refusedSpyMessage(JSGlobalObject* globalObject, JSObject* object, UniquedStringImpl* key)
{
    ImmutablePropertyNames* names = object->structure()->listedImmutablePropertyNames();
    String message = names ? names->storeErrorMessage(key) : lockedObjectErrorMessage(globalObject, object, key, RejectedWrite::Store);
    if (isNameRefusedByLibraryFunction(object, key))
        message = assignmentToNameThatBuiltinFunctionInheritsMessage(key);
    constexpr ASCIILiteral start = "Cannot assign to "_s;
    if (!message.startsWith(start))
        return { };
    return makeString("Cannot spy on "_s, StringView(message).substring(start.length()));
}

static LockedIn kindOfRun(JSGlobalObject* realm)
{
    Instance* instance = Instance::of(realm);
    return instance && admitsReplacedBuiltins(*instance) ? LockedIn::TestRun : LockedIn::Executable;
}

String refusedSpyOnExportMessage(JSCell* moduleNamespace, UniquedStringImpl* name, const String& callerOutsideBuild)
{
    auto* object = uncheckedDowncast<ModuleNamespace>(moduleNamespace);
    JSGlobalObject* realm = object->realm();
    return spyOnExportRefusedMessage(name, modulesOf(realm)->table().pathText(object->module()), kindOfRun(realm), callerOutsideBuild);
}

String refusedMockMessage(JSGlobalObject* realm, const String& path, const String& callerOutsideBuild)
{
    return mockOfModuleRefusedMessage(path, kindOfRun(realm), callerOutsideBuild);
}

static bool hasUnlocked(const Instance& instance, BuiltinID member)
{
    return ProgramTables::isUnlocked(instance.tables().unlockedBuiltinMembers(), member.index());
}

static BuiltinID builtinOfFunctionByIdentity(BuiltinCells& cells, JSObject* object)
{
    VM& vm = object->vm();
    if (!object->isCallable())
        return { };
    JSValue stored = object->getDirect(vm, vm.propertyNames->name);
    JSFunction* function = stored ? nullptr : dynamicDowncast<JSFunction>(object);
    String written = function ? function->name(vm) : stored && stored.isString() ? asString(stored)->tryGetValue() : String();
    if (written.isNull())
        return { };
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinName name = table.nameOf(vm, Identifier::fromString(vm, written).impl());
    if (!name)
        return { };
    for (unsigned index = 0; index < table.builtinCount(); ++index) {
        BuiltinID id { index };
        const Builtin& entry = table.at(id);
        bool isFunction = entry.kind == BuiltinKind::Constructor || entry.kind == BuiltinKind::Function;
        if (isFunction && entry.name == name && table.canonicalID(id) == id && cells.findObject(id) == object)
            return id;
    }
    return { };
}

static BuiltinID builtinOfObject(BuiltinCells& cells, JSObject* object)
{
    if (BuiltinID locked = cells.builtinOfLockedObject(object))
        return locked;
    if (std::optional<BuiltinCellKey> filled = cells.keyOfCellHolding(object); filled && filled->kind() == BuiltinCellKey::Kind::Object)
        return filled->builtin();
    return builtinOfFunctionByIdentity(cells, object);
}

bool isMemberUnlockedForTests(JSObject* object, UniquedStringImpl* key)
{
    Instance* running = instanceOfRealmOf(object);
    if (!running)
        return false;
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinName name = table.nameOf(object->vm(), key);
    if (!name)
        return false;
    BuiltinID holder = running->state ? builtinOfObject(*running->state->builtinCells, object) : BuiltinID();
    if (!holder)
        holder = builtinOfLockedObject(running, object);
    BuiltinID member = holder ? table.findOwnMember(holder, name) : BuiltinID();
    return member && (table.at(member).mutability == BuiltinMutability::ChangesOnItsOwn || hasUnlocked(*running, member));
}

bool isMemberTakenForConstant(JSObject* object, UniquedStringImpl* key)
{
    JSGlobalObject* realm = object->realm();
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinName name = realm ? table.nameOf(object->vm(), key) : BuiltinName();
    if (!name)
        return false;
    Instance* instance = realm->aotInstance();
    BuiltinCells* cells = instance && instance->state ? instance->state->builtinCells.get() : nullptr;
    if (!cells)
        return false;
    BuiltinID holder = builtinOfObject(*cells, object);
    BuiltinID member = holder ? table.findOwnMember(holder, name) : BuiltinID();
    return member && isImmutableLocation(table, instance->tables(), member) && !table.at(member).isReadWithoutType;
}

static bool admitsReplacedBuiltins(const Instance& instance)
{
    return hasFlag(instance.options().admitted(), AdmittedCode::ReplacedBuiltins);
}

bool reflectThrowsOnLockedProperty(JSObject* lockedObject)
{
    JSGlobalObject* realm = lockedObject->realm();
    Instance* instance = realm ? Instance::of(realm) : nullptr;
    return instance && admitsReplacedBuiltins(*instance) && !isWrittenByFileOfProduct(lockedObject->vm());
}

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

static bool isRemovedWithFeature(const BuiltinTable& table, BuiltinID holder)
{
    return std::ranges::any_of(table.featureUsesOf(holder), [](const BuiltinFeatureUse& use) {
        return !use.condition && isFeatureRemoved(use.feature);
    });
}

static NEVER_INLINE bool isMadeWithClassOfEngine(const BuiltinTable& table, BuiltinID object)
{
    BuiltinClassID builtinClass = table.at(object).instanceClass;
    if (!builtinClass || !engineNamesInstanceStructureOf(builtinClass))
        return false;
    return table.at(builtinClass).prototype == object || table.at(builtinClass).constructor == object;
}

struct BuiltinsToLock {
    BitVector builtins;
    BitVector holdersThatTellOfWrites;
};

static BuiltinsToLock findBuiltinsToLock(const BuiltinTable& table, const ProgramTables& tables, bool isTestRun)
{
    BuiltinsToLock result { BitVector(table.builtinCount()), BitVector(table.builtinCount()) };
    BitVector& builtins = result.builtins;
    builtins.quickSet(Builtins::GlobalThis.index());
    auto addWithPath = [&](BuiltinID id) ALWAYS_INLINE_LAMBDA {
        while (!builtins.quickSet(id.index())) {
            BuiltinID holder = table.at(id).holder;
            if (!holder || builtins.quickGet(holder.index()) || isMadeWithClassOfEngine(table, id))
                return;
            id = holder;
        }
    };
    for (BuiltinID id : table.lockedObjects())
        addWithPath(id);
    forEachHolderOf(table, table.lockedMembers(), addWithPath);
    forEachHolderOf(table, tables.constantBuiltinMembers(), addWithPath);
    for (unsigned index = 0; isTestRun && index < table.classCount(); ++index) {
        BuiltinID above;
        for (BuiltinClassID current(index); current && !above; current = table.at(current).parent)
            above = table.at(current).prototype;
        while (above && !result.holdersThatTellOfWrites.quickSet(table.canonicalID(above).index())) {
            addWithPath(table.canonicalID(above));
            const Builtin& object = table.at(table.canonicalID(above));
            above = object.prototypeLink == BuiltinPrototypeLink::Builtin ? object.inheritsFrom : BuiltinID();
        }
    }
    if (tables.eagerBuiltinCells().empty())
        return result;
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

namespace {

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
            bool isLockedWhereItIsMade = false;
            if (JSObject* existing = findOrCreateMember(table, object, entry.name, target, isLockedWhereItIsMade))
                lock(target, existing);
            else if (isLockedWhereItIsMade && m_cells)
                m_cells->hearsOfWritesTo(target);
        }
        if (BuiltinID inherited = holder.inheritsFrom) {
            if (JSObject* prototype = object->getPrototypeDirect().getObject())
                lock(inherited, prototype);
        }
    }

private:
    JSObject* findOrCreateMember(const BuiltinTable& table, JSObject* holder, BuiltinName name, BuiltinID member, bool& isLockedWhereItIsMade)
    {
        if (name == BuiltinNames::prototype)
            return findExistingObject(holder, m_vm.propertyNames->prototype.impl());
        if (name == BuiltinNames::constructor)
            return findExistingObject(holder, m_vm.propertyNames->constructor.impl());
        if (isConstructorOfClassNotMade(table, member)) {
            isLockedWhereItIsMade = true;
            return nullptr;
        }
        Identifier key = table.identifierOf(m_vm, name);
        if (JSObject* existing = findExistingObject(holder, key.impl()))
            return existing;
        if (isLockedByTable(table, member) || reportsCreationOfMember(holder, key.impl())) {
            isLockedWhereItIsMade = true;
            return nullptr;
        }
        return findOrCreateObject(m_realm, member);
    }

    bool isConstructorOfClassNotMade(const BuiltinTable& table, BuiltinID member) const
    {
        const Builtin& entry = table.at(member);
        if (!entry.flags.contains(BuiltinFlag::IsLazilyCreated) || !entry.instanceClass || !engineNamesInstanceStructureOf(entry.instanceClass))
            return false;
        const BuiltinClass& builtinClass = table.at(entry.instanceClass);
        if (builtinClass.constructor != member || !builtinClass.prototype)
            return false;
        return m_builtinsToLock.quickGet(builtinClass.prototype.index()) && !hasLocked(builtinClass.prototype);
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
        std::span<const uint32_t> openedNames = m_instance.tables().openedNames();
        for (size_t index = 0; index + 1 < openedNames.size(); index += 2) {
            if (openedNames[index] == (id.index() | ProgramTables::openedName))
                names.append(Identifier::fromString(m_vm, m_instance.program->text().view(StringID(openedNames[index + 1] & ~ProgramTables::openedName)).toString()));
        }
        return names;
    }

    void makeImmutable(const BuiltinTable& table, BuiltinID id, JSObject* object)
    {
        Vector<Identifier, 2> openedNames = namesOpenedOn(id);
        bool isWholeObject = table.at(id).flags.contains(BuiltinFlag::IsLockedObject);
        if (isWholeObject && !m_isTestRun && openedNames.isEmpty()) {
            lockObject(m_vm, id, object);
            return;
        }
        std::span<const uint32_t> constantMembers = constantMembersOf(table, m_tables, id);
        bool hasFixedNames = isWholeObject || hasMemberLockedByTable(table, id) || !constantMembers.empty();
        bool tellsOfWrites = m_cells && m_cells->holdersThatTellOfWrites().get(id.index());
        if (!hasFixedNames && !tellsOfWrites)
            return;
        std::optional<std::span<const uint32_t>> namesTakenForAbsent = m_isTestRun ? std::nullopt : std::optional(m_tables.namesTakenForAbsent());
        if (object->makePropertiesImmutable(m_vm, immutablePropertyNamesOf(m_vm, id, constantMembers, m_cells, m_tables.unlockedBuiltinMembers(), m_isTestRun, openedNames.span(), namesTakenForAbsent).get())) [[likely]] {
            if (m_cells)
                m_cells->hearsOfWritesTo(id);
            return;
        }
        if (hasFixedNames)
            crashOnBuiltinHolder(id, "cannot be made immutable"_s);
        m_cells->cannotHearOfWritesTo(id);
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

static bool isCreatedWithReportedStructure(const BuiltinTable& table, BuiltinID holder)
{
    BuiltinClassID builtinClass = table.at(holder).instanceClass;
    if (!builtinClass || (table.at(builtinClass).prototype != holder && table.at(builtinClass).constructor != holder))
        return false;
    return std::ranges::any_of(BuiltinBindings::classesOfHost(), [&](const BuiltinClassBinding& binding) {
        return binding.builtinClass == builtinClass && binding.namesInstanceStructure;
    });
}

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
    BuiltinsToLock toLock = findBuiltinsToLock(BuiltinTable::singleton(), instance.tables(), admitsReplacedBuiltins(instance));
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
    auto namesOpenedOn = [&](BuiltinID holder) {
        Vector<Identifier, 2> names;
        for (const auto& [opened, name] : namesOpenedWithoutImage()) {
            if (table.canonicalID(BuiltinID(opened)) == table.canonicalID(holder))
                names.append(Identifier::fromString(vm, String::fromUTF8(name.span())));
        }
        return names;
    };
    auto lockByNames = [&](BuiltinID holder, JSObject* object, std::span<const Identifier> openedNames) {
        if (!object->makePropertiesImmutable(vm, immutablePropertyNamesOf(vm, holder, { }, nullptr, { }, false, openedNames).get())) [[unlikely]]
            crashOnBuiltinHolder(holder, "cannot be made immutable"_s);
    };
    for (BuiltinID id : table.lockedObjects()) {
        if (JSObject* object = findOrCreateObject(realm, id)) {
            if (Vector<Identifier, 2> openedNames = namesOpenedOn(id); !openedNames.isEmpty())
                lockByNames(id, object, openedNames.span());
            else
                lockObject(vm, id, object);
            noteNameOfLockedObject(object, id);
        }
    }
    forEachHolderOf(table, table.lockedMembers(), [&](BuiltinID holder) {
        JSObject* object = findOrCreateObject(realm, holder);
        if (!object)
            return;
        lockByNames(holder, object, namesOpenedOn(holder).span());
        noteNameOfLockedObject(object, holder);
    });
    scope.assertNoException();
}

static Instance* instanceThatLocksBuiltins(JSGlobalObject* realm)
{
    BuiltinCells* cells = builtinCellsOfRealm(realm);
    return cells && !cells->builtinsToLock().isEmpty() ? realm->aotInstance() : nullptr;
}

static void markPristine(VM& vm, BuiltinClassID builtinClass, Structure* structure)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    if (table.at(builtinClass).pristineTest == BuiltinPristineTest::InstanceStructure)
        return;
    structure->tryMarkPristine(vm);
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
    Instance* instance = instanceThatLocksBuiltins(realm);
    if (!instance)
        return;
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinClassID builtinClass = findClassWithInstanceStructure(realm, structure);
    if (builtinClass)
        instance->didCreateInstanceStructure(builtinClass, structure);
    BuiltinID constructorID = findGlobalCreatedWith(realm, lazyStructure);
    if (!constructorID)
        constructorID = findMemberOfNamespaceCreatedWith(realm, lazyStructure);
    BuiltinID prototypeID;
    if (constructorID) {
        constructorID = table.canonicalID(constructorID);
        prototypeID = findPrototypeMember(table, constructorID);
    } else if (builtinClass) {
        constructorID = table.at(builtinClass).constructor;
        prototypeID = table.at(builtinClass).prototype;
    }
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
    if (BuiltinCells* cells = builtinCellsOfRealm(realm))
        cells->didCreateMember(holder, key, asObject(value));
    lockMemberOfLockedHolder(realm, holder, key, asObject(value));
}

static void lockMemberOfLockedHolder(JSGlobalObject* realm, JSObject* holder, UniquedStringImpl* key, JSObject* made)
{
    Instance* instance = instanceThatLocksBuiltins(realm);
    if (!instance)
        return;
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinCells& cells = *instance->state->builtinCells;
    BuiltinID holderID = holder == realm ? Builtins::GlobalThis : cells.builtinOfLockedObject(holder);
    BuiltinName name = holderID ? table.nameOf(realm->vm(), key) : BuiltinName();
    BuiltinID member = name ? table.findOwnMember(holderID, name) : BuiltinID();
    if (!member)
        return;
    cells.didMakeMember(member, made);
    BuiltinLocker locker(*instance);
    locker.lock(member, made);
}

static void lockObjectNamedByGlobalObject(JSGlobalObject* realm, BuiltinID id, JSObject* object)
{
    Instance* instance = instanceThatLocksBuiltins(realm);
    if (!instance)
        return;
    BuiltinLocker locker(*instance);
    locker.lock(id, object);
}

JSObject* takeStaticPropertyMadeAhead(JSObject* holder, UniquedStringImpl* key)
{
    BuiltinCells* cells = builtinCellsOfRealm(holder->realm());
    return cells ? cells->takeMemberMadeAhead(holder, key) : nullptr;
}

static void awaitConstructorOfClass(Instance& instance, BuiltinClassID builtinClass, JSObject* prototype)
{
    BuiltinCells& cells = *instance.state->builtinCells;
    BuiltinID constructor = BuiltinTable::singleton().at(builtinClass).constructor;
    if (constructor && prototype && cells.builtinsToLock().get(constructor.index()) && !cells.hasLocked(constructor))
        cells.awaitConstructor(prototype, constructor);
}

void hostDidCreateConstructor(JSGlobalObject* realm, JSObject* constructor)
{
    Instance* instance = instanceThatLocksBuiltins(realm);
    if (!instance)
        return;
    JSObject* prototype = findExistingObject(constructor, realm->vm().propertyNames->prototype.impl());
    if (!prototype)
        return;
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinCells& cells = *instance->state->builtinCells;
    BuiltinID id = cells.takeAwaitedConstructor(prototype);
    if (BuiltinID prototypeID = id ? BuiltinID() : cells.builtinOfLockedObject(prototype)) {
        BuiltinClassID builtinClass = table.at(prototypeID).instanceClass;
        if (builtinClass && table.at(builtinClass).prototype == prototypeID)
            id = table.at(builtinClass).constructor;
    }
    if (!id)
        return;
    BuiltinLocker locker(*instance);
    locker.lock(id, constructor);
}

void hostDidCreateStructure(JSGlobalObject* realm, Structure* structure)
{
    Instance* instance = realm->aotInstance();
    if (!instance)
        return;
    BuiltinClassID builtinClass = findClassWithInstanceStructure(realm, structure);
    if (builtinClass)
        instance->didCreateInstanceStructure(builtinClass, structure);
    if (!instanceThatLocksBuiltins(realm))
        return;
    BuiltinLocker locker(*instance);
    JSObject* prototype = structure->storedPrototypeObject();
    if (!builtinClass) {
        const ClassInfo* info = structure->classInfoForCells();
        BuiltinClassID classOfInstances = BuiltinBindings::classOf(info);
        if (!classOfInstances || BuiltinBindings::instanceClassInfoOf(classOfInstances) != info)
            return;
        lockPrototypeOfClass(locker, classOfInstances, prototype);
        awaitConstructorOfClass(*instance, classOfInstances, prototype);
        return;
    }
    lockPrototypeOfClass(locker, builtinClass, prototype);
    awaitConstructorOfClass(*instance, builtinClass, prototype);
    markPristine(realm->vm(), builtinClass, structure);
}

void hostDidCreateObjectOfGlobal(JSGlobalObject* realm, UniquedStringImpl* key, JSObject* object)
{
    lockMemberOfLockedHolder(realm, realm, key, object);
}

namespace {

class FoldedMembersOfHostObject final : public ImmutablePropertyNames::OutsideNames {
public:
    FoldedMembersOfHostObject(ASCIILiteral holder, LockedIn lockedIn)
        : m_holder(holder)
        , m_lockedIn(lockedIn)
    {
    }

    bool contains(UniquedStringImpl*) const final { return false; }
    String storeErrorMessage(UniquedStringImpl* uid) const final { return assignmentToFoldedMemberMessage(makeString(m_holder, '.', StringView(uid)), m_lockedIn); }
    String deleteErrorMessage(UniquedStringImpl* uid) const final { return deletionOfFoldedMemberMessage(makeString(m_holder, '.', StringView(uid)), m_lockedIn); }
    bool throwsInSloppyMode() const final { return true; }
    bool throwsFromReflect() const final { return m_lockedIn == LockedIn::TestRun; }

private:
    ASCIILiteral m_holder;
    LockedIn m_lockedIn;
};

} // anonymous namespace

void hostDidFillObjectWithFoldedMembers(JSGlobalObject* realm, JSObject* object, ASCIILiteral holder, std::span<const ASCIILiteral> names)
{
    VM& vm = realm->vm();
    IdentifierSet folded;
    for (ASCIILiteral name : names)
        folded.add(Identifier::fromString(vm, name).impl());
    Instance* instance = realm->aotInstance();
    LockedIn lockedIn = instance && admitsReplacedBuiltins(*instance) ? LockedIn::TestRun : LockedIn::Executable;
    Ref<ImmutablePropertyNames> closed = ImmutablePropertyNames::create(WTF::move(folded), { }, { }, adoptRef(*new FoldedMembersOfHostObject(holder, lockedIn)));
    RELEASE_ASSERT(object->makePropertiesImmutable(vm, closed.get()));
}

void hostDidCreateNodeModule(JSGlobalObject* realm, JSObject* constructor)
{
    Instance* instance = instanceThatLocksBuiltins(realm);
    if (!instance)
        return;
    BuiltinLocker locker(*instance);
    locker.lock(Builtins::NodeModule, constructor);
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
    instance.captureCellHeaders();
    scope.assertNoException();
}

void hostDidCreateRealm(JSGlobalObject* realm)
{
    if (realm->aotInstance()) {
        didInstallBuiltins(*realm->aotInstance());
        RuntimeHooks::installDeferredWatchpoints(realm);
    } else if (locksBuiltins(realm))
        lockBuiltinsOfTable(realm);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
