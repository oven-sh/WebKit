/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFaultHandler.h"

#if ENABLE(AOT)

#include "AOTCodeSites.h"
#include "AOTFaultTable.h"
#include "AOTImage.h"
#include "AOTImageFormat.h"
#include "AOTInstance.h"
#include "AOTInstanceLayout.h"
#include "AOTOperationData.h"
#include "AOTStubDescriptor.h"
#include "AOTStubKey.h"
#include "AOTStubRegisters.h"
#include "AOTStubsExceptions.h"
#include "AOTStubsOperations.h"
#include "AOTStubsPolls.h"
#include "AOTUnwinding.h"
#include "MachineContext.h"
#include <optional>
#include <wtf/threads/Signals.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

struct Fault {
    const Image& image;
    uintptr_t pc;
    uintptr_t address;
    CallerFrame callerFrame;
};

} // anonymous namespace

#if CPU(ARM64)
static uintptr_t& registerOf(PlatformRegisters& registers, GPRReg reg)
{
    RELEASE_ASSERT(reg == faultCallerLinkGPR || reg == ARM64Registers::lr || reg == cellCageBaseGPR || reg == instanceGPR);
    unsigned number = static_cast<unsigned>(reg);
#if OS(DARWIN)
    return reg == ARM64Registers::lr ? reinterpret_cast<uintptr_t&>(registers.__lr) : reinterpret_cast<uintptr_t&>(registers.__x[number]);
#elif OS(WINDOWS)
    return reinterpret_cast<uintptr_t&>(registers.X[number]);
#elif OS(FREEBSD)
    return reg == ARM64Registers::lr ? reinterpret_cast<uintptr_t&>(registers.machineContext.mc_gpregs.gp_lr) : reinterpret_cast<uintptr_t&>(registers.machineContext.mc_gpregs.gp_x[number]);
#else
    return reinterpret_cast<uintptr_t&>(registers.machineContext.regs[number]);
#endif
}
#elif CPU(X86_64)
static uintptr_t& scratchRegisterOf(PlatformRegisters& registers)
{
    static_assert(MacroAssembler::s_scratchRegister == X86Registers::r11);
#if OS(DARWIN)
    return reinterpret_cast<uintptr_t&>(registers.__r11);
#elif OS(WINDOWS)
    return reinterpret_cast<uintptr_t&>(registers.R11);
#elif OS(FREEBSD)
    return reinterpret_cast<uintptr_t&>(registers.machineContext.mc_r11);
#else
    return reinterpret_cast<uintptr_t&>(registers.machineContext.gregs[REG_R11]);
#endif
}

static thread_local uint64_t s_scratchRegisterBeforeFault;

uint64_t scratchRegisterBeforeFault()
{
    return s_scratchRegisterBeforeFault;
}
#endif

static uintptr_t cellCageBaseIn(PlatformRegisters& registers)
{
#if CPU(ARM64)
    return registerOf(registers, cellCageBaseGPR);
#elif CPU(X86_64)
    static_assert(cellCageBaseGPR == X86Registers::r14);
#if OS(DARWIN)
    return registers.__r14;
#elif OS(WINDOWS)
    return registers.R14;
#elif OS(FREEBSD)
    return registers.machineContext.mc_r14;
#else
    return registers.machineContext.gregs[REG_R14];
#endif
#endif
}

static uintptr_t instanceIn(PlatformRegisters& registers)
{
#if CPU(ARM64)
    return registerOf(registers, instanceGPR);
#elif CPU(X86_64)
    static_assert(instanceGPR == X86Registers::r13);
#if OS(DARWIN)
    return registers.__r13;
#elif OS(WINDOWS)
    return registers.R13;
#elif OS(FREEBSD)
    return registers.machineContext.mc_r13;
#else
    return registers.machineContext.gregs[REG_R13];
#endif
#endif
}

static void* findStub(const Image& image, StubKey key)
{
    if (!exists(key))
        return nullptr;
    return const_cast<void*>(image.stubAddress(key));
}

static SignalAction handleNullCheck(const Fault& fault, PlatformRegisters& registers)
{
    // A null 32-bit reference decodes to the base of the cell cage.
    if (fault.address >= FaultSite::reach && fault.address - cellCageBaseIn(registers) >= FaultSite::reach)
        return SignalAction::NotHandled;
    void* target = findStub(fault.image, throwAtSite(ThrowSiteSource::FaultSite, fault.callerFrame));
    if (!target)
        return SignalAction::NotHandled;

    // The call-site table and ThrowAtSite treat the faulting instruction as a near call, whatever its length.
    uintptr_t returnAddress = fault.pc + sizeOfNearCall;
#if CPU(ARM64)
    if (fault.callerFrame == CallerFrame::Frameless)
        registerOf(registers, faultCallerLinkGPR) = registerOf(registers, ARM64Registers::lr);
    registerOf(registers, ARM64Registers::lr) = returnAddress;
#elif CPU(X86_64)
    s_scratchRegisterBeforeFault = scratchRegisterOf(registers);
    scratchRegisterOf(registers) = returnAddress;
#endif
    MachineContext::setInstructionPointer(registers, target);
    return SignalAction::Handled;
}

#if CPU(X86_64) && OS(LINUX)
static SignalAction handleLoopPoll(const Fault& fault, PlatformRegisters& registers)
{
    static_assert(hasFaultingLoopPolls);
    static_assert(instanceGPR == X86Registers::r13);
    bool isFrameless = fault.callerFrame == CallerFrame::Frameless;
    uintptr_t instance = static_cast<uintptr_t>(registers.machineContext.gregs[REG_R13]);
    if (fault.address != instance + InstanceLayout::offsetOfLoopPollPage() + (isFrameless ? offsetOfLeafPollInLoopPollPage : 0))
        return SignalAction::NotHandled;
    void* target = findStub(fault.image, pollAtLoop(fault.callerFrame));
    if (!target)
        return SignalAction::NotHandled;

    void*& stackPointer = MachineContext::stackPointerImpl(registers);
    auto* top = static_cast<uintptr_t*>(stackPointer);
    if (isFrameless) {
        void*& framePointer = MachineContext::framePointerImpl(registers);
        *--top = std::bit_cast<uintptr_t>(framePointer);
        framePointer = top;
    }
    *--top = fault.pc + sizeOfLoopPoll;
    stackPointer = top;
    MachineContext::setInstructionPointer(registers, target);
    return SignalAction::Handled;
}
#endif

static SignalAction handleDivertedReturn(const Image& image, PlatformRegisters& registers)
{
    void* target = findStub(image, handleException(false));
    if (!target)
        return SignalAction::NotHandled;
    uintptr_t returnAddress = std::bit_cast<uintptr_t>(std::bit_cast<Instance*>(instanceIn(registers))->runtime.exceptions.stackPointerOfHandler);
#if CPU(ARM64)
    registerOf(registers, ARM64Registers::lr) = returnAddress;
#elif CPU(X86_64)
    void*& stackPointer = MachineContext::stackPointerImpl(registers);
    auto* top = static_cast<uintptr_t*>(stackPointer);
    *--top = returnAddress;
    stackPointer = top;
#endif
    MachineContext::setInstructionPointer(registers, target);
    return SignalAction::Handled;
}

void installFaultHandler()
{
    addSignalHandler(Signal::AccessFault, [](Signal, SigInfo& info, PlatformRegisters& registers) {
        auto instructionPointer = MachineContext::instructionPointer(registers);
        if (!instructionPointer)
            return SignalAction::NotHandled;
        const Image* image = Image::current();
        if (!image)
            return SignalAction::NotHandled;
        void* pc = instructionPointer->untaggedPtr();
        if (std::bit_cast<uintptr_t>(pc) == instanceIn(registers) + InstanceLayout::offsetOfLoopPollPage() + offsetOfDivertedReturnInLoopPollPage) [[unlikely]]
            return handleDivertedReturn(*image, registers);
        std::optional<FaultSite> site = image->faultSiteAt(pc);
        if (!site)
            return SignalAction::NotHandled;

        Fault fault {
            .image = *image,
            .pc = std::bit_cast<uintptr_t>(pc),
            .address = std::bit_cast<uintptr_t>(info.faultingAddress),
            .callerFrame = site->hasFrame ? CallerFrame::HasFrame : CallerFrame::Frameless,
        };
        switch (site->kind) {
        case FaultSiteKind::NullCheck:
            return handleNullCheck(fault, registers);
        case FaultSiteKind::LoopPoll:
#if CPU(X86_64) && OS(LINUX)
            return handleLoopPoll(fault, registers);
#else
            static_assert(!hasFaultingLoopPolls);
            return SignalAction::NotHandled;
#endif
        }
        return SignalAction::NotHandled;
    });
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
