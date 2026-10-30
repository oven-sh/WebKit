/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinTable.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinNameSlots.h"
#include "AOTIRProgram.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/BitVector.h>

// What a test run unlocks. It runs once per build, over short lists.

namespace JSC { namespace AOT {

// What the language, an iterator's user or a call reads without a lookup that compiled code could test.
static bool isReadWithoutLookup(const BuiltinTable& table, BuiltinName name)
{
    if (table.isSymbolName(name) || table.isClosedOnHostFunctions(name) || name == BuiltinNames::next || name == BuiltinNames::returnKeyword || name == BuiltinNames::throwKeyword)
        return true;
    for (Protocol protocol : allMembersOf<Protocol>) {
        if (std::ranges::contains(table.keysReadBy(protocol), name))
            return true;
    }
    return false;
}

bool BuiltinTable::canBeReplacedInTestRuns(BuiltinID id) const
{
    const Builtin& member = at(id);
    if (member.mutability != BuiltinMutability::AlwaysLocked || member.scope != BuiltinScope::Realm || !member.holder || !member.name)
        return false;
    if (member.flags.containsAny({ BuiltinFlag::IsOwnPropertyOfInstances, BuiltinFlag::IsCompilerHelper, BuiltinFlag::IsDefaultExport }))
        return false;
    switch (member.kind) {
    case BuiltinKind::Function:
    case BuiltinKind::Method:
    case BuiltinKind::Accessor:
        break;
    case BuiltinKind::Constructor:
        // `globalThis.Set = Fake`. Under any other holder it is the `constructor` of a prototype object.
        if (!at(member.holder).flags.contains(BuiltinFlag::IsGlobalObject))
            return false;
        break;
    case BuiltinKind::Namespace:
    case BuiltinKind::Prototype:
    case BuiltinKind::DataProperty:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return false;
    }
    return !isReadWithoutLookup(*this, member.name);
}

bool BuiltinTable::canTakeNameInTestRuns(BuiltinID holder, BuiltinName name) const
{
    const Builtin& object = at(holder);
    if (!object.flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::IsClosedToOtherNames }) || object.flags.contains(BuiltinFlag::IsGlobalObject) || object.scope != BuiltinScope::Realm)
        return false;
    if (!name)
        return true;
    // A key that is `undefined`, `null`, `true` or `false` is looked up under its text.
    for (ASCIILiteral text : { "undefined"_s, "null"_s, "true"_s, "false"_s }) {
        if (equalSpans(textOf(name), byteCast<uint8_t>(text.span())))
            return false;
    }
    return !isReadWithoutLookup(*this, name) && !findOwnMember(holder, name);
}

BuiltinTableOfBuild::BuiltinTableOfBuild(const BuiltinTable& table)
    : m_data(table.m_data)
    , m_table(m_data)
{
}

// As Function.prototype is locked: each member by its name, closed to every other text and to the symbols of the language.
void BuiltinTableOfBuild::lockByNames(BuiltinID wholeObject)
{
    wholeObject = m_table.canonicalID(wholeObject);
    if (!m_builtins[wholeObject.index()].flags.contains(BuiltinFlag::IsLockedObject))
        return;
    for (uint32_t index = 0; index < m_builtins.size(); ++index) {
        Builtin& entry = m_builtins[index];
        if (BuiltinID(index) != wholeObject && entry.sameObjectAs != wholeObject)
            continue;
        if (entry.flags.contains(BuiltinFlag::IsLockedObject))
            entry.flags.add({ BuiltinFlag::HasLockedMembers, BuiltinFlag::IsClosedToOtherNames });
        entry.flags.remove(BuiltinFlag::IsLockedObject);
    }
    m_lockedObjects.removeFirst(wholeObject);

    Vector<BuiltinName> symbols;
    for (uint32_t index = 0; index < m_table.nameCount(); ++index) {
        if (m_table.isSymbolName(BuiltinName(index)) && !m_table.findOwnMember(wholeObject, BuiltinName(index)))
            symbols.append(BuiltinName(index));
    }
    m_closedSymbols.append(WTF::move(symbols));
    size_t position = std::ranges::lower_bound(m_closedKeys, wholeObject, { }, &BuiltinClosedKeys::holder) - m_closedKeys.begin();
    RELEASE_ASSERT(position == m_closedKeys.size() || m_closedKeys[position].holder != wholeObject);
    m_closedKeys.insert(position, BuiltinClosedKeys { wholeObject, m_closedSymbols.last().span(), { } });
}

void BuiltinTableOfBuild::unlock(std::span<const BuiltinID> members, std::span<const BuiltinID> holdersOfNewNames)
{
    RELEASE_ASSERT(m_builtins.isEmpty());
    for (BuiltinID member : members) {
        // A member that is constant unless written is listed for the sake of an image that does not hold the write: it is not to lock the member at its start.
        if (m_table.contains(member) && (m_table.canBeReplacedInTestRuns(member) || m_table.at(member).mutability == BuiltinMutability::ConstantUnlessWritten))
            m_unlockedMembers.append(member);
    }
    if (m_unlockedMembers.isEmpty() && holdersOfNewNames.empty())
        return;
    m_builtins = Vector<Builtin>(m_data.builtins);
    m_lockedObjects = Vector<BuiltinID>(m_data.runtime.lockedObjects);
    m_closedKeys = Vector<BuiltinClosedKeys>(m_data.runtime.closedKeys);
    for (BuiltinID member : m_data.runtime.lockedMembers) {
        if (!std::ranges::binary_search(m_unlockedMembers, member))
            m_lockedMembers.append(member);
    }
    for (BuiltinID member : m_unlockedMembers) {
        m_builtins[member.index()].mutability = BuiltinMutability::ConstantUnlessWritten;
        lockByNames(m_builtins[member.index()].holder);
    }
    for (BuiltinID holder : holdersOfNewNames)
        lockByNames(holder);
    m_data.builtins = m_builtins.span();
    m_data.runtime.lockedMembers = m_lockedMembers.span();
    m_data.runtime.lockedObjects = m_lockedObjects.span();
    m_data.runtime.closedKeys = m_closedKeys.span();
}

namespace Typed {

void Program::unlockReplaceableBuiltins()
{
    Vector<BuiltinID> members;
    for (const ReplaceableBuiltin& entry : replaceableBuiltins)
        members.append(entry.member);
    openedNames.removeAllMatching([&](OpenedName& opened) {
        opened.holder = m_builtins.canonicalID(opened.holder);
        return !m_builtins.canTakeNameInTestRuns(opened.holder, m_builtins.nameOf(strings[opened.name]));
    });
    Vector<BuiltinID> holdersOfNewNames;
    for (const OpenedName& opened : openedNames)
        holdersOfNewNames.append(opened.holder);
    m_tableOfBuild.unlock(members.span(), holdersOfNewNames.span());
    if (unlockedBuiltins().empty())
        return;

    BitVector unlockedNames(m_builtins.nameCount());
    for (BuiltinID member : unlockedBuiltins())
        unlockedNames.quickSet(m_builtins.at(member).name.index());
    auto looksUpUnlockedName = [&](const BuiltinParameter& parameter) {
        auto isUnlocked = [&](BuiltinName name) {
            return unlockedNames.quickGet(name.index());
        };
        return std::ranges::any_of(parameter.namedMembers, isUnlocked) || std::ranges::any_of(parameter.calledMembers, isUnlocked) || std::ranges::any_of(parameter.convertedMembers, isUnlocked);
    };
    const std::pair<BuiltinID, BuiltinID> constructorsAndWhatAddsTheirEntries[] = {
        { Builtins::Map, Builtins::MapPrototypeSet }, { Builtins::Set, Builtins::SetPrototypeAdd },
        { Builtins::WeakMap, Builtins::WeakMapPrototypeSet }, { Builtins::WeakSet, Builtins::WeakSetPrototypeAdd },
    };
    m_functionsCallingUnlockedMembersThatNoRowNames = BitVector(m_builtins.builtinCount());
    for (auto [constructor, member] : constructorsAndWhatAddsTheirEntries) {
        if (isUnlocked(member))
            m_functionsCallingUnlockedMembersThatNoRowNames.quickSet(m_builtins.canonicalID(constructor).index());
    }
    // A row that takes everything from an operand names nothing: Set.prototype.union asks the other set for `has`, `keys` and `size`.
    // So a member that is unlocked anywhere above the instances of a class counts for each function with such a row and a row for a pristine instance of that class.
    auto hasUnlockedMemberAbove = [&](BuiltinClassID instanceClass) {
        for (BuiltinClassID current = instanceClass; current; current = m_builtins.at(current).parent) {
            BuiltinID prototype = m_builtins.at(current).prototype;
            bool isHolder = prototype && std::ranges::any_of(unlockedBuiltins(), [&](BuiltinID member) {
                return m_builtins.canonicalID(m_builtins.at(member).holder) == m_builtins.canonicalID(prototype);
            });
            if (isHolder)
                return true;
        }
        return false;
    };
    auto forEachParameter = [](const BuiltinOverload& overload, NOESCAPE const Invocable<bool(const BuiltinParameter&)> auto& predicate) {
        return (overload.receiver && predicate(*overload.receiver)) || std::ranges::any_of(overload.parameters, predicate);
    };
    auto asksOperandForUnlockedMemberWithoutName = [&](std::span<const BuiltinOverload> overloads) {
        return std::ranges::any_of(overloads, [&](const BuiltinOverload& special) {
            return forEachParameter(special, [&](const BuiltinParameter& pristine) {
                bool isAbove = std::ranges::any_of(m_builtins.at(pristine.accepted).parts, [&](const BuiltinTypePart& part) {
                    return !part.pristine.isEmpty() && part.builtinClass && hasUnlockedMemberAbove(part.builtinClass);
                });
                return isAbove && std::ranges::any_of(overloads, [&](const BuiltinOverload& general) {
                    return general.mode == special.mode && forEachParameter(general, [&](const BuiltinParameter& parameter) {
                        return parameter.operand == pristine.operand && parameter.uses == OperandUses::all();
                    });
                });
            });
        });
    };

    // A function that calls such a function under its name is one too: String.prototype.replace calls @@replace, which calls `exec`.
    m_functionsLookingUpUnlockedNames = BitVector(m_builtins.builtinCount());
    for (bool didMark = true; std::exchange(didMark, false);) {
        for (uint32_t index = 0; index < m_builtins.builtinCount(); ++index) {
            std::span<const BuiltinOverload> overloads = m_builtins.overloadsOf(BuiltinID(index));
            if (overloads.empty() || m_functionsLookingUpUnlockedNames.quickGet(m_builtins.canonicalID(BuiltinID(index)).index()))
                continue;
            bool looksUp = asksOperandForUnlockedMemberWithoutName(overloads) || std::ranges::any_of(overloads, [&](const BuiltinOverload& overload) {
                return forEachParameter(overload, looksUpUnlockedName);
            });
            if (!looksUp)
                continue;
            m_functionsLookingUpUnlockedNames.quickSet(m_builtins.canonicalID(BuiltinID(index)).index());
            if (BuiltinName name = m_builtins.at(BuiltinID(index)).name)
                didMark |= !unlockedNames.quickSet(name.index());
        }
    }
}

} // namespace Typed

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
