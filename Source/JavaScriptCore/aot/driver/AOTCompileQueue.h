/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeRole.h"
#include "AOTCompileUnit.h"
#include "AOTCompiledCode.h"
#include "AOTLinkedFunctions.h"
#include "AOTNumbering.h"
#include "AOTVector.h"
#include "AOTWalkNotes.h"
#include "JSExportMacros.h"
#include <array>
#include <atomic>
#include <optional>
#include <wtf/BitSet.h>
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Seconds.h>

namespace JSC { namespace AOT {

namespace Typed {
class Program;
struct LinkTables;
}

class UnitQueue {
    WTF_MAKE_NONCOPYABLE(UnitQueue);
public:
    JS_EXPORT_PRIVATE UnitQueue(const Typed::Program&, const Typed::CallingConventions&, unsigned numberOfThreads);
    JS_EXPORT_PRIVATE UnitQueue(const Typed::Program&, const Typed::CallingConventions&, const Typed::LinkTables&, Typed::Arrivals&, unsigned numberOfThreads);
    JS_EXPORT_PRIVATE ~UnitQueue();

    JS_EXPORT_PRIVATE void mention(unsigned thread, Typed::CodeUnit, MentionKind = MentionKind::Call);
    JS_EXPORT_PRIVATE void noteEntryByValue(unsigned thread, FunctionID, bool asksEveryTeller);
    JS_EXPORT_PRIVATE bool asksWhoMentions(Typed::CodeUnit) const;
    bool hasMentionedUnitsBehindSelector(uint32_t selector) const { return m_selectorsWhoseUnitsAreMentioned[selector].load(std::memory_order_acquire); }
    void didMentionUnitsBehindSelector(uint32_t selector) { m_selectorsWhoseUnitsAreMentioned[selector].store(true, std::memory_order_release); }
    const Vector<Typed::CodeUnit>* unitsThatAskAmongMentionedUnitsOfClass(LayoutID classLayout) const { return m_unitsThatAskAmongMentionedUnitsOfClasses[classLayout.index()].load(std::memory_order_acquire); }
    JS_EXPORT_PRIVATE void didMentionUnitsOfClass(LayoutID, Vector<Typed::CodeUnit>&& thoseThatAskWhoMentions);
    JS_EXPORT_PRIVATE void addSeeds(std::span<const Typed::CodeUnit> lightestFirst, const ParallelLoops&);

    static constexpr size_t mostUnitsTakenAtOnce = 32;
    JS_EXPORT_PRIVATE static Vector<uint32_t> inRunsOfUnitQueue(std::span<const uint32_t> lightestFirst);
    JS_EXPORT_PRIVATE std::optional<Typed::CodeUnit> take(unsigned thread);
    JS_EXPORT_PRIVATE void didFinish(unsigned thread);

    bool isMentioned(uint32_t unitIndex) const { return m_isMentioned[unitIndex / 32].load(std::memory_order_relaxed) >> (unitIndex % 32) & 1; }
    JS_EXPORT_PRIVATE bool leadsTo(FunctionID referrer, FunctionID function) const;
    JS_EXPORT_PRIVATE Vector<Typed::CodeUnit> mentionedFusedCallEntries() const;

private:
    struct UnitsOfThread {
        Vector<uint32_t> units WTF_GUARDED_BY_LOCK(lock);
        // units.size(), for whoever looks without the lock.
        std::atomic<uint32_t> numberOfUnits { 0 };
        uint32_t takenUnit { 0 };
        uint32_t cycleOfTakenUnit { 0 };
        uint32_t takenUnitStandsOutsideCycles { 0 };
        Lock lock;
        uint8_t restOfCacheLines[128 - sizeof(Vector<uint32_t>) - 4 * sizeof(uint32_t) - sizeof(Lock)];
    };
    static_assert(sizeof(UnitsOfThread) == 128);

    enum class CycleState : uint8_t { Waiting, Ready, ReadyForSlowVersions, ReadyForClosedEntries, Done };
    struct Cycle {
        std::atomic<uint32_t> numberOfCyclesAwaited;
        std::atomic<uint32_t> numberOfOpenUnits;
        std::atomic<CycleState> state;
    };
    struct UnitBehindRoles {
        uint32_t function { 0 };
        uint32_t unit { 0 };
    };

    UnitQueue(const Typed::Program&, const Typed::CallingConventions&, const Typed::LinkTables*, Typed::Arrivals*, unsigned numberOfThreads);

    uint32_t cycleOf(FunctionID function) const { return m_cycleOfFunction.isEmpty() ? 0 : m_cycleOfFunction[function.index()]; }
    bool isInQueueFromStart(FunctionID) const;
    bool standsOutsideCycles(Typed::CodeUnit unit) const { return m_link && m_link->standsOutsideCycles(unit); }
    void orderCycles();
    std::span<const uint32_t> cyclesNamedBy(uint32_t cycle) const LIFETIME_BOUND;
    bool cycleLeadsTo(uint32_t referrer, uint32_t named) const;
    bool isSeedThatWaitsForNobody(Typed::CodeUnit) const;
    void checkMentionByTakenUnit(unsigned thread, Typed::CodeUnit) const;
    void push(unsigned thread, uint32_t unitIndex, Cycle*);
    std::optional<uint32_t> takeNewestUnitOf(UnitsOfThread&);
    bool takeRunOfSharedUnits(unsigned thread);
    bool takeOlderHalfOfAnotherThread(unsigned thread);
    bool waitForUnits(unsigned thread);
    bool someThreadHoldsUnits() const;
    void wakeThreadUnlessOneIsLooking();
    bool beginCycle(unsigned thread, uint32_t cycle);
    bool beginSlowVersionsOfCycle(unsigned thread, uint32_t cycle);
    bool beginClosedEntriesOfCycle(unsigned thread, uint32_t cycle);
    bool isEntryThatMayBeClosed(Typed::CodeUnit unit) const { return m_link && !m_program.admitsInspector && Typed::isGenericEntry(unit.role) && m_link->entryByValueMayBeClosed(unit.function); }
    void mentionEntriesOf(unsigned thread, FunctionID);
    static constexpr uint8_t wordIsNamed = 1;
    static constexpr uint8_t isEnteredByValue = 2;
    void finishCycle(unsigned thread, uint32_t cycle);

    const Typed::Program& m_program;
    const Typed::CallingConventions& m_conventions;
    const Typed::LinkTables* m_link { nullptr };
    Typed::Arrivals* m_arrivals { nullptr };
    Vector<uint32_t> m_cycleOfFunction;
    Vector<Cycle> m_cycles;
    Vector<uint32_t> m_firstMemberOfCycle;
    Vector<FunctionID> m_membersOfCycles;
    Vector<UnitBehindRoles> m_unitsBehindRoles;
    Vector<uint32_t> m_cyclesReadyFromStart;
    Vector<std::atomic<uint32_t>> m_isMentioned;
    Vector<std::atomic<uint8_t>> m_whatIsKnownOfEntries;
    Vector<std::atomic<bool>> m_selectorsWhoseUnitsAreMentioned;
    Vector<std::atomic<const Vector<Typed::CodeUnit>*>> m_unitsThatAskAmongMentionedUnitsOfClasses;
    Vector<uint32_t> m_isSeed;
    Vector<UnitsOfThread> m_unitsOfThreads;
    bool m_isTakingSeeds { false };
    Lock m_lock;
    Condition m_didChange;
    Vector<uint32_t> m_sharedUnits WTF_GUARDED_BY_LOCK(m_lock);
    // m_sharedUnits.size(), for whoever looks without the lock.
    std::atomic<size_t> m_numberOfSharedUnits { 0 };
    std::atomic<uint32_t> m_numberOfThreadsLooking { 0 };
    std::atomic<uint32_t> m_numberOfThreadsAsleep { 0 };
    std::atomic<uint64_t> m_numberOfOpenUnits { 0 };
};

JS_EXPORT_PRIVATE void forEachUnitMentionedBy(const CompiledCode&, const Typed::CompileInputs&, NOESCAPE const ScopedLambda<void(Typed::CodeUnit)>&);
JS_EXPORT_PRIVATE void forEachUnitMentionedBy(const CompiledCode&, const Typed::CompileInputs&, NOESCAPE const ScopedLambda<void(Typed::CodeUnit, MentionKind)>&);
JS_EXPORT_PRIVATE void mentionUnitNamedOutsideCode(UnitQueue&, unsigned thread, Typed::CodeUnit);
JS_EXPORT_PRIVATE void mentionWhatCodeNames(UnitQueue&, unsigned thread, const CompiledCode&, const Typed::CompileInputs&);

struct UseOfCompileThreads {
    static constexpr size_t numberOfLongestUnits = 10;
    static constexpr size_t numberOfParts = 20;

    struct Unit {
        Typed::CodeUnit unit;
        Seconds begin;
        Seconds duration;
        uint64_t instructionsOfWalkAndSweep { 0 };
        uint64_t instructionsOfEmitToEncode { 0 };
    };

    unsigned numberOfThreads { 0 };
    Seconds wall;
    Seconds busy;
    unsigned mostThreadsBusyAtOnce { 0 };
    std::array<double, numberOfParts> threadsBusyInPart { };
    Vector<Unit> longestUnits;
};

struct CompiledProgram {
    LinkedFunctions functions;
    WTF::BitSet<numberOfEntries> entriesUsed;
    Vector<Typed::WalkNotes> notes;
    uint64_t instructionsOfWalkAndSweep { 0 };
    uint64_t instructionsOfEmitToEncode { 0 };
    UseOfCompileThreads useOfThreads;
    bool succeeded { true };
};

JS_EXPORT_PRIVATE CompiledProgram compileQueuedUnits(const Typed::CompileInputs&, Typed::TypeTable&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
