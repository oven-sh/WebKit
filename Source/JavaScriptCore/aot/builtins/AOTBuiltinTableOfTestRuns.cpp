/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinTable.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>

// The table as the build of a test run sees it. It is made once per build, in one look at each entry.

namespace JSC { namespace AOT {

// As lockByNames() does for one object, for all of them in one look at the table. The run time locks every built-in object by its names in a test run: a test may ask whether Object.prototype can be extended.
// So a member of such an object is locked where the table locks the member itself, and nowhere else.
void BuiltinTableOfBuild::lockEveryWholeObjectByNames()
{
    Vector<BuiltinID> wholeObjects;
    for (uint32_t index = 0; index < m_builtins.size(); ++index) {
        Builtin& entry = m_builtins[index];
        if (!entry.flags.contains(BuiltinFlag::IsLockedObject))
            continue;
        entry.flags.add({ BuiltinFlag::HasLockedMembers, BuiltinFlag::IsClosedToOtherNames });
        entry.flags.remove(BuiltinFlag::IsLockedObject);
        if (!entry.sameObjectAs)
            wholeObjects.append(BuiltinID(index));
    }
    m_lockedObjects.removeAllMatching([&](BuiltinID object) {
        return !m_builtins[object.index()].flags.contains(BuiltinFlag::IsLockedObject);
    });

    Vector<BuiltinName> symbolsOfLanguage;
    for (uint32_t index = 0; index < m_table.nameCount(); ++index) {
        if (m_table.isSymbolName(BuiltinName(index)))
            symbolsOfLanguage.append(BuiltinName(index));
    }
    for (BuiltinID wholeObject : wholeObjects) {
        Vector<BuiltinName> symbols;
        for (BuiltinName symbol : symbolsOfLanguage) {
            if (!m_table.findOwnMember(wholeObject, symbol))
                symbols.append(symbol);
        }
        m_closedSymbols.append(WTF::move(symbols));
        m_closedKeys.append(BuiltinClosedKeys { wholeObject, m_closedSymbols.last().span(), { } });
    }
    AOT::sort(m_closedKeys, { }, &BuiltinClosedKeys::holder);
    RELEASE_ASSERT(std::ranges::adjacent_find(m_closedKeys, { }, &BuiltinClosedKeys::holder) == m_closedKeys.end());
}

void BuiltinTableOfBuild::unlockForTestRun(std::span<const BuiltinID> members, std::span<const BuiltinID> membersThatStayLocked, std::span<const BuiltinID> holdersOfNewNames)
{
    RELEASE_ASSERT(m_builtins.isEmpty());
    for (BuiltinID member : members) {
        if (m_table.contains(member) && (m_table.canBeReplacedInTestRuns(member) || m_table.at(member).mutability == BuiltinMutability::ConstantUnlessWritten))
            m_unlockedMembers.append(member);
    }
    m_builtins = Vector<Builtin>(m_data.builtins);
    m_lockedObjects = Vector<BuiltinID>(m_data.runtime.lockedObjects);
    m_closedKeys = Vector<BuiltinClosedKeys>(m_data.runtime.closedKeys);
    for (BuiltinID member : m_unlockedMembers)
        m_builtins[member.index()].mutability = BuiltinMutability::ConstantUnlessWritten;

    // `put(getPrototypeOf(Uint8Array).prototype, "values", f)`: no file spells the member. Whether it is written is asked of the flow, as it is of Date.now in every build. Until the end of this function m_table reads the table as it was.
    for (uint32_t index = 0; index < m_builtins.size(); ++index) {
        BuiltinID member(index);
        // The front end has left out a lookup of a member that stays locked. Were the lock left to the program, a store that no file spells would replace the member for some of its readers alone, in silence. It is refused, aloud.
        if (!m_table.canBeReplacedInTestRuns(member) || std::ranges::binary_search(m_unlockedMembers, member) || std::ranges::binary_search(membersThatStayLocked, member))
            continue;
        m_builtins[index].mutability = BuiltinMutability::ConstantUnlessWritten;
        m_membersWhoseLockIsLeftToProgram.append(member);
    }
    for (BuiltinID member : m_data.runtime.lockedMembers) {
        if (!std::ranges::binary_search(m_unlockedMembers, member) && !std::ranges::binary_search(m_membersWhoseLockIsLeftToProgram, member))
            m_lockedMembers.append(member);
    }

    // Each holder of a new name is among them, or takes other names as it is.
    UNUSED_PARAM(holdersOfNewNames);
    lockEveryWholeObjectByNames();

    m_data.builtins = m_builtins.span();
    m_data.runtime.lockedMembers = m_lockedMembers.span();
    m_data.runtime.lockedObjects = m_lockedObjects.span();
    m_data.runtime.closedKeys = m_closedKeys.span();
}

namespace Typed {

void Program::openNameForTestRun(BuiltinID canonicalHolder, StringID name)
{
    openedNames.append(OpenedName { canonicalHolder, name });
    m_namesOpenedSomewhere.set(name.index());
    m_holdersOfOpenedNames.set(canonicalHolder.index());
    m_namesOpened.add(static_cast<uint64_t>(canonicalHolder.index()) << 32 | name.index());
}

} // namespace Typed

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
