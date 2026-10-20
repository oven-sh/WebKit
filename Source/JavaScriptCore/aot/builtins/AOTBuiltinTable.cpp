/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinTable.h"

#if ENABLE(AOT)

#include "AOTBuiltinNameSlots.h"
#include "CommonIdentifiers.h"
#include "IdentifierInlines.h"
#include "VM.h"
#include <algorithm>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/SymbolImpl.h>
#include <wtf/text/SymbolRegistry.h>

namespace JSC { namespace AOT {

const BuiltinTable& BuiltinTable::singleton()
{
    static constinit const BuiltinTable table(builtinTableData);
    return table;
}

template<typename T>
static const T& checkedAt(std::span<const T> list, size_t index)
{
    RELEASE_ASSERT(index < list.size());
    return list[index];
}

template<typename Record, typename Key>
static std::optional<size_t> findPosition(std::span<const Record> records, Key Record::* member, Key key)
{
    auto found = std::ranges::lower_bound(records, key, { }, member);
    if (found == records.end() || (*found).*member != key)
        return std::nullopt;
    return static_cast<size_t>(found - records.begin());
}

template<typename Record, typename Key>
static const Record* findRecord(std::span<const Record> records, Key Record::* member, Key key)
{
    std::optional<size_t> position = findPosition(records, member, key);
    return position ? &records[*position] : nullptr;
}

template<typename Record>
static std::span<const Record> findRecords(std::span<const Record> records, BuiltinID builtin)
{
    auto found = std::ranges::equal_range(records, builtin, { }, &Record::builtin);
    return records.subspan(found.begin() - records.begin(), found.size());
}

bool BuiltinTable::contains(RuntimeFunctionID id) const
{
    return !!findRecord(m_data.runtime.functions, &RuntimeFunctionData::id, id);
}

const Builtin& BuiltinTable::at(BuiltinID id) const
{
    return checkedAt(m_data.builtins, id.index());
}

const BuiltinOverload& BuiltinTable::at(BuiltinOverloadID id) const
{
    return checkedAt(m_data.overloads, id.index());
}

const BuiltinClass& BuiltinTable::at(BuiltinClassID id) const
{
    return checkedAt(m_data.classes, id.index());
}

const BuiltinSlot& BuiltinTable::at(BuiltinSlotID id) const
{
    return checkedAt(m_data.slots, id.index());
}

const BuiltinLayout& BuiltinTable::at(BuiltinLayoutID id) const
{
    return checkedAt(m_data.layouts, id.index());
}

const BuiltinType& BuiltinTable::at(BuiltinTypeID id) const
{
    return checkedAt(m_data.types, id.index());
}

const BuiltinDispatchNode& BuiltinTable::at(BuiltinDispatchNodeID id) const
{
    return checkedAt(m_data.dispatchNodes, id.index());
}

const RuntimeModuleData& BuiltinTable::at(RuntimeModuleID id) const
{
    return checkedAt(m_data.runtime.modules, id.index());
}

const RuntimeFunctionData& BuiltinTable::at(RuntimeFunctionID id) const
{
    const RuntimeFunctionData* found = findRecord(m_data.runtime.functions, &RuntimeFunctionData::id, id);
    RELEASE_ASSERT(found);
    return *found;
}

std::span<const BuiltinOverload> BuiltinTable::overloadsOf(BuiltinID id) const
{
    const Builtin& builtin = at(id);
    if (!builtin.overloadCount)
        return { };
    return m_data.overloads.subspan(builtin.firstOverload.index(), builtin.overloadCount);
}

BuiltinTableIDRange<BuiltinOverloadID> BuiltinTable::overloadIDsOf(BuiltinID id) const
{
    const Builtin& builtin = at(id);
    return { builtin.firstOverload, builtin.overloadCount };
}

std::optional<BuiltinDispatchTarget> BuiltinTable::dispatchOf(BuiltinID id, BuiltinCallMode mode) const
{
    auto keyOf = [](const BuiltinDispatch& dispatch) {
        return std::pair { dispatch.builtin, dispatch.mode };
    };
    std::pair key { id, mode };
    auto found = std::ranges::lower_bound(m_data.dispatches, key, { }, keyOf);
    if (found == m_data.dispatches.end() || keyOf(*found) != key)
        return std::nullopt;
    return found->root;
}

const BuiltinParameter* BuiltinTable::parameterFor(const BuiltinOverload& overload, BuiltinOperand position) const
{
    if (position.isReceiver())
        return overload.receiver;
    unsigned argumentIndex = position.argumentIndex();
    auto found = std::ranges::find_if(overload.parameters, [&](const BuiltinParameter& parameter) {
        return parameter.operand.coversArgument(argumentIndex);
    });
    return found == overload.parameters.end() ? nullptr : &*found;
}

BuiltinTableIDRange<BuiltinID> BuiltinTable::membersOf(BuiltinID holder) const
{
    const Builtin& object = at(holder);
    return { object.firstMember, object.memberCount };
}

BuiltinID BuiltinTable::findOwnMember(BuiltinID holder, BuiltinName name) const
{
    const Builtin& object = at(holder);
    if (holder == Builtins::GlobalThis && name == object.name)
        return holder;
    if (!object.memberCount)
        return { };
    auto members = m_data.builtins.subspan(object.firstMember.index(), object.memberCount);
    std::optional<size_t> position = findPosition(members, &Builtin::name, name);
    // A module's namespace binds `default` to the object. The object has no such property.
    if (!position || members[*position].flags.contains(BuiltinFlag::IsDefaultExport))
        return { };
    return membersOf(holder).at(static_cast<uint32_t>(*position));
}

BuiltinMemberLookup BuiltinTable::findMember(BuiltinID holder, BuiltinName name) const
{
    using Outcome = BuiltinMemberLookup::Outcome;
    BuiltinID object = canonicalID(holder);
    while (true) {
        if (BuiltinID member = findOwnMember(object, name))
            return { .outcome = Outcome::Found, .member = member, .holder = object };
        const Builtin& data = at(object);
        if (!data.flags.contains(BuiltinFlag::ListsAllMembers))
            return { .outcome = Outcome::Unknown };
        switch (data.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return { .outcome = Outcome::Unknown };
        case BuiltinPrototypeLink::Null:
            return { .outcome = Outcome::Absent };
        case BuiltinPrototypeLink::Builtin:
            object = canonicalID(data.inheritsFrom);
            break;
        }
    }
}

bool BuiltinTable::hasLockedPrototypeChain(BuiltinClassID builtinClass) const
{
    if (!at(builtinClass).prototype)
        return false;
    BuiltinID holder = canonicalID(at(builtinClass).prototype);
    while (true) {
        if (!hasLockedPrototypeLink(holder))
            return false;
        const Builtin& object = at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return false;
        case BuiltinPrototypeLink::Null:
            return true;
        case BuiltinPrototypeLink::Builtin:
            holder = canonicalID(object.inheritsFrom);
            break;
        }
    }
}

bool BuiltinTable::mayHavePristineInstances(BuiltinClassID builtinClass) const
{
    const BuiltinClass& instanceClass = at(builtinClass);
    return !instanceClass.isPrimitiveWrapper && !instanceClass.isNeverPristine && hasLockedPrototypeChain(builtinClass);
}

bool BuiltinTable::isRedefinedBelow(BuiltinClassID builtinClass, BuiltinName name) const
{
    return std::ranges::any_of(classesWithInstanceMember(name), [&](BuiltinClassID other) {
        return other != builtinClass && isSameOrSubclassOf(other, builtinClass) && mayHavePristineInstances(other);
    });
}

BuiltinMemberLookup BuiltinTable::findForInstance(BuiltinClassID builtinClass, BuiltinName name) const
{
    if (isRedefinedBelow(builtinClass, name))
        return { .outcome = BuiltinMemberLookup::Outcome::Unknown };
    for (BuiltinClassID current = builtinClass; current; current = at(current).parent) {
        if (BuiltinID prototype = at(current).prototype)
            return findMember(prototype, name);
    }
    return { .outcome = BuiltinMemberLookup::Outcome::Unknown };
}

BuiltinID BuiltinTable::findOwnPropertyOfInstances(BuiltinClassID builtinClass, BuiltinName name) const
{
    BuiltinID member = findForInstance(builtinClass, name).member;
    if (!member || !at(member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances))
        return { };
    return canonicalID(member);
}

bool BuiltinTable::mayDefineOnNewInstances(BuiltinClassID builtinClass, BuiltinName name) const
{
    if (listsOwnPropertiesOfNewInstances(builtinClass))
        return false;
    for (BuiltinClassID current = builtinClass; current; current = at(current).parent) {
        const BuiltinClass& entry = at(current);
        if (!entry.listsNamesDefinedOnNewInstances)
            return true;
        if (name && std::ranges::binary_search(entry.namesDefinedOnNewInstances, name))
            return true;
    }
    return false;
}

bool BuiltinTable::definesCharactersOnNewInstances(BuiltinClassID builtinClass) const
{
    for (BuiltinClassID current = builtinClass; current; current = at(current).parent) {
        if (at(current).definesCharactersOnNewInstances)
            return true;
    }
    return false;
}

bool BuiltinTable::listsOwnPropertiesOfNewInstances(BuiltinClassID builtinClass) const
{
    BuiltinID constructor = at(builtinClass).constructor;
    if (!constructor)
        return false;

    bool hasSignature = false;
    for (const BuiltinOverload& overload : overloadsOf(constructor)) {
        if (overload.mode != BuiltinCallMode::Construct)
            continue;
        if (overload.isUndeclared() || at(overload.result.type).freshness == BuiltinFreshness::Unaliased)
            return false;
        hasSignature = true;
    }
    return hasSignature;
}

std::span<const BuiltinClassID> BuiltinTable::classesWithInstanceMember(BuiltinName name) const
{
    const BuiltinNameClasses* found = findRecord(m_data.names.classesByMemberName, &BuiltinNameClasses::name, name);
    if (!found)
        return { };
    return found->classes;
}

BuiltinClassID BuiltinTable::receiverClassOf(BuiltinID id) const
{
    BuiltinID holder = at(id).holder;
    if (!holder || at(holder).kind != BuiltinKind::Prototype)
        return { };
    return at(holder).instanceClass;
}

bool BuiltinTable::isSameOrSubclassOf(BuiltinClassID builtinClass, BuiltinClassID ancestor) const
{
    for (BuiltinClassID current = builtinClass; current; current = at(current).parent) {
        if (current == ancestor)
            return true;
    }
    return false;
}

BuiltinClassID BuiltinTable::typedArrayClassOf(BuiltinClassID builtinClass) const
{
    for (BuiltinClassID current = builtinClass; current; current = at(current).parent) {
        if (at(current).typedArrayType != NotTypedArray)
            return current;
    }
    return { };
}

template<typename Predicate>
static BuiltinSlotID findSlotIf(const BuiltinTable& table, BuiltinClassID builtinClass, const Predicate& predicate)
{
    for (BuiltinSlotID id : table.at(builtinClass).slots) {
        if (predicate(table.at(id)))
            return id;
    }
    return { };
}

BuiltinSlotID BuiltinTable::findSlot(BuiltinClassID builtinClass, BuiltinName name) const
{
    return findSlotIf(*this, builtinClass, [&](const BuiltinSlot& slot) {
        return slot.name == name;
    });
}

BuiltinSlotID BuiltinTable::findSlot(BuiltinClassID builtinClass, ContentsSlot contentsSlot) const
{
    return findSlotIf(*this, builtinClass, [&](const BuiltinSlot& slot) {
        return slot.contentsSlot == contentsSlot;
    });
}

BuiltinSlotID BuiltinTable::slotInType(BuiltinClassID builtinClass, unsigned indexInType) const
{
    return findSlotIf(*this, builtinClass, [&](const BuiltinSlot& slot) {
        return slot.indexInType == indexInType;
    });
}

unsigned BuiltinTable::numberOfSlotsInType(BuiltinClassID builtinClass) const
{
    return static_cast<unsigned>(std::ranges::count_if(at(builtinClass).slots, [&](BuiltinSlotID id) {
        return at(id).indexInType.has_value();
    }));
}

BuiltinID BuiltinTable::rootOf(BuiltinID id) const
{
    while (true) {
        BuiltinID holder = at(id).holder;
        if (!holder || holder == Builtins::GlobalThis)
            return id;
        id = holder;
    }
}

bool BuiltinTable::isUnder(BuiltinID member, BuiltinID holder) const
{
    for (BuiltinID current = member; current; current = at(current).holder) {
        if (current == holder)
            return true;
    }
    return false;
}

bool BuiltinTable::isLocationOfLockedObject(BuiltinID id) const
{
    BuiltinID holder = at(id).holder;
    return holder && at(holder).flags.contains(BuiltinFlag::IsLockedObject);
}

std::span<const BuiltinName> BuiltinTable::closedKeysOf(BuiltinID holder) const
{
    const BuiltinClosedKeys* found = findRecord(m_data.runtime.closedKeys, &BuiltinClosedKeys::holder, holder);
    if (!found)
        return { };
    return found->names;
}

std::span<const BuiltinName> BuiltinTable::openNamesOf(BuiltinID holder) const
{
    const BuiltinClosedKeys* found = findRecord(m_data.runtime.closedKeys, &BuiltinClosedKeys::holder, holder);
    if (!found)
        return { };
    return found->openNames;
}

bool BuiltinTable::isClosedKey(BuiltinID holder, BuiltinName name) const
{
    if (std::ranges::binary_search(closedKeysOf(holder), name))
        return true;
    if (!at(holder).flags.contains(BuiltinFlag::IsClosedToOtherNames) || isSymbolName(name))
        return false;
    return !findOwnMember(holder, name) && !std::ranges::binary_search(openNamesOf(holder), name);
}

std::span<const BuiltinFeatureUse> BuiltinTable::featureUsesOf(BuiltinID id) const
{
    return findRecords(m_data.attachments.featureUses, id);
}

std::span<const BuiltinModuleLoad> BuiltinTable::moduleLoadsOf(BuiltinID id) const
{
    return findRecords(m_data.attachments.moduleLoads, id);
}

std::span<const BuiltinCalledFunction> BuiltinTable::calledFunctionsOf(BuiltinID id) const
{
    return findRecords(m_data.attachments.calledFunctions, id);
}

const BuiltinUnseenCode* BuiltinTable::unseenCodeOf(BuiltinID id) const
{
    return findRecord(m_data.attachments.unseenCode, &BuiltinUnseenCode::builtin, id);
}

bool BuiltinTable::letsInUnseenCode(BuiltinID id) const
{
    constexpr OptionSet<BuiltinFlag> flagsOfEvents { BuiltinFlag::EvaluatesText, BuiltinFlag::LoadsModuleByName, BuiltinFlag::RunsUnseenCode };
    return at(id).flags.containsAny(flagsOfEvents) || unseenCodeOf(id);
}

const BuiltinForms* BuiltinTable::formsOf(BuiltinID id) const
{
    return findRecord(m_data.attachments.forms, &BuiltinForms::builtin, id);
}

std::optional<BuiltinBoxedStub> BuiltinTable::boxedCallStubOf(BuiltinID id) const
{
    const BuiltinBoxedCall* found = findRecord(m_data.attachments.boxedCalls, &BuiltinBoxedCall::builtin, id);
    if (!found)
        return std::nullopt;
    return found->stub;
}

std::span<const BuiltinName> BuiltinTable::keysReadBy(Protocol protocol) const
{
    auto found = std::ranges::find(m_data.names.protocolKeys, protocol, &BuiltinProtocolKeys::protocol);
    if (found == m_data.names.protocolKeys.end())
        return { };
    return found->keys;
}

BuiltinName BuiltinTable::nameOf(std::span<const uint8_t> utf8) const
{
    std::span<const ASCIILiteral> textNames = m_data.names.textNames;
    auto found = std::lower_bound(textNames.begin(), textNames.end(), utf8, [](ASCIILiteral name, std::span<const uint8_t> bytes) {
        return compareSpans(name.span(), bytes) < 0;
    });
    if (found == textNames.end() || !equalSpans(found->span(), utf8))
        return { };
    return BuiltinName(static_cast<uint32_t>(found - textNames.begin()));
}

BuiltinName BuiltinTable::textNameOf(UniquedStringImpl* key) const
{
    if (key->isSymbol())
        return { };
    std::span<const uint32_t> slots = m_data.names.textNameSlots;
    unsigned hash = key->hash();
    size_t mask = slots.size() - 1;
    for (size_t index = hash & mask; uint32_t slot = slots[index]; index = (index + 1) & mask) {
        if ((slot & ~positionBitsOfNameSlot) != hashBitsOfNameSlot(hash))
            continue;
        uint32_t position = (slot & positionBitsOfNameSlot) - 1;
        if (equal(key, m_data.names.textNames[position]))
            return BuiltinName(position);
    }
    return { };
}

template<typename Key>
static const BuiltinSymbolConstant* findSymbolConstant(const BuiltinTableNames& names, Key BuiltinSymbolConstant::* member, Key key)
{
    auto found = std::ranges::find(names.symbolConstants, key, member);
    return found == names.symbolConstants.end() ? nullptr : &*found;
}

BuiltinName BuiltinTable::nameOfSymbol(BuiltinID wellKnownSymbol) const
{
    const BuiltinSymbolConstant* found = findSymbolConstant(m_data.names, &BuiltinSymbolConstant::symbol, wellKnownSymbol);
    return found ? found->name : BuiltinName();
}

BuiltinName BuiltinTable::nameOf(WellKnownSymbol wellKnownSymbol) const
{
    const BuiltinSymbolConstant* found = findSymbolConstant(m_data.names, &BuiltinSymbolConstant::wellKnownSymbol, wellKnownSymbol);
    return found ? found->name : BuiltinName();
}

std::optional<WellKnownSymbol> BuiltinTable::wellKnownSymbolOf(BuiltinName name) const
{
    const BuiltinSymbolConstant* found = findSymbolConstant(m_data.names, &BuiltinSymbolConstant::name, name);
    if (!found)
        return std::nullopt;
    return found->wellKnownSymbol;
}

bool BuiltinTable::isSymbolName(BuiltinName name) const
{
    return contains(name) && name.index() >= m_data.names.textNames.size();
}

bool BuiltinTable::isRegisteredSymbolName(BuiltinName name) const
{
    return contains(name) && name.index() >= m_data.names.textNames.size() + m_data.names.symbolNames.size();
}

static ASCIILiteral literalOf(const BuiltinTableNames& names, BuiltinName name)
{
    size_t index = name.index();
    if (index < names.textNames.size())
        return names.textNames[index];
    index -= names.textNames.size();
    if (index < names.symbolNames.size())
        return names.symbolNames[index];
    return checkedAt(names.registeredSymbolNames, index - names.symbolNames.size());
}

std::span<const uint8_t> BuiltinTable::textOf(BuiltinName name) const
{
    return byteCast<uint8_t>(literalOf(m_data.names, name).span());
}

String BuiltinTable::textOf(BuiltinID id) const
{
    Vector<BuiltinName, 8> path;
    path.append(at(id).name);
    for (BuiltinID holder = at(id).holder; holder && holder != Builtins::GlobalThis; holder = at(holder).holder)
        path.append(at(holder).name);

    StringBuilder text;
    for (size_t i = path.size(); i--;) {
        ASCIILiteral component = literalOf(m_data.names, path[i]);
        if (isRegisteredSymbolName(path[i])) {
            text.append("[Symbol.for("_s);
            text.appendQuotedJSONString(String(component));
            text.append(")]"_s);
        } else if (isSymbolName(path[i]))
            text.append("[Symbol."_s, component, ']');
        else if (text.isEmpty())
            text.append(component);
        else
            text.append('.', component);
    }
    return text.toString();
}

static const Identifier& identifierOfSymbol(VM& vm, WellKnownSymbol symbol)
{
    const CommonIdentifiers& names = *vm.propertyNames;
    switch (symbol) {
    case WellKnownSymbol::AsyncDispose:
        return names.asyncDisposeSymbol;
    case WellKnownSymbol::AsyncIterator:
        return names.asyncIteratorSymbol;
    case WellKnownSymbol::Dispose:
        return names.disposeSymbol;
    case WellKnownSymbol::HasInstance:
        return names.hasInstanceSymbol;
    case WellKnownSymbol::IsConcatSpreadable:
        return names.isConcatSpreadableSymbol;
    case WellKnownSymbol::Iterator:
        return names.iteratorSymbol;
    case WellKnownSymbol::Match:
        return names.matchSymbol;
    case WellKnownSymbol::MatchAll:
        return names.matchAllSymbol;
    case WellKnownSymbol::Replace:
        return names.replaceSymbol;
    case WellKnownSymbol::Search:
        return names.searchSymbol;
    case WellKnownSymbol::Species:
        return names.speciesSymbol;
    case WellKnownSymbol::Split:
        return names.splitSymbol;
    case WellKnownSymbol::ToPrimitive:
        return names.toPrimitiveSymbol;
    case WellKnownSymbol::ToStringTag:
        return names.toStringTagSymbol;
    case WellKnownSymbol::Unscopables:
        return names.unscopablesSymbol;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinName BuiltinTable::nameOf(VM& vm, UniquedStringImpl* key) const
{
    if (!key->isSymbol())
        return textNameOf(key);
    for (const BuiltinSymbolConstant& constant : m_data.names.symbolConstants) {
        if (identifierOfSymbol(vm, constant.wellKnownSymbol).impl() == key)
            return constant.name;
    }
    if (!static_cast<SymbolImpl*>(key)->isRegistered())
        return { };
    std::span<const ASCIILiteral> registeredNames = m_data.names.registeredSymbolNames;
    for (size_t index = 0; index < registeredNames.size(); ++index) {
        if (equal(key, registeredNames[index]))
            return BuiltinName(static_cast<uint32_t>(m_data.names.textNames.size() + m_data.names.symbolNames.size() + index));
    }
    return { };
}

Identifier BuiltinTable::identifierOf(VM& vm, BuiltinName name) const
{
    ASCIILiteral text = literalOf(m_data.names, name);
    if (!isSymbolName(name))
        return Identifier::fromString(vm, text);
    if (isRegisteredSymbolName(name))
        return Identifier::fromUid(protect(vm.symbolRegistry())->symbolForKey(String(text)).get());
    std::optional<WellKnownSymbol> symbol = wellKnownSymbolOf(name);
    RELEASE_ASSERT(symbol);
    return identifierOfSymbol(vm, *symbol);
}

std::optional<double> BuiltinTable::numberOf(BuiltinID id) const
{
    const BuiltinNumberConstant* found = findRecord(m_data.attachments.numberConstants, &BuiltinNumberConstant::builtin, id);
    if (!found)
        return std::nullopt;
    return found->value;
}

StringView BuiltinTable::tokenOf(BuiltinID id) const
{
    return checkedAt(m_data.tokens.builtins, id.index());
}

StringView BuiltinTable::tokenOf(BuiltinClassID id) const
{
    return checkedAt(m_data.tokens.classes, id.index());
}

StringView BuiltinTable::tokenOf(BuiltinSlotID id) const
{
    return checkedAt(m_data.tokens.slots, id.index());
}

StringView BuiltinTable::tokenOf(BuiltinLayoutID id) const
{
    return checkedAt(m_data.tokens.layouts, id.index());
}

StringView BuiltinTable::tokenOf(RuntimeModuleID id) const
{
    return checkedAt(m_data.tokens.modules, id.index());
}

String BuiltinTable::tokenOf(BuiltinOverloadID id) const
{
    BuiltinID builtin = at(id).builtin;
    return makeString(tokenOf(builtin), '/', id.index() - at(builtin).firstOverload.index());
}

template<typename ID>
static ID findToken(std::span<const ASCIILiteral> tokens, StringView token)
{
    for (unsigned i = 0; i < tokens.size(); ++i) {
        if (token == tokens[i])
            return ID(i);
    }
    return { };
}

BuiltinID BuiltinTable::builtinFromToken(StringView token) const
{
    return findToken<BuiltinID>(m_data.tokens.builtins, token);
}

BuiltinClassID BuiltinTable::classFromToken(StringView token) const
{
    return findToken<BuiltinClassID>(m_data.tokens.classes, token);
}

BuiltinSlotID BuiltinTable::slotFromToken(StringView token) const
{
    return findToken<BuiltinSlotID>(m_data.tokens.slots, token);
}

BuiltinLayoutID BuiltinTable::layoutFromToken(StringView token) const
{
    return findToken<BuiltinLayoutID>(m_data.tokens.layouts, token);
}

RuntimeModuleID BuiltinTable::moduleFromToken(StringView token) const
{
    return findToken<RuntimeModuleID>(m_data.tokens.modules, token);
}

BuiltinOverloadID BuiltinTable::overloadFromToken(StringView token) const
{
    size_t slash = token.reverseFind('/');
    if (slash == notFound)
        return { };
    BuiltinID builtin = builtinFromToken(token.left(slash));
    StringView digits = token.substring(slash + 1);
    if (!builtin || !digits.containsOnly<isASCIIDigit>())
        return { };
    auto index = parseInteger<unsigned>(digits);
    if (!index || *index >= at(builtin).overloadCount)
        return { };
    return overloadIDsOf(builtin).at(*index);
}

bool isValidCheckedJSType(uint32_t word, const BuiltinTable&)
{
    return word < numberOfCheckedJSTypes;
}

bool isValidLinkTimeConstantID(uint32_t word, const BuiltinTable&)
{
    return word < numberOfLinkTimeConstantIDs;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
