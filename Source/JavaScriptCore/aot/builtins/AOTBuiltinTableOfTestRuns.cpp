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

namespace JSC { namespace AOT {

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

    for (uint32_t index = 0; index < m_builtins.size(); ++index) {
        BuiltinID member(index);
        if (!m_table.canBeReplacedInTestRuns(member) || std::ranges::binary_search(m_unlockedMembers, member) || std::ranges::binary_search(membersThatStayLocked, member))
            continue;
        m_builtins[index].mutability = BuiltinMutability::ConstantUnlessWritten;
        m_membersWhoseLockIsLeftToProgram.append(member);
    }
    for (BuiltinID member : m_data.runtime.lockedMembers) {
        if (!std::ranges::binary_search(m_unlockedMembers, member) && !std::ranges::binary_search(m_membersWhoseLockIsLeftToProgram, member))
            m_lockedMembers.append(member);
    }

    UNUSED_PARAM(holdersOfNewNames);
    lockEveryWholeObjectByNames();

    m_data.builtins = m_builtins.span();
    m_data.runtime.lockedMembers = m_lockedMembers.span();
    m_data.runtime.lockedObjects = m_lockedObjects.span();
    m_data.runtime.closedKeys = m_closedKeys.span();
    answerQuestionsAboutClasses();
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
