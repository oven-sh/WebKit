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

enum class NameIs : bool { OfMember, New };

// What the language, an iterator's user or a call reads without a lookup that compiled code could test.
// A conversion to a primitive or to JSON looks `toString`, `valueOf` and `toJSON` up like any call that names them, where the object has such a member.
// So does what resolves a promise with a promise look `then` up. `await` of a promise of the language reads no `then`, replaced or not.
static bool isReadWithoutLookup(const BuiltinTable& table, BuiltinName name, NameIs nameIs)
{
    // No function of the engine gets a `call`, `apply` or `bind` of its own. The members that have these names are read by whoever names them, and by nobody else: a spread call reads no `apply`.
    bool isLookedUpByCalls = nameIs == NameIs::OfMember && (name == BuiltinNames::call || name == BuiltinNames::apply || name == BuiltinNames::bind);
    if (table.isSymbolName(name) || (table.isClosedOnHostFunctions(name) && !isLookedUpByCalls) || name == BuiltinNames::next || name == BuiltinNames::returnKeyword || name == BuiltinNames::throwKeyword)
        return true;
    for (Protocol protocol : allMembersOf<Protocol>) {
        // A `toJSON` that a test adds, to Object.prototype say, is looked up too: what converts to JSON is left to the host where the name is opened.
        if (protocol == Protocol::ToJSON || (nameIs == NameIs::OfMember && (protocol == Protocol::ToPrimitive || protocol == Protocol::Thenable)))
            continue;
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
    // `process.versions`, `process.release`, `process.isBun`: the front end folds no read of them in a test run. The others have names that are read without a lookup.
    case BuiltinKind::DataProperty:
        break;
    case BuiltinKind::Constructor:
    case BuiltinKind::Namespace:
        // `globalThis.Set = Fake`, `globalThis.Math = Object.create(Math)`. Under any other holder a constructor is the `constructor` of a prototype object.
        if (!at(member.holder).flags.contains(BuiltinFlag::IsGlobalObject))
            return false;
        break;
    case BuiltinKind::Prototype:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return false;
    }
    // Whoever walks a map or a set asks what its prototype holds under @@iterator, and an object of the host is walked by the protocol alone.
    if (member.name == nameOf(WellKnownSymbol::Iterator)) {
        BuiltinID holder = canonicalID(member.holder);
        if (holder == canonicalID(Builtins::MapPrototype) || holder == canonicalID(Builtins::SetPrototype) || at(holder).origin == BuiltinOrigin::Host)
            return true;
    }
    return !isReadWithoutLookup(*this, member.name, NameIs::OfMember);
}

bool BuiltinTable::canTakeNameInTestRuns(BuiltinID holder, BuiltinName name) const
{
    const Builtin& object = at(holder);
    if (object.flags.contains(BuiltinFlag::IsGlobalObject) || object.scope != BuiltinScope::Realm)
        return false;
    bool takesNoOtherName = object.flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::IsClosedToOtherNames });
    if (!name)
        return takesNoOtherName;
    // `spyOn(Uint8Array.prototype, "set")`: an object above has the member. What a test puts here stands in front of it for whatever lies below,
    // and whoever looks the member up from there passes this object on the way. So it goes by the member: where a test can replace that, it can hide it.
    if (BuiltinMemberLookup above = findMember(holder, name); above.outcome == BuiltinMemberLookup::Outcome::Found && !findOwnMember(holder, name)) {
        if (!takesNoOtherName && !std::ranges::binary_search(closedKeysOf(holder), name))
            return false;
        if (isClosedOnHostFunctions(name) || isReadWithoutLookup(*this, name, NameIs::OfMember))
            return false;
        return canBeReplacedInTestRuns(above.member) || at(above.member).mutability == BuiltinMutability::ConstantUnlessWritten;
    }
    if (!takesNoOtherName)
        return false;
    // A key that is `undefined`, `null`, `true` or `false` is looked up under its text.
    for (ASCIILiteral text : { "undefined"_s, "null"_s, "true"_s, "false"_s }) {
        if (equalSpans(textOf(name), byteCast<uint8_t>(text.span())))
            return false;
    }
    return !isReadWithoutLookup(*this, name, NameIs::New) && !findOwnMember(holder, name);
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
    BitVector losesLockOfTable(m_builtins.builtinCount());
    for (const ReplaceableBuiltin& entry : replaceableBuiltins) {
        members.append(entry.member);
        if (m_builtins.contains(entry.member) && m_builtins.canBeReplacedInTestRuns(entry.member))
            losesLockOfTable.quickSet(entry.member.index());
    }
    openedNames.removeAllMatching([&](OpenedName& opened) {
        opened.holder = m_builtins.canonicalID(opened.holder);
        return !m_builtins.canTakeNameInTestRuns(opened.holder, m_builtins.nameOf(strings[opened.name]));
    });
    Vector<BuiltinID> holdersOfNewNames;
    for (const OpenedName& opened : openedNames) {
        holdersOfNewNames.append(opened.holder);
        m_namesOpenedSomewhere.set(opened.name.index());
    }
    if (admitsReplacedBuiltins)
        m_tableOfBuild.unlockForTestRun(members.span(), holdersOfNewNames.span());
    else
        m_tableOfBuild.unlock(members.span(), holdersOfNewNames.span());
    m_testsReplaceBuiltins = admitsReplacedBuiltins || !openedNames.isEmpty() || !losesLockOfTable.isEmpty();
}

} // namespace Typed

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
