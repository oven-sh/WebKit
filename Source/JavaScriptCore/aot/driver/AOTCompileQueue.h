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
class LeavePoints;
struct LinkTables;
}

// Compile as a stage. What happens to one unit on one thread is walk/AOTCompileUnit.h: Typed::CompileInputs, and Typed::CompileWorker with the thread's arenas.
// This header is the queue, its closure, and what is left when it is empty.

// The units that finished code has mentioned and that have no code yet. It starts with unitsNamedOutsideCode().
// What gets code is the closure of the mentions, so neither the order in which units are taken nor the number of threads changes it.
// A unit that nothing mentions gets no code: a function that every caller inlined, an entry that nobody enters.
//
// CALLERS FIRST. The queue works by the cycles of Link (LinkedFunction::cycle). A unit goes with the cycle of CodeUnit::function, entries and bound functions too.
//   Waiting   some other cycle that holds a referrer of a member is not done. A mention of a member sets its bit and nothing else.
//   Ready     all of them are done. The members that have their bit come into the queue, and so does at once each member that is mentioned from here on.
//   Done      all of them are done, and no mentioned member is in the queue or being compiled. Nobody is left who could name the others: they are dropped, and the cycles that this one may name wait for one fewer.
// So a function that takes what was left is walked behind all who may name it, and Arrivals holds what every one of them has left.
// SLOW VERSIONS LAST. A SlowBody has an entry for each point at which a fast version is left for it (walk/AOTLeavePoints.h), so it is made behind every unit that may note one: the units of its own cycle and of the cycles that lead there.
// A mention of a SlowBody sets its bit and nothing else until no other unit of its cycle is open and all the cycles that the cycle awaits are done. Then the points of the members are closed and the slow versions that have their bit come into the queue.
// The cycle is done behind them. So what a slow version leaves for its callees is there when they are walked, like what any other unit leaves.
// What a slow version is the first to mention in its own cycle comes at once, and finds the points closed.
// ONLY WHOEVER READS WAITS. A cycle none of whose members takes what was left (Arrivals::takesWhatWasLeft(): no member of a cycle of several does) is ready from the start: its code is the same whenever it is made.
// EVERY MENTION BY FINISHED CODE THAT MAY BRING THE BODY OF A FUNCTION IS HELD AGAINST THE TABLES, the first of a unit and each one after it: leadsTo().
// Of a function with LinkedFunction::isInQueueFromStart every unit whose code may name another function is among the seeds, so whoever mentions a unit of it needs no place in the order. The one exception has a place: CallingConventions::isSeedOfFunctionInQueueFromStart().
// A Serializer is listed under a function that it is no part of. It reads nothing that was left and leaves nothing, and its code names no unit but serializers. So it stands outside the cycles, and whoever mentions it needs no place in the order.
// Where the tables do not lead from the function of the code to the function that it mentions, Link lacks a referrer,
// and the process ends there, whatever the states of the two cycles happen to be. So a build stops with any number of threads or with none.
// Of the tables the queue reads the cycle of each function and the order of cycles (LinkTables::cyclesAwaited), and no list of any function. A cycle that holds groups alone has no unit: it is done once all that it awaits are.
//
// WHAT THE QUEUE NEEDS OF LinkTables::referrers. Each function whose finished code may bring a unit of F here, by forEachUnitMentionedBy(), leads to F from node to node.
// Then the cycle of F waits while that code is made. Besides a call, a NewClosure and the evaluation of a class, such code holds:
//   Variable, CardOfVariable                     a read or a store of a variable that is born as F
//   ClosureFunction, ClosureCell, ClassRank,     F is the constructor, or a member with MemberCreation::ByClassRecord, of the class that is named or that the closure constructs,
//   a Request of NewClass                        or of a class above it
//   Allocation                                   F is PlacedAllocation::constructedBy, or PlacedSlot::deferredFunction of a slot of the layout
//   MethodSelector                               the family lists F, or F is the function of tableLookupOf()
//   ModuleValue, RequireEdge                     F is the body of the module
//   Function                                     the unit is a bound function of F
class UnitQueue {
    WTF_MAKE_NONCOPYABLE(UnitQueue);
public:
    // Every function is in one cycle, which is ready from the start: nothing is ordered, and a unit comes into the queue when it is first mentioned.
    JS_EXPORT_PRIVATE UnitQueue(const Typed::CallingConventions&, unsigned numberOfThreads);
    // By the cycles of the tables. With the tables at their defaults it is the queue above. addSeeds() is called once, and nobody takes a unit before.
    JS_EXPORT_PRIVATE UnitQueue(const Typed::CallingConventions&, const Typed::LinkTables&, Typed::Arrivals&, unsigned numberOfThreads);
    JS_EXPORT_PRIVATE ~UnitQueue();

    // Before addSeeds(). Without it nobody notes a point.
    void closePointsIn(Typed::LeavePoints& points) { m_leavePoints = &points; }

    // By any thread. The unit exists in the plan: CallingConventions::exists(). Only the first mention of a unit does anything.
    JS_EXPORT_PRIVATE void mention(unsigned thread, Typed::CodeUnit);
    // Before any thread takes a unit, by the thread that may start a loop. The loops are no wider than the queue has threads. The seeds come lightest first, and each thread mentions a share of them as mentionUnitNamedOutsideCode() does.
    // No thread keeps any: all that came into the queue goes to the shared list in the order of the seeds, as inRunsOfUnitQueue() orders it. A seed need not come into the queue, so the runs are made of what did.
    // Then the cycles that were ready from the start and hold no seed are done, and so on down. What that brings into the queue is taken behind the runs.
    JS_EXPORT_PRIVATE void addSeeds(std::span<const Typed::CodeUnit> lightestFirst, const ParallelLoops&);

    // take() hands a thread no more units of the shared list at once, and always the newest of the list.
    static constexpr size_t mostUnitsTakenAtOnce = 32;
    // A thread takes a run off the end of the shared list, and of what it holds the newest first. So the heaviest stand one to a run, as the newest of it, and the others fill the runs up:
    // as many of the heaviest are begun at once as there are threads.
    JS_EXPORT_PRIVATE static Vector<uint32_t> inRunsOfUnitQueue(std::span<const uint32_t> lightestFirst);
    // The next unit for the thread. Nothing once every mentioned unit is finished. It waits while another thread is at a unit and may mention more.
    // A thread keeps what it mentions and takes the newest of it. One that holds nothing takes a run of the shared list, and then the older half of what another thread holds.
    // There is no lock that all threads take for a unit. One that finds nothing looks on for as long as it would take to sleep and be woken, and then sleeps.
    // Whoever has more than it takes next wakes a sleeper, unless somebody is looking: that one finds it, and wakes the next if it got more than one.
    JS_EXPORT_PRIVATE std::optional<Typed::CodeUnit> take(unsigned thread);
    // Behind the last mention on behalf of the unit that take() gave.
    JS_EXPORT_PRIVATE void didFinish(unsigned thread);

    bool isMentioned(uint32_t unitIndex) const { return m_isMentioned[unitIndex / 32].load(std::memory_order_relaxed) >> (unitIndex % 32) & 1; }
    // LinkTables::referrers leads from the one to the other, from node to node: they are in one cycle, or the cycle of `function` stays Waiting until that of `referrer` is done. It follows from the tables alone.
    JS_EXPORT_PRIVATE bool leadsTo(FunctionID referrer, FunctionID function) const;
    // Once take() has answered nothing. Ascending. A fused call entry is never in the queue: a mention of it is a mention of its target.
    JS_EXPORT_PRIVATE Vector<Typed::CodeUnit> mentionedFusedCallEntries() const;

private:
    // Padded with bytes and not with alignas(): the buffer of a Vector is aligned as malloc() aligns, whatever the type of its elements asks for.
    struct UnitsOfThread {
        Vector<uint32_t> units WTF_GUARDED_BY_LOCK(lock);
        // units.size(), for whoever looks without the lock.
        std::atomic<uint32_t> numberOfUnits { 0 };
        // What take() gave last. The thread's own.
        uint32_t takenUnit { 0 };
        uint32_t cycleOfTakenUnit { 0 };
        uint32_t takenUnitStandsOutsideCycles { 0 };
        // Its thread takes it for each unit, another thread only where numberOfUnits says that there is something to take.
        Lock lock;
        uint8_t restOfCacheLines[128 - sizeof(Vector<uint32_t>) - 4 * sizeof(uint32_t) - sizeof(Lock)];
    };
    static_assert(sizeof(UnitsOfThread) == 128);

    enum class CycleState : uint8_t { Waiting, Ready, ReadyForSlowVersions, Done };
    struct Cycle {
        // The other cycles that hold a referrer of a member and are not done.
        std::atomic<uint32_t> numberOfCyclesAwaited;
        // Its units that are in the queue or being compiled, and one more while somebody still brings in the seeds or the members that had their bit, or while a cycle that is ready awaits another.
        std::atomic<uint32_t> numberOfOpenUnits;
        std::atomic<CycleState> state;
    };
    // A unit of a bound function or of the method table. Its number says nothing of its function.
    struct UnitBehindRoles {
        uint32_t function { 0 };
        uint32_t unit { 0 };
    };

    UnitQueue(const Typed::CallingConventions&, const Typed::LinkTables*, Typed::Arrivals*, unsigned numberOfThreads);

    uint32_t cycleOf(FunctionID function) const { return m_cycleOfFunction.isEmpty() ? 0 : m_cycleOfFunction[function.index()]; }
    bool isInQueueFromStart(FunctionID) const;
    // It names nothing but units of its own function, which are among the seeds or such units themselves. So it may come when the cycle of the function is done, and no cycle counts it.
    bool standsOutsideCycles(Typed::CodeUnit unit) const { return Typed::namesOnlyUnitsOfItsFunction(unit.role) && isInQueueFromStart(unit.function); }
    void orderCycles();
    std::span<const uint32_t> cyclesNamedBy(uint32_t cycle) const LIFETIME_BOUND;
    bool cycleLeadsTo(uint32_t referrer, uint32_t named) const;
    // In front of whatever a mention does. Nothing is asked of a seed.
    void checkMentionByTakenUnit(unsigned thread, Typed::CodeUnit) const;
    // Null: standsOutsideCycles().
    void push(unsigned thread, uint32_t unitIndex, Cycle*);
    std::optional<uint32_t> takeNewestUnitOf(UnitsOfThread&);
    // Each is false where there was nothing to take. What is taken is the thread's own from then on.
    bool takeRunOfSharedUnits(unsigned thread);
    bool takeOlderHalfOfAnotherThread(unsigned thread);
    // False: every mentioned unit is finished.
    bool waitForUnits(unsigned thread);
    bool someThreadHoldsUnits() const;
    void wakeThreadUnlessOneIsLooking();
    // False: no member has its bit, so the cycle is done already.
    bool beginCycle(unsigned thread, uint32_t cycle);
    // False: no slow version of the cycle has its bit.
    bool beginSlowVersionsOfCycle(unsigned thread, uint32_t cycle);
    // No unit of the cycle is open any more.
    void finishCycle(unsigned thread, uint32_t cycle);

    const Typed::CallingConventions& m_conventions;
    // Null: every function is in cycle 0.
    const Typed::LinkTables* m_link { nullptr };
    Typed::Arrivals* m_arrivals { nullptr };
    Typed::LeavePoints* m_leavePoints { nullptr };
    // [function]. Empty: every function is in cycle 0.
    Vector<uint32_t> m_cycleOfFunction;
    Vector<Cycle> m_cycles;
    // By cycle, with one more entry than there are cycles: the functions among its members.
    Vector<uint32_t> m_firstMemberOfCycle;
    Vector<FunctionID> m_membersOfCycles;
    // Ascending by function.
    Vector<UnitBehindRoles> m_unitsBehindRoles;
    // addSeeds() lets go of them.
    Vector<uint32_t> m_cyclesReadyFromStart;
    // [unitIndex / 32]
    Vector<std::atomic<uint32_t>> m_isMentioned;
    Vector<UnitsOfThread> m_unitsOfThreads;
    // Nobody is woken meanwhile.
    bool m_isTakingSeeds { false };
    // Whoever goes to sleep holds it from the count until it is in the queue of the condition, and whoever wakes takes it in between.
    Lock m_lock;
    Condition m_didChange;
    // What came into the queue with the seeds.
    Vector<uint32_t> m_sharedUnits WTF_GUARDED_BY_LOCK(m_lock);
    // m_sharedUnits.size(), for whoever looks without the lock.
    std::atomic<size_t> m_numberOfSharedUnits { 0 };
    // Threads that hold no unit.
    std::atomic<uint32_t> m_numberOfThreadsLooking { 0 };
    std::atomic<uint32_t> m_numberOfThreadsAsleep { 0 };
    // Pushed and not yet finished.
    std::atomic<uint64_t> m_numberOfOpenUnits { 0 };
};

// Every unit that has to have code because this code exists: the callees that its relocations of SymbolSpace::Function name, and forEachUnitBehind() each of its mentions.
// A callee that only an inline frame or a site names is not among them.
JS_EXPORT_PRIVATE void forEachUnitMentionedBy(const CompiledCode&, const Typed::CompileInputs&, NOESCAPE const ScopedLambda<void(Typed::CodeUnit)>&);
JS_EXPORT_PRIVATE void forEachUnitMentionedBy(const CompiledCode&, const Typed::CompileInputs&, NOESCAPE const ScopedLambda<void(Typed::CodeUnit, MentionKind)>&);
// What the stage tells the queue: of unitsNamedOutsideCode(), and of finished code.
JS_EXPORT_PRIVATE void mentionUnitNamedOutsideCode(UnitQueue&, unsigned thread, Typed::CodeUnit);
JS_EXPORT_PRIVATE void mentionWhatCodeNames(UnitQueue&, unsigned thread, const CompiledCode&, const Typed::CompileInputs&);

// How the threads of the stage were used. Filled where the build logs its phase times.
struct UseOfCompileThreads {
    static constexpr size_t numberOfLongestUnits = 10;
    static constexpr size_t numberOfParts = 20;

    struct Unit {
        Typed::CodeUnit unit;
        // From the moment the first thread asked for a unit.
        Seconds begin;
        Seconds duration;
        uint64_t instructionsOfWalkAndSweep { 0 };
        uint64_t instructionsOfEmitToEncode { 0 };
    };

    unsigned numberOfThreads { 0 };
    // Until the last unit was finished.
    Seconds wall;
    // Of all threads, at units.
    Seconds busy;
    unsigned mostThreadsBusyAtOnce { 0 };
    // The wall in equal parts: how many threads were at a unit, on average.
    std::array<double, numberOfParts> threadsBusyInPart { };
    // Longest first.
    Vector<Unit> longestUnits;
};

struct CompiledProgram {
    LinkedFunctions functions;
    WTF::BitSet<numberOfEntries> entriesUsed;
    // Ascending by unit. Empty unless WalkOptions::takesNotes.
    Vector<Typed::WalkNotes> notes;
    // What all threads have retired inside walkAndSweep() and inside generateCode(). Zero unless the build logs its phase times and the threads have counters.
    // AOTDriver.cpp prints them. The stage prints nothing.
    uint64_t instructionsOfWalkAndSweep { 0 };
    uint64_t instructionsOfEmitToEncode { 0 };
    UseOfCompileThreads useOfThreads;
    bool succeeded { true };
};

// The stage. As many threads as the build has, each with a CompileWorker, until the queue is empty. Then the code of all threads in the order of LinkedFunctions.
// A unit that fails leaves the others to be compiled: one build reports every unit that fails.
// All of them leave what they have proven in WalkInputs::arrivals. What an earlier run of the stage has left there changes nothing: the same is left again.
JS_EXPORT_PRIVATE CompiledProgram compileQueuedUnits(const Typed::CompileInputs&, Typed::TypeTable&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
