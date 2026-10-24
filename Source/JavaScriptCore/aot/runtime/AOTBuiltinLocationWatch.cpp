/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinLocationWatch.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinFamilyState.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTImmortalCells.h"
#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "AOTRealmStartup.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "GetterSetter.h"
#include "JSCInlines.h"
#include "ObjectPropertyConditionSet.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

static std::optional<uint32_t> numberOfCell(const ProgramTables& tables, BuiltinCellKey key)
{
    std::span<const uint32_t> keys = tables.builtinCells();
    auto position = std::ranges::lower_bound(keys, key.encoded());
    if (position == keys.end() || *position != key.encoded())
        return std::nullopt;
    return static_cast<uint32_t>(position - keys.begin());
}

static bool isSameNumber(double a, double b)
{
    return (std::isnan(a) && std::isnan(b)) || std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
}

BuiltinLocationWatch::BuiltinLocationWatch(State& state)
    : m_state(state)
    , m_states(unsafeMakeSpan(state.instance.tableAt<WatchedLocationState>(InstanceLayout::offsetOfBuiltinLocationStates), BuiltinTable::singleton().builtinCount()))
{
    static_assert(InstanceLayout::holdsBuiltinLocationStates(maxNumberOfBuiltins * sizeof(WatchedLocationState)));
    state.instance.builtins.locationStates = std::bit_cast<uint8_t*>(m_states.data());
}

BuiltinLocationWatch::~BuiltinLocationWatch() = default;

WatchedLocationState BuiltinLocationWatch::stateOf(BuiltinID member) const
{
    RELEASE_ASSERT(member.index() < m_states.size());
    return m_states[member.index()];
}

bool BuiltinLocationWatch::arm(VM& vm, BuiltinID member)
{
    RELEASE_ASSERT(member.index() < m_states.size());
    WatchedLocationState& state = m_states[member.index()];
    if (state == WatchedLocationState::NotArmed) {
        switch (tryWatch(vm, member, state)) {
        case Attempt::Watches:
            state = WatchedLocationState::Unchanged;
            break;
        case Attempt::CannotWatch:
            state = WatchedLocationState::Changed;
            break;
        case Attempt::IsNotMadeYet:
            break;
        }
    }
    return state == WatchedLocationState::Unchanged;
}

// The engine also reports each native function of a static table when somebody first looks for it, thousands in a long run.
static bool isMadeByCodeOfHolder(JSObject* holder, UniquedStringImpl* key)
{
    constexpr unsigned madeByCode = PropertyAttribute::PropertyCallback | PropertyAttribute::ClassStructure | PropertyAttribute::CellProperty;
    for (const ClassInfo* info = holder->classInfo(); info; info = info->parentClass) {
        const HashTable* hashTable = info->staticPropHashTable;
        if (const HashTableValue* entry = hashTable ? hashTable->entry(PropertyName(key)) : nullptr)
            return entry->attributes() & madeByCode;
    }
    return false;
}

void BuiltinLocationWatch::didCreateMember(JSObject* holder, UniquedStringImpl* key, JSObject* created)
{
    if (!isMadeByCodeOfHolder(holder, key) || !m_objectsMadeByHost.add(Location { holder, key }, created).isNewEntry)
        return;
    m_keysOfObjectsMadeByHost.append(Identifier::fromUid(*m_state.instance.vm, key));
    m_state.immortalCells->add(holder);
    m_state.immortalCells->add(created);
}

void BuiltinLocationWatch::didFire(VM&, void* context)
{
    // A watchpoint is not destroyed while it fires. The watch stays in the map until the instance goes.
    static_cast<Watch*>(context)->state = WatchedLocationState::Changed;
}

// The program may have replaced a step of the path to the holder, so only a cell of the program's table answers.
JSObject* BuiltinLocationWatch::findHolder(BuiltinID member)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinID holder = table.at(member).holder;
    // A cached store to a free name compares no Structure and fires no watchpoint.
    if (!holder || table.at(holder).flags.contains(BuiltinFlag::IsGlobalObject))
        return nullptr;
    for (BuiltinID name : { holder, table.canonicalID(holder) }) {
        std::optional<uint32_t> number = numberOfCell(m_state.tables, BuiltinCellKey::object(name));
        if (!number)
            continue;
        JSCell* cell = m_state.builtinCells->cell(*number);
        return cell ? cell->getObject() : nullptr;
    }
    return nullptr;
}

// An empty cell says that nobody kept what the realm was created with.
JSCell* BuiltinLocationWatch::cellFilledBeforeProgramRan(BuiltinCellKey key) const
{
    std::optional<uint32_t> number = numberOfCell(m_state.tables, key);
    return number ? m_state.builtinCells->cell(*number) : nullptr;
}

bool BuiltinLocationWatch::isWhatStartUpFound(BuiltinID member, JSValue held) const
{
    const BuiltinTable& table = BuiltinTable::singleton();
    auto isCellOf = [&](BuiltinCellKey key, JSCell* candidate) {
        JSCell* original = cellFilledBeforeProgramRan(key);
        return original && original == candidate;
    };

    switch (table.at(member).kind) {
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method:
        return held.isCell() && isCellOf(BuiltinCellKey::object(member), held.asCell());
    case BuiltinKind::Constant: {
        if (table.nameOfSymbol(member))
            return held.isCell() && isCellOf(BuiltinCellKey::object(member), held.asCell());
        std::optional<double> number = table.numberOf(member);
        return number && held.isNumber() && isSameNumber(*number, held.asNumber());
    }
    case BuiltinKind::Accessor: {
        if (!held.isCell())
            return false;
        // Only native code makes such a cell. What a program defines is a pair of functions.
        if (held.asCell()->type() == CustomGetterSetterType)
            return true;
        auto* accessors = dynamicDowncast<GetterSetter>(held.asCell());
        if (!accessors)
            return false;
        bool hasGetterCell = BuiltinCellKey::getter(member).hasCell(table);
        bool hasSetterCell = BuiltinCellKey::setter(member).hasCell(table);
        if (!hasGetterCell && !hasSetterCell)
            return false;
        if (hasGetterCell && !isCellOf(BuiltinCellKey::getter(member), accessors->getter()))
            return false;
        return !hasSetterCell || isCellOf(BuiltinCellKey::setter(member), accessors->setter());
    }
    case BuiltinKind::DataProperty:
    case BuiltinKind::NativeBinding:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinLocationWatch::Attempt BuiltinLocationWatch::tryWatch(VM& vm, BuiltinID member, WatchedLocationState& state)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    if (!table.at(member).name)
        return Attempt::CannotWatch;
    JSObject* holder = findHolder(member);
    if (!holder || !holder->structure()->propertyAccessesAreCacheable())
        return Attempt::CannotWatch;

    Identifier name = table.identifierOf(vm, table.at(member).name);
    JSObject* madeByHost = nullptr;
    if (table.at(member).flags.contains(BuiltinFlag::IsLazilyCreated)) {
        madeByHost = m_objectsMadeByHost.get(Location { holder, name.impl() });
        if (!madeByHost) {
            // A store puts an own property in place of the entry of the static table, a definition makes the member first, and a deletion makes every member.
            if (isValidOffset(holder->getDirectOffset(vm, name)) || !reportsCreationOfMember(holder, name.impl()))
                return Attempt::CannotWatch;
            return Attempt::IsNotMadeYet;
        }
    }

    ObjectPropertyCondition condition = generateConditionForSelfEquivalence(vm, nullptr, holder, name.impl());
    if (!condition || !condition.isWatchable(PropertyCondition::EnsureWatchability))
        return Attempt::CannotWatch;
    if (madeByHost ? condition.requiredValue() != JSValue(madeByHost) : !isWhatStartUpFound(member, condition.requiredValue()))
        return Attempt::CannotWatch;

    auto watch = makeUnique<Watch>(name, state);
    watch->watchpoint.watch(vm, condition, didFire, watch.get());
    m_watches.set(member.index(), WTF::move(watch));
    return Attempt::Watches;
}

GetValueFunc::Ptr BuiltinLocationWatch::customGetterOf(BuiltinOverloadID overload)
{
    CustomGetter& known = m_customGetters[overload.index() % m_customGetters.size()];
    if (known.overload != overload)
        known = { overload, BuiltinBindings::customGetterOf(overload) };
    return known.function;
}

// The table has the names and not the attributes.
static Structure* structureAfterAdding(VM& vm, const BuiltinTable& table, Structure* structure, std::span<const BuiltinName> names, JSType typeOfInstances)
{
    for (size_t position = 0; structure && position < names.size(); ++position) {
        Identifier name = table.identifierOf(vm, names[position]);
        unsigned attributes = 0;
        PropertyOffset offset = structure->get(vm, name, attributes);
        if (!isValidOffset(offset)) {
            attributes = static_cast<unsigned>(PropertyAttribute::DontEnum);
            structure = Structure::addPropertyTransition(vm, structure, name, attributes, offset);
        }
        if (structure->isDictionary() || (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
            return nullptr;
        if (offset != propertyOffsetOfOwnSlot(typeOfInstances, static_cast<uint8_t>(position)))
            return nullptr;
    }
    return structure;
}

void publishStructureOfOwnSlot(Instance& instance, BuiltinOverloadID id)
{
    const BuiltinTable& table = BuiltinTable::singleton();
    const BuiltinOverloadSummary& overload = table.summaryOf(id);
    RELEASE_ASSERT(overload.kind == BuiltinImplementationKind::OwnSlot);
    const BuiltinOwnSlot& slot = overload.ownSlot;
    RELEASE_ASSERT(slot.structure.index() < BuiltinWords::numberOfOwnSlotStructures);
    uint32_t& published = instance.builtins.ownSlotStructureIDs[slot.structure.index()];
    if (published)
        return;

    const BuiltinRealmStructure& description = table.at(slot.structure);
    BuiltinClassID builtinClass = table.receiverClassOf(overload.builtin);
    RELEASE_ASSERT(builtinClass && description.names.size() == slot.offset + 1u);
    Structure* structure = structureAfterAdding(*instance.vm, table, description.findInitialStructure(instance.globalObject), description.names, table.at(builtinClass).type);
    if (!structure) {
        published = structureIDOfNoInstance;
        return;
    }
    // The number of a dead Structure may be given to another one.
    instance.state->immortalCells->add(structure);
    published = structure->id().bits();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTArmBuiltinLocation, size_t, (Instance* instance, uint32_t member))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArmBuiltinLocation);
    DeferGCForAWhile deferGC(vm);
    OPERATION_RETURN(scope, instance->state->builtinFamilies->locationWatch->arm(vm, BuiltinID(member)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFindOwnSlotStructure, void, (Instance* instance, uint32_t overload))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFindOwnSlotStructure);
    DeferGCForAWhile deferGC(vm);
    publishStructureOfOwnSlot(*instance, BuiltinOverloadID(overload));
}

// The getter takes its receiver for an instance of its class without a test, so compiled code comes here only with a receiver that is proven to be one.
JSC_DEFINE_JIT_OPERATION(operationAOTCallCustomGetter, EncodedJSValue, (Instance* instance, EncodedJSValue thisValue, uint32_t overload, uint32_t key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCallCustomGetter);
    const Identifier& name = identifierAt(instance, key);
    if (GetValueFunc::Ptr getter = instance->state->builtinFamilies->locationWatch->customGetterOf(BuiltinOverloadID(overload)))
        OPERATION_RETURN(scope, getter(globalObject, thisValue, name));
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(thisValue).get(globalObject, name)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
