/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStackSlotPoisoning.h"

#if ENABLE(AOT)

#include "AOTClaimChecks.h"
#include "AOTCodeGenerator.h"
#include "AirStackSlot.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3BasicBlockInlines.h"
#include "B3Const64Value.h"
#include "B3InsertionSetInlines.h"
#include "B3MemoryValueInlines.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3SlotBaseValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"
#include <wtf/SharedTask.h>

namespace JSC { namespace AOT {

// No cell lies at this address, no code does, and as a value it is a number that no computation of a test arrives at. Both halves are the same, so a read of either is known.
static constexpr uint32_t unwrittenHalfWord = 0xbadbeef5;
static constexpr uint64_t unwrittenWord = static_cast<uint64_t>(unwrittenHalfWord) << 32 | unwrittenHalfWord;

namespace {

struct UnwrittenWordCheck {
    void generate(CCallHelpers&, const B3::StackmapGenerationParams&) const;

    int32_t offset;
    bool readsWholeWord;
    Typed::FunctionID function;
    Typed::ValueID value;
};

} // anonymous namespace

void UnwrittenWordCheck::generate(CCallHelpers& jit, const B3::StackmapGenerationParams& params) const
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    GPRReg base = params[0].gpr();
    CCallHelpers::Jump mayBeUnwritten = jit.branch32(CCallHelpers::Equal, CCallHelpers::Address(base, offset), CCallHelpers::TrustedImm32(static_cast<int32_t>(unwrittenHalfWord)));
    CCallHelpers::Label done = jit.label();
    params.addLatePath([check = *this, base, mayBeUnwritten, done](CCallHelpers& jit) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        mayBeUnwritten.link(&jit);
        if (check.readsWholeWord)
            jit.branch32(CCallHelpers::NotEqual, CCallHelpers::Address(base, check.offset + 4), CCallHelpers::TrustedImm32(static_cast<int32_t>(unwrittenHalfWord))).linkTo(done, &jit);
        emitReportOfBrokenClaim(jit, BrokenClaim::StackSlotIsWrittenBeforeRead, check.function, check.value);
    });
}

static bool fillStackSlotsAtEntry(B3::Procedure& proc)
{
    if (!proc.stackSlots().size())
        return false;
    B3::BasicBlock* root = proc.at(0);
    B3::InsertionSet insertionSet(proc);
    B3::Origin origin = root->at(0)->origin();
    B3::Value* word = insertionSet.insert<B3::Const64Value>(0, origin, static_cast<int64_t>(unwrittenWord));
    for (B3::Air::StackSlot* slot : proc.stackSlots()) {
        B3::Value* base = insertionSet.insert<B3::SlotBaseValue>(0, origin, slot);
        for (unsigned offset = 0; offset + sizeof(uint64_t) <= slot->byteSize(); offset += sizeof(uint64_t))
            insertionSet.insert<B3::MemoryValue>(0, B3::Store, origin, word, base, static_cast<int32_t>(offset));
    }
    insertionSet.execute(root);
    return true;
}

static std::optional<bool> readsWholeWord(const B3::MemoryValue* memory)
{
    if (memory->opcode() != B3::Load || memory->hasFence())
        return std::nullopt;
    switch (memory->accessByteSize()) {
    case 4:
        return false;
    case 8:
        return true;
    default:
        return std::nullopt;
    }
}

static bool checkLoads(B3::Procedure& proc, Typed::FunctionID function)
{
    bool changed = false;
    B3::InsertionSet insertionSet(proc);
    for (B3::BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            auto* memory = block->at(index)->as<B3::MemoryValue>();
            if (!memory)
                continue;
            std::optional<bool> wholeWord = readsWholeWord(memory);
            if (!wholeWord)
                continue;
            // Behind the load: one that is allowed to fault has faulted by then.
            auto* test = insertionSet.insert<B3::PatchpointValue>(index + 1, B3::Void, memory->origin());
            test->append(B3::ConstrainedValue(memory->lastChild(), B3::ValueRep::SomeRegister));
            test->clobber(RegisterSet::macroClobberedGPRs());
            test->effects = B3::Effects::none();
            test->effects.writesLocalState = true;
            test->setRunsWithoutFrame(true);
            UnwrittenWordCheck check { memory->offset(), *wholeWord, function, Typed::valueOfOrigin(memory->origin()) };
            test->setGenerator([check](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
                check.generate(jit, params);
            });
            changed = true;
        }
        insertionSet.execute(block);
    }
    return changed;
}

B3::ClientPhase reportReadsOfUnwrittenStackSlots(Typed::FunctionID function, bool entersOnlyAtRoot)
{
    return {
        B3::ClientPhase::Point::AfterSinking,
        "reportReadsOfUnwrittenStackSlots"_s,
        createSharedTask<bool(B3::Procedure&)>([function, entersOnlyAtRoot](B3::Procedure& proc) {
            bool changed = checkLoads(proc, function);
            if (entersOnlyAtRoot)
                changed |= fillStackSlotsAtEntry(proc);
            return changed;
        })
    };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
