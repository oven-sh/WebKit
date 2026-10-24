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
#include "AOTRuntimeTable.h"
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
#include <wtf/SharedTask.h>

namespace JSC { namespace AOT {

void emitReportOfBrokenClaim(CCallHelpers& jit, BrokenClaim claim, Typed::FunctionID function, Typed::ValueID value, int32_t offset, GPRReg found, GPRReg given)
{
    constexpr int32_t bytesForCallee = 32;
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

// A word that never changes still holds, wherever what was read from it is used, what was read. The check keeps the address alive, and with it the cell.
static bool readUnchangingWordsAgainAtUses(B3::Procedure& proc, Typed::FunctionID function)
{
    bool changed = false;
    RefPtr<B3::DifferenceReport> report = reportOf(BrokenClaim::WordNeverChanges, function);
    AOTHeaps& heaps = AOTHeaps::of(proc);
    B3::HeapRange shapeWords = heaps.rangeOf(heaps.AOTObject_shapeWord);
    // A slim closure that gets properties gets the type beside its own.
    B3::HeapRange cellTypes = heaps.rangeOf(heaps.JSCell_typeInfoType);
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
                if (!described)
                    continue;
                // In a cell that is no compiled object other things lie there, half of a pointer for one. They change, and what every test of the word answers does not.
                if (load->range() == cellTypes)
                    continue;
                if (load->range() == shapeWords) {
                    if (load->opcode() != B3::Load || load->offset() != offsetOfShapeWord)
                        continue;
                    described->bitsOfWordsThatCount = compiledObjectShapeBit;
                }
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

void checkMovedLoadsAtRunTime(B3::Policy& policy, Typed::FunctionID function, const std::unique_ptr<B3::Procedure>& procedure)
{
    policy.validation.checksMovedLoadsAtRunTime = true;
    policy.reportMovedLoadThatDiffers = reportOf(BrokenClaim::LoadMayMove, function);
    policy.isWrittenBehindTheBackOfCode = createSharedTask<bool(const B3::HeapRange&)>([&procedure](const B3::HeapRange& range) {
        AOTHeaps& heaps = AOTHeaps::of(*procedure);
        // A rope that is resolved, a slim closure that gets properties, a class that its first allocation defines: each changes a word, and none what a test of the word answers or may answer.
        const B3::AbstractHeap* heapsThatChangeUnseen[] = { &heaps.collectorState, &heaps.AOTObject_shapeWord, &heaps.JSCell_typeInfoType, &heaps.entire(Typed::AbstractHeap::StringStorage), &heaps.entire(Typed::AbstractHeap::MemberGuards) };
        return std::ranges::any_of(heapsThatChangeUnseen, [&](const B3::AbstractHeap* heap) {
            return range.overlaps(heaps.rangeOf(*heap));
        });
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
