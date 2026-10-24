/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClaimChecks.h"

#if ENABLE(AOT)

#include "AOTCodeGenerator.h"
#include "AOTConvention.h"
#include "AOTHeaps.h"
#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTStackSlotPoisoning.h"
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
#include <wtf/SharedTask.h>

namespace JSC { namespace AOT {

void emitReportOfBrokenClaim(CCallHelpers& jit, BrokenClaim claim, Typed::FunctionID function, Typed::ValueID value)
{
    jit.andPtr(CCallHelpers::TrustedImm32(-16), CCallHelpers::stackPointerRegister);
    jit.subPtr(CCallHelpers::TrustedImm32(32), CCallHelpers::stackPointerRegister);
    jit.loadPtr(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(runtimeTable)), GPRInfo::nonArgGPR0);
    jit.move(instanceGPR, GPRInfo::argumentGPR0);
    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(claim)), GPRInfo::argumentGPR1);
    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(function.index())), GPRInfo::argumentGPR2);
    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(value.index())), GPRInfo::argumentGPR3);
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
    return createSharedTask<void(CCallHelpers&, B3::Origin)>([claim, function](CCallHelpers& jit, B3::Origin origin) {
        emitReportOfBrokenClaim(jit, claim, function, Typed::valueOfOrigin(origin));
    });
}

// A word that never changes still holds, wherever what was read from it is used, what was read. The check keeps the address alive, and with it the cell.
static bool readUnchangingWordsAgainAtUses(B3::Procedure& proc, Typed::FunctionID function)
{
    bool changed = false;
    RefPtr<B3::DifferenceReport> report = reportOf(BrokenClaim::WordNeverChanges, function);
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
                insertionSet.insertValue(index, B3::createMovedLoadCheck(proc, *described, load->lastChild(), load, report));
                lastChecked = load;
                changed = true;
            }
        }
        insertionSet.execute(block);
    }
    return changed;
}

void checkClaimsAtRunTime(B3::Policy& policy, Typed::FunctionID function, bool entersOnlyAtRoot)
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
    policy.clientPhases.append(reportReadsOfUnwrittenStackSlots(function, entersOnlyAtRoot));
}

void checkMovedLoadsAtRunTime(B3::Policy& policy, Typed::FunctionID function, const std::unique_ptr<B3::Procedure>& procedure)
{
    policy.validation.checksMovedLoadsAtRunTime = true;
    policy.reportMovedLoadThatDiffers = reportOf(BrokenClaim::LoadMayMove, function);
    policy.isWrittenBehindTheBackOfCode = createSharedTask<bool(const B3::HeapRange&)>([&procedure](const B3::HeapRange& range) {
        AOTHeaps& heaps = AOTHeaps::of(*procedure);
        return range.overlaps(heaps.rangeOf(heaps.collectorState));
    });
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
