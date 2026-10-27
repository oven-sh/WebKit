/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClaimChecks.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTCodeGenerator.h"
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

void emitReportOfBrokenClaim(CCallHelpers& jit, BrokenClaim claim, Typed::FunctionID function, Typed::ValueID value, int32_t offset, GPRReg found, GPRReg given)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    constexpr int32_t bytesForCallee = 32;
    // ARM64 keeps the stack pointer aligned, and its AND cannot read it.
    if constexpr (!isARM64())
        jit.andPtr(CCallHelpers::TrustedImm32(-16), CCallHelpers::stackPointerRegister);
    jit.subPtr(CCallHelpers::TrustedImm32(bytesForCallee + 2 * static_cast<int32_t>(sizeof(uint64_t))), CCallHelpers::stackPointerRegister);
    for (unsigned i = 0; i < 2; ++i) {
        CCallHelpers::Address word(CCallHelpers::stackPointerRegister, bytesForCallee + static_cast<int32_t>(i * sizeof(uint64_t)));
        if (GPRReg reg = i ? given : found; reg != InvalidGPRReg)
            jit.store64(reg, word);
        else
            jit.store64(CCallHelpers::TrustedImm64(0), word);
    }
    jit.loadPtr(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(runtimeTable)), GPRInfo::nonArgGPR0);
    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(static_cast<uint32_t>(claim) | static_cast<uint32_t>(offset) << 8)), GPRInfo::argumentGPR0);
    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(function.index())), GPRInfo::argumentGPR1);
    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(value.index())), GPRInfo::argumentGPR2);
    jit.addPtr(CCallHelpers::TrustedImm32(bytesForCallee), CCallHelpers::stackPointerRegister, GPRInfo::argumentGPR3);
    jit.call(CCallHelpers::Address(GPRInfo::nonArgGPR0, offsetInRuntimeTable(Entry::operationAOTReportBrokenClaim)), OperationPtrTag);
    jit.breakpoint();
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

static RefPtr<B3::DifferenceReport> reportOf(BrokenClaim claim, Typed::FunctionID function)
{
    return createSharedTask<void(CCallHelpers&, const B3::MovedLoad&, GPRReg, GPRReg)>([claim, function](CCallHelpers& jit, const B3::MovedLoad& load, GPRReg found, GPRReg given) {
        emitReportOfBrokenClaim(jit, claim, function, Typed::valueOfOrigin(load.origin), load.offset, found, given);
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

void checkClaimsAtRunTime(B3::Policy& policy, Typed::FunctionID function, const StackSlots& slotsOfCells, bool entersOnlyAtRoot)
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
    policy.clientPhases.append(reportReadsOfUnwrittenStackSlots(function, slotsOfCells, entersOnlyAtRoot));
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

void checkMovedLoadsAtRunTime(B3::Policy& policy, Typed::FunctionID function, const std::unique_ptr<B3::Procedure>& procedure, const GuardTests& guardTests)
{
    policy.validation.logsMovedLoads = true;
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
        if (isIn(heaps.entire(Typed::AbstractHeap::MemberGuards)))
            return saysWhatCountsOfGuardByte(guardTests, moved);
        return saysWhatCountsOfUnchangingWord(heaps, load, moved);
    });
    policy.clientPhases.append(B3::ClientPhase {
        B3::ClientPhase::Point::AfterSinking,
        "checkMovedLoadsAtRunTime"_s,
        createSharedTask<bool(B3::Procedure&)>([function](B3::Procedure& proc) {
            return proc.movedLoadLog().insertChecks(proc, reportOf(BrokenClaim::LoadMayMove, function));
        })
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
