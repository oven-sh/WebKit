/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTProgramTables.h"
#include "RememberedSlot.h"
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

class ProgramStrings;
struct State;

class ProgramStatics {
    WTF_MAKE_NONCOPYABLE(ProgramStatics);
    WTF_MAKE_TZONE_ALLOCATED(ProgramStatics);
public:
    static constexpr unsigned cardShift = InstanceLayout::cardShift;
    static constexpr uint8_t cardIsDirty = RememberedSlotSet::writtenCard;

    explicit ProgramStatics(State&);
    ~ProgramStatics();

    void ensureProgramStaticsInitialized()
    {
        if (!m_programStaticsInitialized) [[unlikely]]
            initializeProgramStatics();
    }
    void initializeStatics(uint32_t firstWord, uint32_t endWord);
    // By its number in ProgramTables::staticsOfFiles(): the group and all that its files import.
    void fillVariablesOfFiles(uint32_t group);
    bool fillsVariablesWithFiles() const { return m_filledFiles; }
    // For whoever hands out the names of a file that is being evaluated and may not have been entered.
    void fillVariablesOfAllFiles();
    void ensureVariableIsFilled(uint32_t offset)
    {
        if (offset / sizeof(uint64_t) - m_firstWordFilledWithFiles < m_numberOfWordsFilledWithFiles) [[unlikely]]
            fillVariablesOfFiles(groupWithWord(offset / sizeof(uint64_t)));
    }
    bool isFilled(uint32_t offset) const;
    void didStoreRootInStatics(uint32_t offset) { m_slots.record(m_instance.statics() + offset); }
    void setCell(uint32_t offset, JSCell* cell) { at<RememberedSlot<JSCell*>>(offset).set(m_slots, cell); }

    template<typename T> T& at(uint32_t offset)
    {
        RELEASE_ASSERT(offset + sizeof(T) <= m_tables.instanceSizes.sizeOfStatics);
        return *reinterpret_cast<T*>(m_instance.statics() + offset);
    }

    static JSValue readVariable(Instance&, SlotData);
    static JSValue readVariableWithoutAllocating(const Instance&, SlotData);
    static void writeVariable(Instance&, SlotData, JSValue);

    template<typename Visitor> void visitRoots(Visitor&);

private:
    template<typename Appender> void visitRegion(Appender&, size_t begin, size_t end);
    void initializeProgramStatics();
    uint32_t groupWithWord(uint32_t word) const;

    Instance& m_instance;
    const ProgramTables& m_tables;
    ProgramStrings& m_strings;
    RememberedSlotSet m_slots;
    bool m_programStaticsInitialized { false };
    uint32_t m_readOnlyStaticsSize { 0 };
    uint32_t m_firstWordFilledWithFiles { 0 };
    uint32_t m_numberOfWordsFilledWithFiles { 0 };
    uint8_t* m_filledFiles { nullptr };
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
