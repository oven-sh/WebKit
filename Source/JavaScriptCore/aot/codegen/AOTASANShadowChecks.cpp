/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTASANShadowChecks.h"

#if ENABLE(AOT)

#include <wtf/SharedTask.h>

#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
#include "AOTConvention.h"
#include "AOTFaultTable.h"
#include "AOTInstance.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3BasicBlockInlines.h"
#include "B3InsertionSetInlines.h"
#include "B3MemoryValueInlines.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "CCallHelpers.h"
#include <limits>
#endif

namespace JSC { namespace AOT {

#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
namespace {

constexpr int32_t shadowOffset = 0x7fff8000;
constexpr int32_t redZoneSize = 128;

struct ShadowCheck {
    void generate(CCallHelpers&, const B3::StackmapGenerationParams&) const;
    void generateLatePath(CCallHelpers&, GPRReg base, GPRReg shadow, GPRReg last, CCallHelpers::Label done) const;

    int32_t offset;
    int32_t size;
    bool isStore;
    bool mayFault;
};

} // anonymous namespace

static_assert(FaultSite::reach <= static_cast<uintptr_t>(std::numeric_limits<int32_t>::max()));

static bool isFrameAddress(const B3::Value* pointer)
{
    return pointer->opcode() == B3::SlotBase || pointer->opcode() == B3::FramePointer;
}

static void emitReport(CCallHelpers& jit, unsigned size, bool isStore, unsigned bytesPushedByCheck)
{
    jit.move(CCallHelpers::TrustedImm32(size | isStore << 8), GPRInfo::argumentGPR2);
    jit.addPtr(CCallHelpers::TrustedImm32(bytesPushedByCheck), CCallHelpers::stackPointerRegister, GPRInfo::argumentGPR3);
    jit.andPtr(CCallHelpers::TrustedImm32(-16), CCallHelpers::stackPointerRegister);
    jit.move(instanceGPR, GPRInfo::argumentGPR0);
    jit.loadPtr(CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(runtimeTable)), GPRInfo::argumentGPR4);
    jit.call(CCallHelpers::Address(GPRInfo::argumentGPR4, offsetInRuntimeTable(Entry::operationAOTReportPoisonedAccess)), OperationPtrTag);
    jit.breakpoint();
}

void ShadowCheck::generate(CCallHelpers& jit, const B3::StackmapGenerationParams& params) const
{
    GPRReg base = params[0].gpr();
    GPRReg shadow = params.gpScratch(0);
    GPRReg last = params.gpScratch(1);
    jit.addPtr(CCallHelpers::TrustedImm32(offset), base, shadow);
    CCallHelpers::Jump faults;
    if (mayFault)
        faults = jit.branch64(CCallHelpers::Below, shadow, CCallHelpers::TrustedImm32(static_cast<int32_t>(FaultSite::reach)));
    jit.urshiftPtr(CCallHelpers::TrustedImm32(3), shadow);
    CCallHelpers::Jump mayBePoisoned = jit.branchTest8(CCallHelpers::NonZero, CCallHelpers::Address(shadow, shadowOffset));
    if (mayFault)
        faults.link(&jit);
    CCallHelpers::Label done = jit.label();
    params.addLatePath([check = *this, base, shadow, last, mayBePoisoned, done](CCallHelpers& jit) {
        mayBePoisoned.link(&jit);
        check.generateLatePath(jit, base, shadow, last, done);
    });
}

void ShadowCheck::generateLatePath(CCallHelpers& jit, GPRReg base, GPRReg shadow, GPRReg last, CCallHelpers::Label done) const
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    if (size < 8) {
        jit.load8SignedExtendTo32(CCallHelpers::Address(shadow, shadowOffset), shadow);
        jit.addPtr(CCallHelpers::TrustedImm32(offset), base, last);
        jit.and32(CCallHelpers::TrustedImm32(7), last);
        jit.add32(CCallHelpers::TrustedImm32(size - 1), last);
        jit.branch32(CCallHelpers::LessThan, last, shadow).linkTo(done, &jit);
    }
    jit.addPtr(CCallHelpers::TrustedImm32(offset), base, GPRInfo::argumentGPR1);
    emitReport(jit, size, isStore, 0);
    // Never runs. The report reads from it where the access is.
    jit.jump().linkTo(done, &jit);
}

static void emitEffectiveAddress(CCallHelpers& jit, CCallHelpers::Address address, GPRReg result)
{
    jit.addPtr(CCallHelpers::TrustedImm32(address.offset), address.base, result);
}

static void emitEffectiveAddress(CCallHelpers& jit, CCallHelpers::BaseIndex address, GPRReg result)
{
    jit.getEffectiveAddress(address, result);
}

static GPRReg scratchOtherThan(GPRReg first, GPRReg second, GPRReg third)
{
    for (GPRReg candidate : { GPRInfo::regT0, GPRInfo::regT1, GPRInfo::regT2, GPRInfo::regT3 }) {
        if (candidate != first && candidate != second && candidate != third)
            return candidate;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void pushFlags(CCallHelpers& jit) { jit.m_assembler.buffer().putByte(static_cast<int8_t>(0x9c)); }
static void popFlags(CCallHelpers& jit) { jit.m_assembler.buffer().putByte(static_cast<int8_t>(0x9d)); }

// A word that is popped keeps what it held, and a conservative collector reads it once another frame lies over it.
static void popAndClear(CCallHelpers& jit, GPRReg reg)
{
    jit.load64(CCallHelpers::Address(CCallHelpers::stackPointerRegister), reg);
    jit.store64(CCallHelpers::TrustedImm32(0), CCallHelpers::Address(CCallHelpers::stackPointerRegister));
    jit.addPtrNoFlags(CCallHelpers::TrustedImm32(sizeof(void*)), CCallHelpers::stackPointerRegister);
}

template<typename AddressType>
static void emitStandaloneCheck(CCallHelpers& jit, AddressType address, GPRReg index, unsigned size, bool isStore)
{
    if (address.base == CCallHelpers::stackPointerRegister || address.base == CCallHelpers::framePointerRegister)
        return;
    if (size > 8) {
        RELEASE_ASSERT(!(size % 8));
        for (unsigned offset = 0; offset < size; offset += 8)
            emitStandaloneCheck(jit, address.withOffset(offset), index, 8, isStore);
        return;
    }

    GPRReg shadow = scratchOtherThan(address.base, index, InvalidGPRReg);
    GPRReg last = scratchOtherThan(address.base, index, shadow);
    jit.addPtrNoFlags(CCallHelpers::TrustedImm32(-redZoneSize), CCallHelpers::stackPointerRegister);
    pushFlags(jit);
    jit.push(shadow);
    emitEffectiveAddress(jit, address, shadow);
    jit.urshiftPtr(CCallHelpers::TrustedImm32(3), shadow);
    CCallHelpers::Jump isAddressable = jit.branchTest8(CCallHelpers::Zero, CCallHelpers::Address(shadow, shadowOffset));
    CCallHelpers::Jump isPartiallyAddressable;
    if (size < 8) {
        jit.push(last);
        jit.load8SignedExtendTo32(CCallHelpers::Address(shadow, shadowOffset), shadow);
        emitEffectiveAddress(jit, address, last);
        jit.and32(CCallHelpers::TrustedImm32(7), last);
        jit.add32(CCallHelpers::TrustedImm32(size - 1), last);
        isPartiallyAddressable = jit.branch32(CCallHelpers::LessThan, last, shadow);
    }
    emitEffectiveAddress(jit, address, GPRInfo::argumentGPR1);
    emitReport(jit, size, isStore, redZoneSize + (size < 8 ? 3 : 2) * sizeof(void*));
    if (size < 8) {
        isPartiallyAddressable.link(&jit);
        popAndClear(jit, last);
    }
    isAddressable.link(&jit);
    popAndClear(jit, shadow);
    popFlags(jit);
    jit.addPtrNoFlags(CCallHelpers::TrustedImm32(redZoneSize), CCallHelpers::stackPointerRegister);
}

static bool insertShadowChecks(B3::Procedure& proc)
{
    bool changed = false;
    B3::InsertionSet insertionSet(proc);
    for (B3::BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            auto* memory = block->at(index)->as<B3::MemoryValue>();
            if (!memory)
                continue;
            B3::Value* pointer = memory->lastChild();
            if (isFrameAddress(pointer))
                continue;
            auto* test = insertionSet.insert<B3::PatchpointValue>(index, B3::Void, memory->origin());
            test->append(B3::ConstrainedValue(pointer, B3::ValueRep::SomeRegister));
            test->numGPScratchRegisters = 2;
            test->effects = B3::Effects::none();
            test->effects.exitsSideways = true;
            test->effects.reads = B3::HeapRange::top();
            test->setRunsWithoutFrame(true);
            ShadowCheck check { memory->offset(), static_cast<int32_t>(memory->accessByteSize()), memory->isStore(), memory->traps() };
            test->setGenerator([check](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
                check.generate(jit, params);
            });
            changed = true;
        }
        insertionSet.execute(block);
    }
    return changed;
}

void emitASANShadowCheck(CCallHelpers& jit, CCallHelpers::Address address, unsigned size, bool isStore)
{
    emitStandaloneCheck(jit, address, InvalidGPRReg, size, isStore);
}

void emitASANShadowCheck(CCallHelpers& jit, CCallHelpers::BaseIndex address, unsigned size, bool isStore)
{
    emitStandaloneCheck(jit, address, address.index, size, isStore);
}
#endif

B3::ClientPhase checkASANShadowBeforeAccesses()
{
    return {
        B3::ClientPhase::Point::AfterOptimization,
        "checkASANShadowBeforeAccesses"_s,
#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
        createSharedTask<bool(B3::Procedure&)>([](B3::Procedure& proc) { return insertShadowChecks(proc); })
#else
        createSharedTask<bool(B3::Procedure&)>([](B3::Procedure&) { return false; })
#endif
    };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
