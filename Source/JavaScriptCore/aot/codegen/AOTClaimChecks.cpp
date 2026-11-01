/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClaimChecks.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCodeGenerator.h"
#include "AOTCompactTable.h"
#include "AOTConvention.h"
#include "AOTHeaps.h"
#include "AOTInstance.h"
#include "AOTPrimitiveEmitter.h"
#include "AOTRuntimeTable.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3BasicBlockInlines.h"
#include "B3CheckValue.h"
#include "B3InsertionSetInlines.h"
#include "B3IntRange.h"
#include "B3MemoryValueInlines.h"
#include "B3MovedLoadCheck.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"
#include <algorithm>
#include <wtf/IndexMap.h>
#include <wtf/SharedTask.h>

namespace JSC { namespace AOT {

static void forEachRegisterKeptRoundReport(NOESCAPE const Invocable<void(GPRReg, int32_t)> auto& forGPR, NOESCAPE const Invocable<void(FPRReg, int32_t)> auto& forFPR)
{
    int32_t offset = 0;
    for (auto reg = CCallHelpers::firstRegister(); reg <= CCallHelpers::lastRegister(); reg = static_cast<GPRReg>(reg + 1)) {
        if (reg != CCallHelpers::stackPointerRegister)
            forGPR(reg, offset);
        offset += sizeof(uint64_t);
    }
    offset = WTF::roundUpToMultipleOf<16>(offset);
    for (auto reg = CCallHelpers::firstFPRegister(); reg <= CCallHelpers::lastFPRegister(); reg = static_cast<FPRReg>(reg + 1)) {
        forFPR(reg, offset);
        offset += 16;
    }
}

static constexpr int32_t bytesOfRegistersKeptRoundReport = WTF::roundUpToMultipleOf<16>((CCallHelpers::lastRegister() - CCallHelpers::firstRegister() + 1) * static_cast<int32_t>(sizeof(uint64_t))) + (CCallHelpers::lastFPRegister() - CCallHelpers::firstFPRegister() + 1) * 16;
// A function without a frame may have words below the stack pointer.
static constexpr int32_t bytesLeftAloneBelowStackPointer = 128;

void emitReportOfBrokenClaim(CCallHelpers& jit, BrokenClaim claim, Typed::FunctionID function, Typed::ValueID value, int32_t offset, GPRReg found, GPRReg given, ReportOfBrokenClaim report)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    constexpr int32_t bytesForCallee = 32;
    bool comesBack = report == ReportOfBrokenClaim::ComesBack;
    constexpr int32_t offsetOfStackPointer = bytesForCallee + 2 * static_cast<int32_t>(sizeof(uint64_t));
    GPRReg stackPointerOfRegisters = InvalidGPRReg;
    if (comesBack) {
        jit.subPtr(CCallHelpers::TrustedImm32(bytesLeftAloneBelowStackPointer + bytesOfRegistersKeptRoundReport), CCallHelpers::stackPointerRegister);
        forEachRegisterKeptRoundReport([&](GPRReg reg, int32_t at) {
            jit.store64(reg, CCallHelpers::Address(CCallHelpers::stackPointerRegister, at));
        }, [&](FPRReg reg, int32_t at) {
            jit.storeVector(reg, CCallHelpers::Address(CCallHelpers::stackPointerRegister, at));
        });
        for (GPRReg candidate : { GPRInfo::regT0, GPRInfo::regT1, GPRInfo::regT2, GPRInfo::regT3 }) {
            if (candidate != found && candidate != given && candidate != instanceGPR)
                stackPointerOfRegisters = candidate;
        }
        jit.move(CCallHelpers::stackPointerRegister, stackPointerOfRegisters);
    }
    // ARM64 keeps the stack pointer aligned, and its AND cannot read it.
    if constexpr (!isARM64())
        jit.andPtr(CCallHelpers::TrustedImm32(-16), CCallHelpers::stackPointerRegister);
    jit.subPtr(CCallHelpers::TrustedImm32(bytesForCallee + 4 * static_cast<int32_t>(sizeof(uint64_t))), CCallHelpers::stackPointerRegister);
    if (comesBack)
        jit.store64(stackPointerOfRegisters, CCallHelpers::Address(CCallHelpers::stackPointerRegister, offsetOfStackPointer));
    for (unsigned i = 0; i < 2; ++i) {
        CCallHelpers::Address word(CCallHelpers::stackPointerRegister, bytesForCallee + static_cast<int32_t>(i * sizeof(uint64_t)));
        if (GPRReg reg = i ? given : found; reg != InvalidGPRReg)
            jit.store64(reg, word);
        else
            jit.store64(CCallHelpers::TrustedImm64(0), word);
    }
    jit.loadPtr(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(runtimeTable)), GPRInfo::nonArgGPR0);
    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(static_cast<uint32_t>(claim) | (comesBack ? reportOfBrokenClaimComesBack : 0) | static_cast<uint32_t>(offset) << 8)), GPRInfo::argumentGPR0);
    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(function.index())), GPRInfo::argumentGPR1);
    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(value.index())), GPRInfo::argumentGPR2);
    jit.addPtr(CCallHelpers::TrustedImm32(bytesForCallee), CCallHelpers::stackPointerRegister, GPRInfo::argumentGPR3);
    jit.call(CCallHelpers::Address(GPRInfo::nonArgGPR0, offsetInRuntimeTable(Entry::operationAOTReportBrokenClaim)), OperationPtrTag);
    if (!comesBack) {
        jit.breakpoint();
        return;
    }
    jit.loadPtr(CCallHelpers::Address(CCallHelpers::stackPointerRegister, offsetOfStackPointer), GPRInfo::regT0);
    jit.move(GPRInfo::regT0, CCallHelpers::stackPointerRegister);
    forEachRegisterKeptRoundReport([&](GPRReg reg, int32_t at) {
        jit.load64(CCallHelpers::Address(CCallHelpers::stackPointerRegister, at), reg);
    }, [&](FPRReg reg, int32_t at) {
        jit.loadVector(CCallHelpers::Address(CCallHelpers::stackPointerRegister, at), reg);
    });
    jit.addPtr(CCallHelpers::TrustedImm32(bytesLeftAloneBelowStackPointer + bytesOfRegistersKeptRoundReport), CCallHelpers::stackPointerRegister);
}

static bool checkKnownRanges(B3::Procedure& proc, Typed::FunctionID function)
{
    bool changed = false;
    B3::InsertionSet insertionSet(proc);
    for (B3::BasicBlock* block : proc) {
        for (unsigned index = 0; index + 1 < block->size(); ++index) {
            B3::Value* value = block->at(index);
            std::optional<B3::IntRange> range = proc.knownRange(value);
            if (!range || *range == B3::IntRange::top(value->type()))
                continue;
            B3::Origin origin = value->origin();
            B3::Value* least = insertionSet.insertIntConstant(index + 1, origin, value->type(), range->min());
            B3::Value* width = insertionSet.insertIntConstant(index + 1, origin, value->type(), static_cast<int64_t>(static_cast<uint64_t>(range->max()) - static_cast<uint64_t>(range->min())));
            B3::Value* distance = insertionSet.insert<B3::Value>(index + 1, B3::Sub, origin, value, least);
            B3::Value* isOutside = insertionSet.insert<B3::Value>(index + 1, B3::Above, origin, distance, width);
            auto* check = insertionSet.insert<B3::CheckValue>(index + 1, B3::Check, origin, isOutside);
            check->setRunsWithoutFrame(true);
            check->setGenerator([function, origin](CCallHelpers& jit, const B3::StackmapGenerationParams&) {
                emitReportOfBrokenClaim(jit, BrokenClaim::ValueIsInRange, function, Typed::valueOfOrigin(origin));
            });
            changed = true;
        }
        insertionSet.execute(block);
    }
    return changed;
}

static RefPtr<B3::DifferenceReport> reportOf(BrokenClaim claim, Typed::FunctionID function, ReportOfBrokenClaim report = ReportOfBrokenClaim::EndsRun)
{
    return createSharedTask<void(CCallHelpers&, const B3::MovedLoad&, GPRReg, GPRReg)>([claim, function, report](CCallHelpers& jit, const B3::MovedLoad& load, GPRReg found, GPRReg given) {
        emitReportOfBrokenClaim(jit, claim, function, Typed::valueOfOrigin(load.origin), load.offset, found, given, report);
    });
}

// False as told, and true of what every test of the word answers: the type of a cell changes in place, by the rows of cellTypeChangesInPlace() alone. Where a compiled object has its shape word
// other cells have other things, half of a pointer for one, and none of those has compiledObjectShapeBit (AOTCellFormat.h).
static bool saysWhatCountsOfUnchangingWord(AOTHeaps& heaps, const B3::MemoryValue& load, B3::MovedLoad& moved)
{
    if (load.range() == heaps.rangeOf(heaps.JSCell_typeInfoType))
        moved.changesThatDoNotCount = Typed::cellTypeChangesInPlace();
    if (load.range() == heaps.rangeOf(heaps.AOTObject_shapeWord)) {
        if (load.opcode() != B3::Load || load.offset() != offsetOfShapeWord)
            return false;
        moved.bitsOfWordsThatCount = compiledObjectShapeBit;
    }
    if (load.range() == heaps.rangeOf(heaps.AOTInstance_fieldsFilledOnce))
        moved.mayGetFilled = true;
    return true;
}

// The text of a string lies outside the heap and goes when the cell of the string goes. Its address keeps no cell alive. What was read from it stays right, and nothing reads it
// for the program once the string is out of use, so behind an operation that may collect there may be no word left to read again.
class OwnedMemory {
public:
    OwnedMemory(B3::Procedure& proc, const AOTHeaps& heaps)
        : m_proc(proc)
        , m_heaps(heaps)
    {
    }

    bool mayBeFreedBetween(B3::MemoryValue* load, B3::BasicBlock* block, unsigned index)
    {
        findPlaces();
        Place read = m_reads.get(load);
        RELEASE_ASSERT(read.block);
        if (read.block == block)
            return collectsBetween(block, read.index, index);
        follow(read);
        return m_isEnteredBehindCollection[block] || collectsBetween(block, 0, index);
    }

private:
    struct Place {
        B3::BasicBlock* block { nullptr };
        unsigned index { 0 };
    };

    void findPlaces()
    {
        if (std::exchange(m_hasPlaces, true))
            return;
        m_collections.resize(m_proc.size());
        m_isEnteredBehindCollection.resize(m_proc.size());
        m_isEnteredBeforeCollection.resize(m_proc.size());
        for (B3::BasicBlock* block : m_proc) {
            for (unsigned index = 0; index < block->size(); ++index) {
                B3::Value* value = block->at(index);
                if (auto* load = value->as<B3::MemoryValue>(); load && load->isLoad() && m_heaps.isFreedWithOwner(load->range()))
                    m_reads.add(load, Place { block, index });
                if ((value->opcode() == B3::Patchpoint || value->opcode() == B3::CCall) && m_heaps.mayCollect(value->effects()))
                    m_collections[block].append(index);
            }
        }
    }

    // From `begin` on, in front of `end`.
    bool collectsBetween(B3::BasicBlock* block, unsigned begin, unsigned end) const
    {
        for (unsigned index : m_collections[block]) {
            if (index >= begin)
                return index < end;
        }
        return false;
    }

    // The read runs again wherever its block is entered again.
    void follow(Place read)
    {
        unsigned collectionsBehindRead = 0;
        for (unsigned index : m_collections[read.block])
            collectionsBehindRead += index > read.index;
        if (m_followed.block == read.block && m_collectionsBehindFollowed == collectionsBehindRead)
            return;
        m_followed = read;
        m_collectionsBehindFollowed = collectionsBehindRead;
        for (B3::BasicBlock* block : m_proc) {
            m_isEnteredBehindCollection[block] = false;
            m_isEnteredBeforeCollection[block] = false;
        }

        Vector<B3::BasicBlock*, 16> behind;
        Vector<B3::BasicBlock*, 16> before;
        auto leave = [&](B3::BasicBlock* block, bool hasCollected) {
            auto& isEntered = hasCollected ? m_isEnteredBehindCollection : m_isEnteredBeforeCollection;
            for (B3::BasicBlock* successor : block->successorBlocks()) {
                if (successor == read.block || std::exchange(isEntered[successor], true))
                    continue;
                (hasCollected ? behind : before).append(successor);
            }
        };
        leave(read.block, !!collectionsBehindRead);
        while (!before.isEmpty()) {
            B3::BasicBlock* block = before.takeLast();
            leave(block, !m_collections[block].isEmpty());
        }
        while (!behind.isEmpty())
            leave(behind.takeLast(), true);
    }

    B3::Procedure& m_proc;
    const AOTHeaps& m_heaps;
    bool m_hasPlaces { false };
    UncheckedKeyHashMap<B3::Value*, Place> m_reads;
    IndexMap<B3::BasicBlock*, Vector<unsigned>> m_collections;
    Place m_followed;
    unsigned m_collectionsBehindFollowed { 0 };
    IndexMap<B3::BasicBlock*, bool> m_isEnteredBehindCollection;
    IndexMap<B3::BasicBlock*, bool> m_isEnteredBeforeCollection;
};

// A word that never changes still holds, wherever what was read from it is used, what was read. The check keeps the address alive, and with it the cell that the word lies in.
static bool readUnchangingWordsAgainAtUses(B3::Procedure& proc, Typed::FunctionID function)
{
    bool changed = false;
    RefPtr<B3::DifferenceReport> report = reportOf(BrokenClaim::WordNeverChanges, function);
    AOTHeaps& heaps = AOTHeaps::of(proc);
    OwnedMemory ownedMemory(proc, heaps);
    B3::InsertionSet insertionSet(proc);
    for (B3::BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            B3::Value* user = block->at(index);
            B3::Value* lastChecked = nullptr;
            for (B3::Value* child : user->children()) {
                auto* load = child->as<B3::MemoryValue>();
                if (!load || load == lastChecked || !load->isLoad() || load->readsMutability() != B3::Mutability::Immutable)
                    continue;
                std::optional<B3::MovedLoad> described = B3::MovedLoad::describe(load);
                if (!described || !saysWhatCountsOfUnchangingWord(heaps, *load, *described))
                    continue;
                if (heaps.isFreedWithOwner(load->range()) && ownedMemory.mayBeFreedBetween(load, block, index))
                    continue;
                insertionSet.insertValue(index, B3::createMovedLoadCheck(proc, *described, load->lastChild(), load, report));
                lastChecked = load;
                changed = true;
            }
        }
        insertionSet.execute(block);
    }
    return changed;
}

void reportBrokenKnownProperties(B3::Policy& policy, Typed::FunctionID function)
{
    policy.reportsBrokenKnownProperty = createSharedTask<void(CCallHelpers&, B3::KnownProperty, B3::Origin)>([function](CCallHelpers& jit, B3::KnownProperty property, B3::Origin origin) {
        emitReportOfBrokenClaim(jit, property == B3::KnownProperty::Range ? BrokenClaim::ValueIsInRange : BrokenClaim::CounterStepsAsTold, function, Typed::valueOfOrigin(origin));
    });
}

void checkClaimsAtRunTime(B3::Policy& policy, Typed::FunctionID function, const SlotsOfCells& slotsOfCells)
{
    policy.clientPhases.append(B3::ClientPhase {
        B3::ClientPhase::Point::AfterSinking,
        "checkClaimsAtRunTime"_s,
        createSharedTask<bool(B3::Procedure&)>([function](B3::Procedure& proc) {
            bool changed = checkKnownRanges(proc, function);
            changed |= readUnchangingWordsAgainAtUses(proc, function);
            return changed;
        })
    });
    policy.clientPhases.append(reportReadsOfUnwrittenStackSlots(function, slotsOfCells));
}

// Only the bit that the operation asks for, and only that the guard has ended: see GuardEmitter::holds().
static bool saysWhatCountsOfGuardByte(const GuardTests& tests, B3::MovedLoad& moved)
{
    const GuardTest* found = nullptr;
    unsigned numberOfTests = 0;
    for (const GuardTest& test : tests) {
        if (test.operation != Typed::valueOfOrigin(moved.origin))
            continue;
        ++numberOfTests;
        if (!found || test.offset == moved.offset)
            found = &test;
    }
    if (!found || (numberOfTests > 1 && found->offset != moved.offset))
        return false;
    moved.bitsThatCount = found->mask;
    (found->holdsWhileClear ? moved.bitsThatMayGetCleared : moved.bitsThatMayGetSet) = found->mask;
    return true;
}

void checkMovedLoadsAtRunTime(B3::Policy& policy, Typed::FunctionID function, const std::unique_ptr<B3::Procedure>& procedure, const GuardTests& guardTests, WhatIsLogged logged)
{
    policy.validation.logsMovedLoads = logged.loads;
    policy.validation.logsDecidedComparisons = logged.decidedComparisons;
    policy.validation.continuesAfterMovedLoadReports = logged.reportsComeBack;
    ReportOfBrokenClaim report = logged.reportsComeBack ? ReportOfBrokenClaim::ComesBack : ReportOfBrokenClaim::EndsRun;
    // The vectorizer's report ends a block: that one does not come back.
    policy.reportsMovedLoad = reportOf(BrokenClaim::LoadMayMove, function);
    policy.saysWhatCountsOfMovedLoad = createSharedTask<bool(const B3::MemoryValue&, B3::MovedLoad&)>([&procedure, &guardTests](const B3::MemoryValue& load, B3::MovedLoad& moved) {
        AOTHeaps& heaps = AOTHeaps::of(*procedure);
        auto isIn = [&](const B3::AbstractHeap& heap) {
            return load.range().overlaps(heaps.rangeOf(heap));
        };
        // The collector's threads write the first. A rope that is resolved changes a word of the second, and nothing that an operation answers.
        if (isIn(heaps.collectorState) || isIn(heaps.entire(Typed::AbstractHeap::StringStorage)))
            return false;
        // Whoever finds the word of a string's or a key's cell empty asks for the cell, and what makes the cell fills the word and is called a pure function. A read from before says "empty", and the code asks again.
        if (isIn(heaps.AOTInstance_stringCells)) {
            moved.bitsThatMayGetSet = std::numeric_limits<uint64_t>::max();
            return true;
        }
        // Whoever finds the word that a getter of the host keeps its answer in empty calls the getter, which fills the word and is said to write nothing. A read from before says "empty", and the code calls again.
        if (load.range() == heaps.rangeOf(heaps.hostObjectCachedValues)) {
            moved.mayGetFilled = true;
            return true;
        }
        if (isIn(heaps.entire(Typed::AbstractHeap::MemberGuards)))
            return saysWhatCountsOfGuardByte(guardTests, moved);
        // A walk that has taken an entry marks the table of the Map or the Set, and writes the mark under a heap of its own so that reads of the byte may move across it. Only clear() asks for the mark, in the run time.
        // What compiled code asks of the byte, or of the word that holds it, are the bits beside the mark; and whoever finds the mark clear sets it.
        if (isIn(heaps.entire(Typed::AbstractHeap::CollectionStorage)) || isIn(heaps.entire(Typed::AbstractHeap::AllocatorState))) {
            constexpr unsigned shiftInHeaderWord = (CompactTable::offsetOfKeysShift() - CompactTable::offsetOfCapacity()) * 8;
            if ((load.opcode() == B3::Load8Z || load.opcode() == B3::Load8S) && load.offset() == CompactTable::offsetOfKeysShift())
                moved.bitsThatMayGetSet = CompactTable::wasWalked;
            else if (load.opcode() == B3::Load && load.type() == B3::Int64 && load.offset() == CompactTable::offsetOfCapacity())
                moved.bitsThatMayGetSet = static_cast<uint64_t>(CompactTable::wasWalked) << shiftInHeaderWord;
        }
        return saysWhatCountsOfUnchangingWord(heaps, load, moved);
    });
    policy.clientPhases.append(B3::ClientPhase {
        B3::ClientPhase::Point::AfterSinking,
        "checkMovedLoadsAtRunTime"_s,
        createSharedTask<bool(B3::Procedure&)>([function, report](B3::Procedure& proc) {
            return proc.movedLoadLog().insertChecks(proc, reportOf(BrokenClaim::LoadMayMove, function, report), reportOf(BrokenClaim::ComparisonIsDecided, function, report));
        })
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
