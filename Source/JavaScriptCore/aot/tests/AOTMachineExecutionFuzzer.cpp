/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineExecutionFuzzer.h"

#if ENABLE(AOT) && OS(LINUX) && !ASAN_ENABLED

#include "AOTMachineBuilder.h"
#include "AOTMachineDescription.h"
#include "AOTMachineFrame.h"
#include "AOTMachineLiveness.h"
#include "AOTMachineOrder.h"
#include "AOTMachineTarget.h"
#include "AOTRelocation.h"
#include "AOTSavedFrame.h"
#include "AOTStubAssembler.h"
#include "AOTStubRegisters.h"
#include "AssemblerPolicy.h"
#include "CCallHelpers.h"
#include "CPUFeatureSet.h"
#include "ExecutableAllocator.h"
#include "ExecutableMemoryHandle.h"
#include "LinkBuffer.h"
#include <algorithm>
#include <array>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include <wtf/SetForScope.h>
#include <wtf/StringPrintStream.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace AOT {
namespace Machine {
namespace ExecutionFuzzer {

struct alignas(64) RegisterArea {
    uint64_t gprs[32];
    uint64_t fprs[32];
    uint64_t stackPointer;
    uint64_t successor;
    uint64_t entryGPRs[32], exitGPRs[32];
    uint64_t entryFPRs[32], exitFPRs[32];
    uint64_t hostStackPointer, exitStackPointer, target;
    uint64_t hostGPRs[32], hostFPRs[32];
};
static RegisterArea* registerArea;
static uint8_t* runtimeStackTop;
static uint8_t* programStackTop;
static uint8_t* programStackBase;
static constexpr size_t programStackSize = 1 << 20;
static uint8_t* memoryBuffer;
static uint8_t* testInstance;
static constexpr int32_t instanceRegionOffset = 20000;
static const unsigned regionOffsets[] = { 64, 320, 576, 832, 0, 0, 1088 };
static constexpr int32_t instanceWordsOffset = 24000;

static uint8_t* regionPointer(unsigned region) { return region == InstanceRegion ? testInstance + instanceRegionOffset : region == ArgumentsRegion ? programStackTop
        : region == InstanceWordsRegion                                                                                                            ? testInstance + instanceWordsOffset
                                                                                                                                                   : memoryBuffer + regionOffsets[region]; }

struct ProgramHeaps {
    HeapTree tree;
    HeapIndex fields, firstField, bytes, constants, instance, slot, arguments, data, sums, words;
    ProgramHeaps()
    {
        fields = tree.add(rootHeapIndex, "fields"_s);
        firstField = 0;
        for (unsigned i = 0; i < 16; ++i) {
            HeapIndex heap = tree.add(fields, "field"_s);
            if (!i)
                firstField = heap;
        }
        bytes = tree.add(rootHeapIndex, "bytes"_s);
        constants = tree.add(rootHeapIndex, "constants"_s);
        instance = tree.add(rootHeapIndex, "inst"_s);
        slot = tree.add(rootHeapIndex, "slot"_s);
        arguments = tree.add(rootHeapIndex, "args"_s);
        data = tree.add(rootHeapIndex, "data"_s);
        sums = tree.add(rootHeapIndex, "sums"_s);
        words = tree.add(rootHeapIndex, "words"_s);
    }
};
static ProgramHeaps* programHeaps;

struct PatchSite {
    const PatchDescription* description;
    int id;
    bool isCheck { false };
    Vector<ValueRep> representations;
    Vector<TypeKind> operandTypes;
    Vector<char> isLateOperand;
    RegisterMask earlyClobbers { 0 }, lateClobbers { 0 }, availableRegisters { 0 }, scratchRegisters { 0 };
    RegisterMask resultRegisters { 0 }, fixedResultRegisters { 0 }, earlyResultRegisters { 0 }, lateOperandRegisters { 0 }, broughtRegisters { 0 };
    RegisterMask registersClobberedOnThrow { 0 };
    bool isCall { false };
    bool leavesMoveToCallee { false };
    bool clearsUpperHalf { false };
    Reg filledRegister, registerMovedIntoResult;
    bool frameIsSetUp { false };
    bool hasFrameRecord { true };
    unsigned frameSize { 0 };
    unsigned argumentAreaSize { 0 };
    Vector<char> isDeadFrameWord;
    bool isSuspension { false };
    uint8_t tailPart { 0 };
};

struct ProgramRun {
    const Trace* expectedTrace { nullptr };
    size_t eventIndex { 0 };
    sigjmp_buf exitPoint;
    std::string error;
    bool didExit { false };
    uint64_t garbageState { 0x5bad5bad5bad5bad };
    bool isRunning { false };
    uint64_t slotAddress { 0 };
    uint64_t stackDistance { 0 };
    const uint8_t* codeStart { nullptr };
    size_t codeSize { 0 };
    const Vector<LinkedTrappingAccess>* trappingAccesses { nullptr };
};
static ProgramRun currentRun;

static std::string toStdString(const StringPrintStream& out)
{
    String string = out.toString();
    std::string otherString;
    for (unsigned i = 0; i < string.length(); ++i)
        otherString += static_cast<char>(string[i]);
    return otherString;
}

[[noreturn]] static void abortRun(const std::string& error)
{
    currentRun.error = error;
    siglongjmp(currentRun.exitPoint, 1);
}

static std::string toHex(uint64_t bits)
{
    char characters[40];
    snprintf(characters, sizeof characters, "0x%llx", static_cast<unsigned long long>(bits));
    return characters;
}

static uint64_t nextGarbage()
{
    currentRun.garbageState = currentRun.garbageState * 6364136223846793005ull + 1442695040888963407ull;
    return currentRun.garbageState ^ 0xdead000000000000ull;
}

static uint64_t& savedRegister(Reg reg) { return reg.isGPR() ? registerArea->gprs[reg.gpr()] : registerArea->fprs[reg.fpr()]; }

static uint64_t currentMemoryHash()
{
    Memory memory;
    for (unsigned choice = 0; choice < SlotRegion; ++choice)
        memcpy(memory.regions[choice], regionPointer(choice), regionSize);
    return hashOfMemory(memory);
}

static std::string findMemoryDifference(const Memory& expected)
{
    for (unsigned choice = 0; choice < SlotRegion; ++choice) {
        for (unsigned i = 0; i < regionSize; ++i) {
            if (regionPointer(choice)[i] != expected.regions[choice][i])
                return std::string(regionNames[choice]) + "+" + std::to_string(i);
        }
    }
    return "none";
}

extern "C" void didReachPatch(PatchSite* site)
{
    const PatchDescription& description = *site->description;
    RegisterArea& area = *registerArea;
    unsigned numberOfResults = site->isCheck ? 0 : static_cast<unsigned>(description.results.size());
    unsigned numberOfOperands = static_cast<unsigned>(description.operands.size());
    if (site->leavesMoveToCallee) {
        savedRegister(site->filledRegister) = area.gprs[GPRInfo::returnValueGPR];
        if (site->registerMovedIntoResult)
            area.gprs[GPRInfo::returnValueGPR] = savedRegister(site->registerMovedIntoResult);
    }
    auto read = [&](unsigned i) -> uint64_t {
        const ValueRep& representation = site->representations[numberOfResults + i];
        bool narrow = site->operandTypes[i] == Int32 || site->operandTypes[i] == Float;
        uint64_t bits = 0;
        switch (representation.kind()) {
        case ValueRep::Register:
            bits = savedRegister(representation.reg());
            break;
        case ValueRep::Stack: {
            uint8_t* otherBytes;
            if (site->hasFrameRecord)
                otherBytes = reinterpret_cast<uint8_t*>(area.gprs[GPRInfo::callFrameRegister]) + representation.offsetFromFP();
            else
                otherBytes = reinterpret_cast<uint8_t*>(area.stackPointer) + representation.offsetFromFP() + site->frameSize;
            memcpy(&bits, otherBytes, narrow ? 4 : 8);
            break;
        }
        case ValueRep::StackArgument:
            memcpy(&bits, reinterpret_cast<uint8_t*>(area.stackPointer) + representation.offsetFromSP(), narrow ? 4 : 8);
            break;
        case ValueRep::Constant:
            bits = static_cast<uint64_t>(representation.value());
            break;
        default:
            abortRun("patch " + std::to_string(site->id) + " operand " + std::to_string(i) + " has no location");
        }
        return narrow ? static_cast<uint32_t>(bits) : bits;
    };
    auto trash = [&](RegisterMask mask) {
        forEachRegister(mask, [&](Reg reg) {
            savedRegister(reg) = nextGarbage();
        });
    };
    bool inTrace = site->isCheck || hasObservableEffects(description);
    const TraceEvent* event = nullptr;
    if (site->tailPart == 2) {
        event = &currentRun.expectedTrace->events[currentRun.eventIndex - 1];
        for (unsigned i = 0; i < numberOfOperands; ++i) {
            if (description.operands[i].constraint != ValueRep::Register)
                continue;
            if (!site->representations[i].isReg())
                abortRun("tail patch " + std::to_string(site->id) + " operand " + std::to_string(i) + " is not in a register");
            uint64_t bits = read(i);
            if (bits != event->operands[i])
                abortRun("event " + std::to_string(currentRun.eventIndex - 1) + ": tail patch " + std::to_string(site->id) + " operand " + std::to_string(i) + " after the teardown is " + toHex(bits) + ", expected " + toHex(event->operands[i]));
        }
#if CPU(X86_64)
        uint64_t entryStackPointer = std::bit_cast<uint64_t>(programStackTop) - 8;
#else
        uint64_t entryStackPointer = std::bit_cast<uint64_t>(programStackTop);
#endif
        if (area.stackPointer + currentRun.stackDistance != entryStackPointer)
            abortRun("the stack pointer differs at the tail patch " + std::to_string(site->id));
        if (area.gprs[GPRInfo::callFrameRegister] != area.entryGPRs[GPRInfo::callFrameRegister])
            abortRun("the frame pointer differs at the tail patch " + std::to_string(site->id));
        area.gprs[GPRInfo::returnValueGPR] = event->results[0];
        return;
    }
    if (inTrace) {
        if (currentRun.eventIndex >= currentRun.expectedTrace->events.size())
            abortRun("patch " + std::to_string(site->id) + " runs after the last event");
        event = &currentRun.expectedTrace->events[currentRun.eventIndex];
        if (event->patch != site->id || event->isCheck != site->isCheck)
            abortRun("event " + std::to_string(currentRun.eventIndex) + ": " + (site->isCheck ? "check " : "patch ") + std::to_string(site->id) + " runs, expected " + (event->isCheck ? "check " : "patch ") + std::to_string(event->patch));
        ++currentRun.eventIndex;
    }
    trash(site->earlyClobbers | site->earlyResultRegisters);
    std::vector<uint64_t> operands(numberOfOperands);
    auto check = [&](unsigned i) {
        operands[i] = read(i);
        if (description.operands[i].isSlotAddress) {
            currentRun.slotAddress = operands[i];
            operands[i] = 0;
            return;
        }
        if (event && operands[i] != event->operands[i]) {
            StringPrintStream out;
            out.print(site->representations[numberOfResults + i]);
            abortRun("event " + std::to_string(currentRun.eventIndex - 1) + ": " + (site->isCheck ? "check " : "patch ") + std::to_string(site->id) + " operand " + std::to_string(i) + " in " + toStdString(out) + " is " + toHex(operands[i]) + ", expected " + toHex(event->operands[i]));
        }
    };
    for (unsigned i = 0; i < numberOfOperands; ++i) {
        if (!site->isLateOperand[i])
            check(i);
    }
    uint64_t memory = 0;
    if ((description.reads == 1 && !site->isCheck) || site->isCheck)
        memory = currentMemoryHash();
    if (event && event->hasMemory && memory != event->memoryHash)
        abortRun("event " + std::to_string(currentRun.eventIndex - 1) + ": memory differs at " + (site->isCheck ? "check " : "patch ") + std::to_string(site->id));
    if (site->isCheck) {
        currentRun.didExit = true;
        abortRun("");
    }
    if (event && event->hasSlot) {
        uint64_t hash = hashBytes(reinterpret_cast<uint8_t*>(currentRun.slotAddress), slotSize);
        if (hash != event->slotHash)
            abortRun("event " + std::to_string(currentRun.eventIndex - 1) + ": the slot differs at patch " + std::to_string(site->id));
    }
    if (site->tailPart != 1)
        trash((site->lateClobbers & ~site->fixedResultRegisters) | (site->broughtRegisters & ~site->resultRegisters & ~site->lateOperandRegisters) | site->availableRegisters | site->scratchRegisters | site->resultRegisters);
    for (unsigned i = 0; i < numberOfOperands; ++i) {
        if (site->isLateOperand[i])
            check(i);
    }
    std::vector<uint64_t> results;
    if (event)
        results = event->results;
    else {
        uint64_t hash = description.salt;
        for (unsigned i = 0; i < numberOfOperands; ++i) {
            hash = (hash ^ operands[i]) * 0x9e3779b97f4a7c15ull;
            hash ^= hash >> 31;
        }
        if (description.reads == 1)
            hash ^= memory * 0xff51afd7ed558ccdull;
        else if (description.kind == PurePatch && description.reads)
            hash ^= hashOfRanges(description.reads, [&](unsigned choice) { return regionPointer(choice); }) * 0xff51afd7ed558ccdull;
        hash ^= hash >> 29;
        for (unsigned i = 0; i < description.results.size(); ++i) {
            uint64_t mixed = (hash + i + 1) * 0xbf58476d1ce4e5b9ull;
            results.push_back(truncateToType(description.results[i].type, Interpreter::valueFromHash(description.results[i].type, mixed ^ (mixed >> 27))));
        }
    }
    if (event) {
        for (size_t i = 0; i < event->writes.size(); i += 3) {
            uint8_t* otherBytes = event->writes[i] == SlotRegion ? reinterpret_cast<uint8_t*>(currentRun.slotAddress) : regionPointer(static_cast<unsigned>(event->writes[i]));
            memcpy(otherBytes + event->writes[i + 1], &event->writes[i + 2], 8);
        }
    }
    if (site->tailPart == 1)
        return;
    memset(reinterpret_cast<uint8_t*>(area.stackPointer) - 2048, 0xb5, 2048);
    if (event && event->exits) {
        currentRun.didExit = true;
        abortRun("");
    }
    if (site->isSuspension) {
        uint64_t& framePointer = area.gprs[GPRInfo::callFrameRegister];
        if (framePointer - area.stackPointer != site->frameSize)
            abortRun("a suspension without its frame: " + std::to_string(framePointer - area.stackPointer) + " bytes, planned " + std::to_string(site->frameSize));
        for (unsigned location = 1; location < site->isDeadFrameWord.size(); ++location) {
            if (site->isDeadFrameWord[location]) {
                uint64_t junk = nextGarbage();
                memcpy(reinterpret_cast<uint8_t*>(framePointer) - 8 * location, &junk, 8);
            }
        }
        constexpr uint64_t distance = 8192;
        uint64_t bytes = framePointer + 16 - area.stackPointer;
        if (currentRun.stackDistance + distance + 65536 < programStackSize && bytes < distance) {
            memcpy(reinterpret_cast<uint8_t*>(area.stackPointer - distance), reinterpret_cast<uint8_t*>(area.stackPointer), bytes);
            memset(reinterpret_cast<uint8_t*>(area.stackPointer), 0xc7, bytes);
            area.stackPointer -= distance;
            framePointer -= distance;
            currentRun.stackDistance += distance;
        }
    }
    if (event && event->successor == 1 && description.kind == ThrowingPatch)
        trash(site->registersClobberedOnThrow);
    else {
        for (unsigned i = 0; i < numberOfResults; ++i) {
            const ValueRep& representation = site->representations[i];
            uint64_t bits = results[i];
            bool narrow = description.results[i].type == Int32 || description.results[i].type == Float;
            if (representation.isReg())
                savedRegister(representation.reg()) = narrow && !site->clearsUpperHalf ? (bits | (nextGarbage() << 32)) : bits;
            else if (representation.isStackArgument())
                memcpy(reinterpret_cast<uint8_t*>(area.stackPointer) + representation.offsetFromSP(), &bits, narrow ? 4 : 8);
            else
                abortRun("a result has no location");
        }
    }
    area.successor = event ? event->successor : 0;
}

#if CPU(X86_64)
static constexpr unsigned numberOfGPRs = 16, numberOfFPRs = 16;
static bool isSavable(GPRReg reg) { return reg != X86Registers::esp && reg != scratchGPR; }
#else
static constexpr unsigned numberOfGPRs = 31, numberOfFPRs = 32;
static bool isSavable(GPRReg reg) { return reg != scratchGPR && reg != ARM64Registers::x18; }
#endif

static int32_t offsetOfGPR(unsigned i) { return static_cast<int32_t>(OBJECT_OFFSETOF(RegisterArea, gprs) + i * 8); }
static int32_t offsetOfFPR(unsigned i) { return static_cast<int32_t>(OBJECT_OFFSETOF(RegisterArea, fprs) + i * 8); }

static void emitCallToRuntime(CCallHelpers& jit, PatchSite* site, bool returns)
{
    AllowMacroScratchRegisterUsage allow(jit);
    GPRReg base = scratchGPR;
    auto loadBase = [&] {
        jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(registerArea)), base);
    };
    loadBase();
    for (unsigned i = 0; i < numberOfGPRs; ++i) {
        GPRReg reg = static_cast<GPRReg>(i);
        if (isSavable(reg))
            jit.store64(reg, CCallHelpers::Address(base, offsetOfGPR(i)));
    }
    for (unsigned i = 0; i < numberOfFPRs; ++i)
        jit.storeDouble(static_cast<FPRReg>(i), CCallHelpers::Address(base, offsetOfFPR(i)));
#if CPU(X86_64)
    jit.store64(X86Registers::esp, CCallHelpers::Address(base, OBJECT_OFFSETOF(RegisterArea, stackPointer)));
    jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(runtimeStackTop)), X86Registers::esp);
    jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(site)), X86Registers::edi);
    jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(&didReachPatch)), X86Registers::eax);
    jit.call(X86Registers::eax, NoPtrTag);
#else
    jit.move(ARM64Registers::sp, ARM64Registers::x9);
    jit.store64(ARM64Registers::x9, CCallHelpers::Address(base, OBJECT_OFFSETOF(RegisterArea, stackPointer)));
    jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(runtimeStackTop)), ARM64Registers::x9);
    jit.move(ARM64Registers::x9, ARM64Registers::sp);
    jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(site)), ARM64Registers::x0);
    jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(&didReachPatch)), ARM64Registers::x9);
    jit.call(ARM64Registers::x9, NoPtrTag);
#endif
    if (!returns) {
        jit.breakpoint();
        return;
    }
    loadBase();
#if CPU(X86_64)
    jit.load64(CCallHelpers::Address(base, OBJECT_OFFSETOF(RegisterArea, stackPointer)), X86Registers::esp);
#else
    jit.load64(CCallHelpers::Address(base, OBJECT_OFFSETOF(RegisterArea, stackPointer)), ARM64Registers::x9);
    jit.move(ARM64Registers::x9, ARM64Registers::sp);
#endif
    for (unsigned i = 0; i < numberOfFPRs; ++i)
        jit.loadDouble(CCallHelpers::Address(base, offsetOfFPR(i)), static_cast<FPRReg>(i));
    for (unsigned i = 0; i < numberOfGPRs; ++i) {
        GPRReg reg = static_cast<GPRReg>(i);
        if (isSavable(reg))
            jit.load64(CCallHelpers::Address(base, offsetOfGPR(i)), reg);
    }
}

static void describePatchSite(PatchSite* site, const PatchParameters& params, RegisterMask allocatable)
{
    const PatchOperation* patch = params.value();
    unsigned numberOfResults = site->isCheck ? 0 : static_cast<unsigned>(site->description->results.size());
    site->representations.clear();
    for (unsigned i = 0; i < params.size(); ++i)
        site->representations.append(params[i]);
    RELEASE_ASSERT(params.size() == numberOfResults + site->description->operands.size());
    for (unsigned i = 0; i < params.size(); ++i) {
        if (!params[i].isReg())
            continue;
        RegisterMask bit = maskOf(params[i].reg());
        if (i < numberOfResults) {
            site->resultRegisters |= bit;
            if (site->description->results[i].constraint == ValueRep::Register)
                site->fixedResultRegisters |= bit;
            if (site->description->results[i].constraint == ValueRep::SomeEarlyRegister)
                site->earlyResultRegisters |= bit;
        } else if (site->isLateOperand[i - numberOfResults])
            site->lateOperandRegisters |= bit;
    }
    for (unsigned i = 0; i < patch->numGPScratchRegisters; ++i)
        site->scratchRegisters |= maskOf(Reg(params.gpScratch(i)));
    for (unsigned i = 0; i < patch->numFPScratchRegisters; ++i)
        site->scratchRegisters |= maskOf(Reg(params.fpScratch(i)));
    for (unsigned i = 0; i < site->description->operands.size(); ++i) {
        Reg reg = patch->registerToBringOperandTo(i);
        const ValueRep& representation = params[numberOfResults + i];
        if (reg && !(representation.isReg() && representation.reg() == reg))
            site->broughtRegisters |= maskOf(reg);
    }
    if (!site->isCheck && !site->tailPart)
        site->availableRegisters = allocatable & ~maskOf(params.unavailableRegisters()) & ~site->resultRegisters & ~site->lateOperandRegisters;
    if (!site->isCheck && patch->leavesMoveOutOfResultToCallee()) {
        site->leavesMoveToCallee = true;
        site->filledRegister = patch->registerThatCalleeFillsOutOfResult();
        site->registerMovedIntoResult = patch->registerThatCalleeMovesIntoResult();
    }
    site->frameIsSetUp = params.frameIsSetUp();
    site->hasFrameRecord = params.frame().hasFrameRecord();
    site->frameSize = params.frame().bytesBelowFrameRecord();
    if (site->isSuspension) {
        const std::optional<BitSpan>& dead = params.value()->frameWordsDeadAcrossSuspension();
        RELEASE_ASSERT(params.frameIsSetUp() && dead);
        site->lateClobbers &= ~(registerFileFor(params.proc().traits()).calleeSaved & ~params.frame().calleeSavesMask());
        unsigned words = site->frameSize / 8, keptWords = words - params.frame().bytesDeadAtSuspensions() / 8;
        site->isDeadFrameWord.append(0);
        for (unsigned location = 1; location <= words; ++location)
            site->isDeadFrameWord.append(location > keptWords || dead->get(location));
    }
}

static constexpr size_t codeAlignment = 4 * KB;

struct CompiledProgram {
    explicit CompiledProgram(const ProcedureTraits& traits)
        : procedure(arena, programHeaps->tree, traits)
        , output(procedure)
    {
    }
    Arena arena;
    Procedure procedure;
    Output output;
    Vector<PatchSite*> patchSites;
    RefPtr<ExecutableMemoryHandle> code;
    uint8_t* entry { nullptr };
    uint8_t* secondEntry { nullptr };
    uint64_t seed { 0 };
    ~CompiledProgram()
    {
        for (PatchSite* site : patchSites)
            delete site;
    }
    bool compile(int branchReach)
    {
        CPUFeatureSet features = procedure.traits().features;
        CCallHelpers jit;
        AssemblerPolicy policy = AssemblerPolicy::forImage(features);
        if (branchReach > 0)
            policy.branchReach = static_cast<uint32_t>(branchReach);
        RelocationRecorder recorder(jit, policy);
        readTagsFromInstance(jit);
        generate(procedure, jit, recorder);
        if (procedure.failure())
            return false;
        recorder.routeTransfersThroughEnd();
        emitPadding(jit);
        ImageLinkBuffer linkBuffer(jit, LinkBuffer::Profile::FTL);
        linkBuffer.finalize(Options::dumpDisassembly(), "net"_s);
        std::expected<Vector<Relocation>, LinkFailure> relocations = recorder.link(linkBuffer, nullptr);
        if (!relocations)
            return false;
        Vector<CodeEntry, 1> entries;
        describeEntries(procedure, linkBuffer.linkBuffer(), entries);
        describeTrappingAccesses(procedure, linkBuffer.linkBuffer(), trappingAccesses);
        Vector<Relocation> usual, linked;
        for (const Relocation& relocation : *relocations)
            (relocation.space == SymbolSpace::Number || relocation.space == SymbolSpace::InstanceWord ? linked : usual).append(relocation);
        Vector<uint8_t> image = layOutTestProcedure(procedure, linkBuffer, usual.span());
        uint64_t applied = applyRelocations(image.mutableSpan(), 0, linked.span(), [&](const Relocation& relocation) -> std::optional<size_t> {
            if (relocation.space == SymbolSpace::Number)
                return static_cast<size_t>(linkedNumber(seed, relocation.symbol));
            return static_cast<size_t>(static_cast<uint32_t>(instanceWordsOffset + static_cast<int32_t>(relocation.symbol) * 8)); }, [](const Relocation&) { abort(); });
        RELEASE_ASSERT(applied == linked.size());
        code = ExecutableAllocator::singleton().allocate(image.size() + codeAlignment, JITCompilationMustSucceed);
        uint8_t* start = roundUpToMultipleOf<codeAlignment>(code->start().untaggedPtr<uint8_t*>());
        performJITMemcpy<jitMemcpyRepatch>(start, image.span().data(), image.size());
        MacroAssembler::cacheFlush(start, image.size());
        entry = start + entries[0].offset;
        secondEntry = entries.size() > 1 ? start + entries[1].offset : nullptr;
        codeStart = start;
        codeSize = image.size();
        return true;
    }
    uint8_t* codeStart { nullptr };
    size_t codeSize { 0 };
    Vector<LinkedTrappingAccess> trappingAccesses;
};

static MacroAssemblerCodeRef<JSEntryPtrTag>* entryTrampoline;
static void compileEntryTrampoline()
{
    CCallHelpers jit;
    AllowMacroScratchRegisterUsage allow(jit);
    GPRReg base = scratchGPR;
    auto loadBase = [&] { jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(registerArea)), base); };
    auto addressOfSlot = [&](size_t offset, unsigned i) { return CCallHelpers::Address(base, static_cast<int32_t>(offset + i * 8)); };
    loadBase();
    auto eachG = [&](auto functor) {
        for (unsigned i = 0; i < numberOfGPRs; ++i) {
            if (isSavable(static_cast<GPRReg>(i)))
                functor(static_cast<GPRReg>(i), i);
        }
    };
    eachG([&](GPRReg reg, unsigned i) { jit.store64(reg, addressOfSlot(OBJECT_OFFSETOF(RegisterArea, hostGPRs), i)); });
    for (unsigned i = 0; i < numberOfFPRs; ++i)
        jit.storeDouble(static_cast<FPRReg>(i), addressOfSlot(OBJECT_OFFSETOF(RegisterArea, hostFPRs), i));
#if CPU(X86_64)
    jit.store64(X86Registers::esp, addressOfSlot(OBJECT_OFFSETOF(RegisterArea, hostStackPointer), 0));
    jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(programStackTop)), X86Registers::esp);
#else
    jit.move(ARM64Registers::sp, ARM64Registers::x9);
    jit.store64(ARM64Registers::x9, addressOfSlot(OBJECT_OFFSETOF(RegisterArea, hostStackPointer), 0));
    jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(programStackTop)), ARM64Registers::x9);
    jit.move(ARM64Registers::x9, ARM64Registers::sp);
#endif
    for (unsigned i = 0; i < numberOfFPRs; ++i)
        jit.loadDouble(addressOfSlot(OBJECT_OFFSETOF(RegisterArea, entryFPRs), i), static_cast<FPRReg>(i));
    eachG([&](GPRReg reg, unsigned i) { jit.load64(addressOfSlot(OBJECT_OFFSETOF(RegisterArea, entryGPRs), i), reg); });
    jit.load64(addressOfSlot(OBJECT_OFFSETOF(RegisterArea, target), 0), base);
    jit.call(base, NoPtrTag);
    loadBase();
    eachG([&](GPRReg reg, unsigned i) { jit.store64(reg, addressOfSlot(OBJECT_OFFSETOF(RegisterArea, exitGPRs), i)); });
    for (unsigned i = 0; i < numberOfFPRs; ++i)
        jit.storeDouble(static_cast<FPRReg>(i), addressOfSlot(OBJECT_OFFSETOF(RegisterArea, exitFPRs), i));
#if CPU(X86_64)
    jit.store64(X86Registers::esp, addressOfSlot(OBJECT_OFFSETOF(RegisterArea, exitStackPointer), 0));
    jit.load64(addressOfSlot(OBJECT_OFFSETOF(RegisterArea, hostStackPointer), 0), X86Registers::esp);
#else
    jit.move(ARM64Registers::sp, ARM64Registers::x9);
    jit.store64(ARM64Registers::x9, addressOfSlot(OBJECT_OFFSETOF(RegisterArea, exitStackPointer), 0));
    jit.load64(addressOfSlot(OBJECT_OFFSETOF(RegisterArea, hostStackPointer), 0), ARM64Registers::x9);
    jit.move(ARM64Registers::x9, ARM64Registers::sp);
#endif
    for (unsigned i = 0; i < numberOfFPRs; ++i)
        jit.loadDouble(addressOfSlot(OBJECT_OFFSETOF(RegisterArea, hostFPRs), i), static_cast<FPRReg>(i));
    eachG([&](GPRReg reg, unsigned i) { jit.load64(addressOfSlot(OBJECT_OFFSETOF(RegisterArea, hostGPRs), i), reg); });
    jit.ret();
    LinkBuffer linkBuffer(jit, nullptr);
    entryTrampoline = new MacroAssemblerCodeRef<JSEntryPtrTag>(FINALIZE_CODE(linkBuffer, JSEntryPtrTag, nullptr, "gate"));
}

struct ProgramEmitter {
    const Program& program;
    CompiledProgram& compiled;
    Procedure& procedure;
    Output& output;
    RandomNumbers random;
    unsigned numberOfBlocks;
    std::vector<int> reversePostOrder, reversePostOrderIndex, immediateDominators;
    std::vector<std::vector<int>> predecessors;
    std::vector<std::vector<char>> liveIn, deadStatements;
    bool usesStrongLiveness { true };
    std::vector<std::vector<char>> needsPhi;
    std::vector<Machine::Block*> machineBlocks;
    std::vector<std::vector<Operation*>> phis, valuesAtEnd;
    std::vector<ScopeFork> scopeForks;
    std::vector<char> isEmitted;
    std::vector<Operation*> currentValues;
    Machine::Block* currentBlock { nullptr };
    Operation* bufferValue { nullptr };
    Operation* narrowBufferValue { nullptr };
    std::vector<StackSlot*> slots;
    RegisterMask callerSaved[2] { 0, 0 };
    RegisterMask allocatable { 0 };
    RegisterMask allocatableOf[2] { 0, 0 };
    bool isAcyclic { true };
    uint32_t constantDataIndex { 0 };
    std::map<int, PatchGenerator*> generators;

    ProgramEmitter(const Program& program, CompiledProgram& compiled)
        : program(program)
        , compiled(compiled)
        , procedure(compiled.procedure)
        , output(compiled.output)
        , random { program.emissionSeed }
        , numberOfBlocks(static_cast<unsigned>(program.blocks.size()))
    {
        RegisterFile file = registerFileFor(procedure.traits());
        for (unsigned bank = 0; bank < 2; ++bank) {
            callerSaved[bank] = file.allocatable[bank] & ~file.calleeSaved;
            allocatableOf[bank] = file.allocatable[bank];
            allocatable |= file.allocatable[bank];
        }
    }

    void addUses(int expression, std::vector<char>& used, const std::vector<char>& defined) const
    {
        if (expression < 0)
            return;
        const Expression& node = program.expressions[expression];
        if (node.kind == Expression::Variable && !defined[node.variable])
            used[node.variable] = 1;
        for (int condition : node.children)
            addUses(condition, used, defined);
        if (node.kind == Expression::MemoryLoad) {
            addUses(node.place.index, used, defined);
            addBaseUse(node.place, used, defined);
        }
    }
    static void addBaseUse(const MemoryPlace& place, std::vector<char>& used, const std::vector<char>& defined)
    {
        bool inBuffer = place.region < InstanceRegion || place.region == SumsRegion;
        int holder = place.mode == NarrowReference ? narrowBufferVariable : inBuffer ? bufferVariable
                                                                                     : -1;
        if (place.baseVariable < 0 && holder >= 0 && !defined[holder])
            used[holder] = 1;
        if (place.baseVariable >= 0 && !defined[place.baseVariable])
            used[place.baseVariable] = 1;
    }
    void analyze()
    {
        std::vector<int> state(numberOfBlocks, 0), postOrder;
        std::vector<int> roots { 0 };
        if (program.secondEntryBlock >= 0)
            roots.insert(roots.begin(), program.secondEntryBlock);
        for (int root : roots) {
            std::vector<std::pair<int, unsigned>> path { { root, 0 } };
            state[root] = 1;
            while (path.size()) {
                auto& [bits, i] = path.back();
                if (i == program.blocks[bits].successors.size()) {
                    state[bits] = 2;
                    postOrder.push_back(bits);
                    path.pop_back();
                    continue;
                }
                int successor = program.blocks[bits].successors[i++];
                if (state[successor] == 1)
                    isAcyclic = false;
                if (!state[successor]) {
                    state[successor] = 1;
                    path.push_back({ successor, 0 });
                }
            }
        }
        reversePostOrder.assign(postOrder.rbegin(), postOrder.rend());
        reversePostOrderIndex.assign(numberOfBlocks, -1);
        for (unsigned i = 0; i < reversePostOrder.size(); ++i)
            reversePostOrderIndex[reversePostOrder[i]] = static_cast<int>(i);
        predecessors.assign(numberOfBlocks, {});
        for (int right : reversePostOrder) {
            for (int successor : program.blocks[right].successors)
                predecessors[successor].push_back(right);
        }
        const int above = static_cast<int>(numberOfBlocks);
        immediateDominators.assign(numberOfBlocks + 1, -1);
        reversePostOrderIndex.push_back(-1);
        immediateDominators[above] = above;
        for (int root : roots)
            immediateDominators[root] = above;
        bool changed = true;
        while (changed) {
            changed = false;
            for (int right : reversePostOrder) {
                if (!right || right == program.secondEntryBlock)
                    continue;
                int dominator = -1;
                for (int predecessor : predecessors[right]) {
                    if (immediateDominators[predecessor] < 0)
                        continue;
                    if (dominator < 0) {
                        dominator = predecessor;
                        continue;
                    }
                    int value = dominator, other = predecessor;
                    while (value != other) {
                        while (reversePostOrderIndex[value] > reversePostOrderIndex[other])
                            value = immediateDominators[value];
                        while (reversePostOrderIndex[other] > reversePostOrderIndex[value])
                            other = immediateDominators[other];
                    }
                    dominator = value;
                }
                if (dominator != immediateDominators[right]) {
                    immediateDominators[right] = dominator;
                    changed = true;
                }
            }
        }
        unsigned numberOfVariables = static_cast<unsigned>(program.variableTypes.size());
        std::vector<std::vector<char>> defined(numberOfBlocks, std::vector<char>(numberOfVariables, 0));
        std::vector<char> nothingDefined(numberOfVariables, 0);
        for (int right : reversePostOrder) {
            auto defs = [&](int patch) {
                for (const PatchResult& result : program.patches[patch].results)
                    defined[right][result.variable] = 1;
            };
            for (const Statement& statement : program.blocks[right].statements) {
                if (statement.kind == Statement::PatchCall || statement.kind == Statement::ResultsOfPatch)
                    defs(statement.patch);
                if ((statement.kind == Statement::Assignment || statement.kind == Statement::MemoryUpdate || statement.kind == Statement::PointerSelection || statement.kind == Statement::TrappingLoad || statement.kind == Statement::NumberedRead || statement.kind == Statement::LazyConstant) && statement.variable >= 0)
                    defined[right][statement.variable] = 1;
            }
        }
        liveIn.assign(numberOfBlocks, std::vector<char>(numberOfVariables, 0));
        deadStatements.assign(numberOfBlocks, {});
        changed = true;
        while (changed) {
            changed = false;
            for (unsigned i = static_cast<unsigned>(reversePostOrder.size()); i--;) {
                int right = reversePostOrder[i];
                const ProgramBlock& block = program.blocks[right];
                std::vector<char> live(numberOfVariables, 0);
                for (int successor : block.successors) {
                    for (unsigned variable = 0; variable < numberOfVariables; ++variable)
                        live[variable] |= liveIn[successor][variable];
                }
                addUses(block.expression, live, nothingDefined);
                auto usesOfPatch = [&](int patch) {
                    for (const PatchOperand& operand : program.patches[patch].operands)
                        addUses(operand.expression, live, nothingDefined);
                };
                if (block.terminator == ProgramBlock::PatchTerminator)
                    usesOfPatch(block.patch);
                std::vector<char>& dead = deadStatements[right];
                dead.assign(block.statements.size(), 0);
                for (unsigned j = static_cast<unsigned>(block.statements.size()); j--;) {
                    const Statement& statement = block.statements[j];
                    if ((statement.kind == Statement::Assignment || statement.kind == Statement::PointerSelection || statement.kind == Statement::NumberedRead || statement.kind == Statement::LazyConstant) && !live[statement.variable] && usesStrongLiveness) {
                        dead[j] = 1;
                        continue;
                    }
                    if (statement.kind == Statement::PatchCall || statement.kind == Statement::ResultsOfPatch) {
                        for (const PatchResult& result : program.patches[statement.patch].results)
                            live[result.variable] = 0;
                    }
                    if ((statement.kind == Statement::Assignment || statement.kind == Statement::MemoryUpdate || statement.kind == Statement::PointerSelection || statement.kind == Statement::TrappingLoad || statement.kind == Statement::NumberedRead || statement.kind == Statement::LazyConstant) && statement.variable >= 0)
                        live[statement.variable] = 0;
                    if (statement.kind == Statement::PointerSelection)
                        live[program.pointerKinds[statement.variable] == 2 ? narrowBufferVariable : bufferVariable] = 1;
                    if (statement.kind != Statement::ResultsOfPatch)
                        addUses(statement.expression, live, nothingDefined);
                    if (statement.kind == Statement::MemoryStore || statement.kind == Statement::MemoryUpdate || statement.kind == Statement::TrappingLoad || statement.kind == Statement::NumberedRead) {
                        addUses(statement.place.index, live, nothingDefined);
                        addBaseUse(statement.place, live, nothingDefined);
                    }
                    if (statement.kind == Statement::PatchCall || statement.kind == Statement::ExitCheck)
                        usesOfPatch(statement.patch);
                }
                if (live != liveIn[right]) {
                    liveIn[right] = live;
                    changed = true;
                }
            }
        }
        std::vector<std::set<int>> frontiers(numberOfBlocks);
        for (int right : reversePostOrder) {
            if (predecessors[right].size() < 2)
                continue;
            for (int predecessor : predecessors[right]) {
                int outcome = predecessor;
                while (outcome != immediateDominators[right]) {
                    frontiers[outcome].insert(right);
                    outcome = immediateDominators[outcome];
                }
            }
        }
        needsPhi.assign(numberOfBlocks, std::vector<char>(numberOfVariables, 0));
        for (unsigned variable = 0; variable < numberOfVariables; ++variable) {
            std::vector<int> worklist;
            for (int right : reversePostOrder) {
                if (defined[right][variable] || !right || right == program.secondEntryBlock)
                    worklist.push_back(right);
            }
            while (worklist.size()) {
                int right = worklist.back();
                worklist.pop_back();
                for (int dominator : frontiers[right]) {
                    if (!needsPhi[dominator][variable] && liveIn[dominator][variable]) {
                        needsPhi[dominator][variable] = 1;
                        worklist.push_back(dominator);
                    }
                }
            }
        }
        if (random.chance(10)) {
            for (int right : reversePostOrder) {
                if (predecessors[right].size() < 2)
                    continue;
                for (unsigned variable = 0; variable < numberOfVariables; ++variable) {
                    if (liveIn[right][variable] && random.chance(30))
                        needsPhi[right][variable] = 1;
                }
            }
        }
    }

    LType typeOf(TypeKind type) { return LType(type); }
    Operation* constantOf(TypeKind type, uint64_t bits)
    {
        switch (type) {
        case Int32:
            return output.const32(static_cast<int32_t>(bits));
        case Int64:
            return output.const64(static_cast<int64_t>(bits));
        case Float:
            return output.constFloat(floatFromBits(bits));
        case Double:
            return output.constDouble(doubleFromBits(bits));
        default:
            abort();
        }
    }
    AbstractHeap heapOfPlace(const MemoryPlace& place, int64_t offsetInRegion, unsigned bytes)
    {
        if (place.usesRootHeap)
            return AbstractHeap(rootHeapIndex);
        switch (place.region) {
        case FieldsRegion:
            if ((place.mode == FixedOffset || place.mode == NarrowReference) && offsetInRegion / 8 == (offsetInRegion + bytes - 1) / 8)
                return AbstractHeap(static_cast<HeapIndex>(programHeaps->firstField + offsetInRegion / 8));
            return AbstractHeap(programHeaps->fields);
        case BytesRegion:
            return AbstractHeap(programHeaps->bytes);
        case ImmutableRegion:
            return AbstractHeap(programHeaps->constants, 0, { HeapTrait::Immutable });
        case InstanceRegion:
            return AbstractHeap(programHeaps->instance);
        case SlotRegion:
            return AbstractHeap(programHeaps->slot);
        case ArgumentsRegion:
            return AbstractHeap(programHeaps->arguments);
        case SumsRegion:
            return AbstractHeap(programHeaps->sums);
        case InstanceWordsRegion:
            return AbstractHeap(programHeaps->words);
        case ConstantDataRegion:
            return AbstractHeap(programHeaps->data, 0, { HeapTrait::Immutable });
        default:
            return AbstractHeap(rootHeapIndex);
        }
    }
    struct Address {
        Operation* base;
        ptrdiff_t offset;
        AbstractHeap heap;
    };
    Address addressOf(const MemoryPlace& place, unsigned bytes)
    {
        Operation* start;
        int64_t offset = place.offset;
        AbstractHeap heap = heapOfPlace(place, place.offset, bytes);
        switch (place.region) {
        case InstanceRegion:
            start = output.argumentRegister(instanceGPR);
            offset += instanceRegionOffset;
            break;
        case SlotRegion:
            start = output.slotBase(slots[0]);
            break;
        case ArgumentsRegion:
            start = output.framePointer();
            offset += 16;
            break;
        case InstanceWordsRegion:
            start = output.instanceAddressOf(CodeSymbol { SymbolSpace::InstanceWord, static_cast<uint32_t>(place.offset / 8) });
            offset = place.offset % 8;
            break;
        case ConstantDataRegion:
            start = output.addressOf(CodeSymbol { SymbolSpace::Constant, constantDataIndex });
            break;
        default:
            start = currentValues[bufferVariable];
            if (place.baseVariable >= 0)
                start = program.pointerKinds[place.baseVariable] == 2 ? output.add(output.argumentRegister(cellCageBaseGPR), output.shl(output.zeroExtPtr(currentValues[place.baseVariable]), 3)) : currentValues[place.baseVariable];
            offset += regionOffsets[place.region];
            break;
        }
        if (place.mode == NarrowReference) {
            Operation* scaled = output.shl(output.zeroExtPtr(currentValues[narrowBufferVariable]), 3);
            return { output.add(output.argumentRegister(cellCageBaseGPR), scaled), offset, heap };
        }
        if (place.mode == FixedOffset)
            return { start, offset, heap };
        Operation* index = emitExpression(place.index);
        Operation* masked = output.binary(BitAnd, index, constantOf(program.expressions[place.index].type, place.mask));
        auto scale = [&](Operation* wide) { return place.scale ? output.shl(wide, place.scale) : wide; };
        switch (place.mode) {
        case ZeroExtendedIndex:
            return { output.add(start, scale(output.unary(ZExt32, masked))), offset, heap };
        case SignExtendedIndex: {
            int32_t k = static_cast<int32_t>(place.mask / 2 + 1);
            return { output.add(start, scale(output.unary(SExt32, output.sub(masked, k)))), offset + (static_cast<int64_t>(k) << place.scale), heap };
        }
        case WideIndex:
            return { output.add(start, scale(masked)), offset, heap };
        case MultipliedIndex:
            return { output.add(start, output.mul(masked, place.scale)), offset, heap };
        case SteppedIndex:
            return { output.add(start, scale(output.add(output.unary(ZExt32, masked), place.step))), offset, heap };
        case SumIndex:
            return { output.add(output.add(start, masked), place.step), offset, heap };
        default:
            abort();
        }
    }
    Operation* preventFolding(Operation* operation) { return operation->isConstant() ? output.opaque(operation) : operation; }

    Operation* emitExpression(int expression)
    {
        const Expression& node = program.expressions[expression];
        switch (node.kind) {
        case Expression::Variable:
            return currentValues[node.variable];
        case Expression::Constant:
            return constantOf(node.type, node.bits);
        case Expression::UnaryOperation:
            return output.unary(node.opcode, emitExpression(node.children[0]));
        case Expression::BinaryOperation: {
            Operation* left = emitExpression(node.children[0]);
            Operation* right = emitExpression(node.children[1]);
            return output.binary(node.opcode, left, right);
        }
        case Expression::SelectOperation: {
            Operation* condition = emitExpression(node.children[0]);
            Operation* left = emitExpression(node.children[1]);
            Operation* right = emitExpression(node.children[2]);
            return output.select(condition, left, right);
        }
        case Expression::MemoryLoad: {
            unsigned bytes = accessSize(node.opcode, node.type);
            Address address = addressOf(node.place, bytes);
            Operation* loaded = output.Builder::load(node.opcode, typeOf(node.type), address.base, address.offset, address.heap);
            if (node.bits && loaded->opcode() == Load && loaded->type() == Int64)
                output.noteIntegerAddedTo(loaded);
            return loaded;
        }
        case Expression::LinkedNumber:
            return output.numberOf(CodeSymbol { SymbolSpace::Number, static_cast<uint32_t>(node.bits) });
        case Expression::ChillOperation: {
            Operation* left = emitExpression(node.children[0]);
            Operation* right = emitExpression(node.children[1]);
            return output.chill(node.opcode, left, right);
        }
        case Expression::LowHalf:
            return output.lowWord(emitExpression(node.children[0]));
        case Expression::NonNegativeExtension:
            return output.extendWordThatIsNotNegative(emitExpression(node.children[0]));
        case Expression::OpaqueValue:
            return output.opaque(emitExpression(node.children[0]));
        case Expression::BitCast:
            return output.bitwiseCast(emitExpression(node.children[0]), typeOf(node.type));
        case Expression::CanonicalNumber: {
            Operation* left = emitExpression(node.children[0]);
            return output.select(output.binary(Equal, left, left), left, constantOf(node.type, node.type == Double ? bitsFromDouble(1.5) : bitsFromFloat(1.5f)));
        }
        }
        abort();
    }
    static unsigned accessSize(Opcode opcode, TypeKind type)
    {
        switch (opcode) {
        case Load8Z:
        case Load8S:
        case Store8:
            return 1;
        case Load16Z:
        case Load16S:
        case Store16:
            return 2;
        default:
            return type == Int32 || type == Float ? 4 : 8;
        }
    }

    Operation* phiOf(int right, int variable)
    {
        Operation*& operation = phis[right][variable];
        if (!operation)
            operation = output.phiIn(machineBlocks[right], typeOf(program.variableTypes[variable]));
        if (program.pointerKinds[variable] == 2 && program.marksNarrowReferences)
            operation->markAsNarrowReference();
        return operation;
    }
    void emitUpsilons(int right)
    {
        std::set<int> done;
        for (int successor : program.blocks[right].successors) {
            if (!done.insert(successor).second)
                continue;
            for (unsigned i = 0; i < program.variableTypes.size(); ++i) {
                unsigned variable = program.emissionSeed & 2 ? static_cast<unsigned>(program.variableTypes.size()) - 1 - i : i;
                if (needsPhi[successor][variable])
                    output.upsilon(currentValues[variable], phiOf(successor, static_cast<int>(variable)));
            }
        }
    }

    Reg nthRegister(RegisterMask candidates, unsigned ordinal)
    {
        RELEASE_ASSERT(candidates);
        for (unsigned skipped = ordinal % static_cast<unsigned>(std::popcount(candidates)); skipped--;)
            candidates &= candidates - 1;
        return Reg::fromIndex(static_cast<unsigned>(std::countr_zero(candidates)));
    }
    static bool isFloatingPoint(TypeKind type) { return type == Float || type == Double; }

    PatchOperation* emitPatch(int otherPatch, int block)
    {
        const PatchDescription& description = program.patches[otherPatch];
        std::vector<Operation*> values;
        for (const PatchOperand& operand : description.operands) {
            values.push_back(operand.isSlotAddress ? output.slotBase(slots[0]) : emitExpression(operand.expression));
            if (values.back()->opcode() == NumberOf)
                values.back() = output.opaque(values.back());
        }
        bool isCall = description.kind == CallPatch || description.kind == ThrowingPatch || description.kind == ExitingPatch;
        RegisterMask fixedOperands[2] { 0, 0 }, fixedResults[2] { 0, 0 };
        unsigned demand[2] { description.numberOfGPScratches, description.numberOfFPScratches };
        uint8_t gpScratch = description.numberOfGPScratches, fpScratch = description.numberOfFPScratches;
        Vector<ValueRep> operandRepresentations, resultRepresentations;
        for (unsigned i = 0; i < description.operands.size(); ++i) {
            const PatchOperand& operand = description.operands[i];
            TypeKind otherType = operand.isSlotAddress ? Int64 : program.expressions[operand.expression].type;
            unsigned bank = isFloatingPoint(otherType);
            switch (operand.constraint) {
            case ValueRep::Register: {
                Reg reg;
                if (operand.location == 1900)
                    reg = Reg(GPRInfo::returnValueGPR);
                else if (operand.location >= 1000)
                    reg = bank ? Reg(FPRInfo::toArgumentRegister(operand.location - 1000)) : Reg(GPRInfo::toArgumentRegister(operand.location - 1000));
                else {
                    RegisterMask candidates = callerSaved[bank] & ~fixedOperands[bank];
                    reg = nthRegister(candidates ? candidates : allocatableOf[bank] & ~fixedOperands[bank], operand.location);
                }
                fixedOperands[bank] |= maskOf(reg);
                operandRepresentations.append(ValueRep::reg(reg));
                break;
            }
            case ValueRep::StackArgument:
                procedure.requestCallArgAreaSizeInBytes((operand.location + 1) * 8);
                operandRepresentations.append(ValueRep::stackArgument(static_cast<int32_t>(operand.location * 8)));
                break;
            default:
                if (operand.constraint == ValueRep::SomeRegister || operand.constraint == ValueRep::SomeLateRegister)
                    ++demand[bank];
                operandRepresentations.append(ValueRep(static_cast<ValueRep::Kind>(operand.constraint)));
                break;
            }
        }
        Vector<Type> resultTypes;
        for (const PatchResult& result : description.results) {
            unsigned bank = isFloatingPoint(result.type);
            resultTypes.append(Type(result.type));
            if (result.constraint == ValueRep::Register) {
                Reg reg;
                if (result.location >= 2000)
                    reg = bank ? Reg(FPRInfo::returnValueFPR) : result.location == 2000 ? Reg(GPRInfo::returnValueGPR)
                                                                                        : Reg(GPRInfo::returnValueGPR2);
                else
                    reg = nthRegister(callerSaved[bank] & ~fixedResults[bank] ? callerSaved[bank] & ~fixedResults[bank] : allocatableOf[bank] & ~fixedResults[bank], result.location);
                if (fixedResults[bank] & maskOf(reg))
                    reg = nthRegister(allocatableOf[bank] & ~fixedResults[bank], result.location);
                fixedResults[bank] |= maskOf(reg);
                resultRepresentations.append(ValueRep::reg(reg));
            } else {
                ++demand[bank];
                resultRepresentations.append(ValueRep(static_cast<ValueRep::Kind>(result.constraint)));
            }
        }
        PatchSite* site = new PatchSite;
        compiled.patchSites.append(site);
        site->description = &description;
        site->id = otherPatch;
        for (unsigned i = 0; i < description.operands.size(); ++i) {
            site->operandTypes.append(description.operands[i].isSlotAddress ? Int64 : program.expressions[description.operands[i].expression].type);
            site->isLateOperand.append(description.operands[i].constraint == ValueRep::LateColdAny || description.operands[i].constraint == ValueRep::SomeLateRegister);
        }
        for (unsigned bank = 0; bank < 2; ++bank) {
            if (isCall) {
                site->lateClobbers |= callerSaved[bank];
                continue;
            }
            RegisterMask fixed = fixedOperands[bank] | fixedResults[bank];
            RegisterMask free = allocatableOf[bank] & ~fixed;
            unsigned numberFree = static_cast<unsigned>(std::popcount(free));
            for (unsigned i = 0; i < description.operands.size() && numberFree < demand[bank]; ++i) {
                bool isFloat = !description.operands[i].isSlotAddress && isFloatingPoint(program.expressions[description.operands[i].expression].type);
                if (isFloat != !!bank || (operandRepresentations[i].kind() != ValueRep::SomeRegister && operandRepresentations[i].kind() != ValueRep::SomeLateRegister))
                    continue;
                operandRepresentations[i] = ValueRep(operandRepresentations[i].kind() == ValueRep::SomeRegister ? ValueRep::WarmAny : ValueRep::LateColdAny);
                --demand[bank];
            }
            while (numberFree < demand[bank] && !bank && gpScratch)
                --gpScratch, --demand[bank];
            while (numberFree < demand[bank] && bank && fpScratch)
                --fpScratch, --demand[bank];
            for (unsigned i = 0; i < description.operands.size() && numberFree < demand[bank]; ++i) {
                bool isFloat = !description.operands[i].isSlotAddress && isFloatingPoint(program.expressions[description.operands[i].expression].type);
                if (isFloat != !!bank || !operandRepresentations[i].isReg() || (fixedResults[bank] & maskOf(operandRepresentations[i].reg())))
                    continue;
                fixedOperands[bank] &= ~maskOf(operandRepresentations[i].reg());
                fixed &= ~maskOf(operandRepresentations[i].reg());
                free |= maskOf(operandRepresentations[i].reg());
                operandRepresentations[i] = ValueRep(ValueRep::ColdAny);
                ++numberFree;
            }
            RELEASE_ASSERT(numberFree >= demand[bank]);
            RegisterMask wanted = (description.earlyClobbers | description.lateClobbers) & free & callerSaved[bank], allowed = 0;
            unsigned spare = std::popcount(callerSaved[bank]) >= 6 ? static_cast<unsigned>(std::popcount(callerSaved[bank] & ~fixed)) : numberFree;
            for (unsigned room = spare > demand[bank] ? spare - demand[bank] : 0; room && wanted; --room) {
                allowed |= wanted & -wanted;
                wanted &= wanted - 1;
            }
            site->earlyClobbers |= description.earlyClobbers & (allowed | (fixedResults[bank] & ~fixedOperands[bank]));
            site->lateClobbers |= description.lateClobbers & (allowed | fixed);
        }
        site->isCall = isCall;
        LType type = Void;
        if (resultTypes.size() == 1)
            type = resultTypes[0];
        else if (resultTypes.size() > 1)
            type = procedure.addTuple(resultTypes.span());

        PureFunction pure { (1ull << 40) + (description.salt & 0xffffffffu) + (static_cast<uint64_t>(description.reads) << 33), !values.empty(), false, description.reads != 0 };
        if (description.kind == PurePatch) {
            Vector<Operation*> operands;
            for (Operation* value : values)
                operands.append(value);
            if (Operation* known = output.findPurePatch(pure, type, operands.span())) {
                currentValues[description.results[0].variable] = known;
                return nullptr;
            }
        }
        if (block >= 0) {
            emitUpsilons(block);
            if (program.usesScopes)
                scopeForks[block] = output.forkScope();
        }
        PatchOperation* patch = output.patch(type);
        for (unsigned i = 0; i < values.size(); ++i)
            patch->append(values[i], operandRepresentations[i]);
        if (!resultRepresentations.isEmpty())
            patch->resultConstraints = resultRepresentations;
        Effects effects = Effects::none();
        effects.exitsSideways = description.exitsSideways;
        effects.controlDependent = description.isControlDependent;
        effects.writesLocalState = description.writesLocalState;
        effects.mayCollect = description.mayCollect;
        auto setOf = [&](uint8_t k) -> HeapSet {
            if (!k)
                return HeapSet();
            if (k == 1)
                return HeapSet::top();
            if (program.seed >= firstSeedWithNumberedReads && heapSets.count(k))
                return heapSets[k];
            if (k >= firstHeapList) {
                static std::map<HeapIndex, AbstractHeap>* kept = new std::map<HeapIndex, AbstractHeap>;
                Vector<const AbstractHeap*> pointers;
                for (const MemoryRange& piece : memoryRangesOf(k)) {
                    HeapIndex heap = piece.offset >= 0 ? static_cast<HeapIndex>(programHeaps->firstField + piece.offset / 8) : piece.region == InstanceRegion ? programHeaps->instance
                                                                                                                                                              : programHeaps->bytes;
                    pointers.append(&kept->try_emplace(heap, AbstractHeap(heap)).first->second);
                }
                return heapSets[k] = procedure.heapSet(pointers.span());
            }
            AbstractHeap otherHeap;
            switch (k - 2) {
            case FieldsRegion:
                otherHeap = AbstractHeap(programHeaps->fields);
                break;
            case BytesRegion:
                otherHeap = AbstractHeap(programHeaps->bytes);
                break;
            case InstanceRegion:
                otherHeap = AbstractHeap(programHeaps->instance);
                break;
            case SlotRegion:
                otherHeap = AbstractHeap(programHeaps->slot);
                break;
            default:
                otherHeap = AbstractHeap(rootHeapIndex);
                break;
            }
            return heapSets[k] = procedure.heapSet(otherHeap);
        };
        effects.reads = setOf(description.reads);
        effects.writes = setOf(description.writes);
        patch->effects = effects;
        patch->clobberEarly(site->earlyClobbers);
        patch->clobberLate(site->lateClobbers);
        patch->numGPScratchRegisters = gpScratch;
        patch->numFPScratchRegisters = fpScratch;
        if (description.kind == KeepAlivePatch) {
            patch->onlyKeepsChildrenAlive = true;
            patch->effects.writesLocalState = true;
            patch->effects.reads = HeapSet::top();
            patch->setRunsWithoutFrame(true);
            return patch;
        }
        patch->setRunsWithoutFrame(description.runsWithoutFrame);
        patch->setShowsRegistersToCollector(description.runsWithoutFrame && description.mayCollect);
        patch->setNeedsFrameRecord(description.needsFrameRecord && !description.runsWithoutFrame);
        patch->setFindsOperandsInFrameWithoutRecord(description.salt & 1);
        if (description.constrainsResult && description.results.size() == 1 && description.results[0].constraint == ValueRep::SomeEarlyRegister && !isFloatingPoint(description.results[0].type)) {
            RegisterMask set = callerSaved[0] & ~(site->earlyClobbers | site->lateClobbers | fixedOperands[0]);
            if (std::popcount(set) >= 3)
                patch->holdResultsTo(registerSetOf(set));
        }
        bool brings = description.bringsOperands && !description.numberOfGPScratches && !site->isLateOperand.contains(true);
        patch->setClearsUpperHalf(description.clearsUpperHalf && description.results.size() == 1 && description.results[0].type == Int32);
        site->clearsUpperHalf = patch->clearsUpperHalf();
        patch->setBringsOperandsToTheirRegisters(brings);
        if (description.fillsRegisterFromResult && brings) {
            for (unsigned i = 0; i < operandRepresentations.size(); ++i) {
                if (operandRepresentations[i].isReg() && operandRepresentations[i].isGPR() && operandRepresentations[i].reg() != Reg(GPRInfo::returnValueGPR)) {
                    patch->setRegisterThatCalleeFillsOutOfResult(operandRepresentations[i].reg());
                    if (description.movesIntoResult)
                        patch->setRegisterThatCalleeMovesIntoResult(Reg(GPRInfo::returnValueGPR2));
                    break;
                }
            }
        }
        site->argumentAreaSize = procedure.callArgAreaSizeInBytes();
        if (description.kind == SuspensionPatch) {
            RegisterMask lost = allocatable;
            if constexpr (SavedFrame::preservedRegister != InvalidGPRReg)
                lost &= ~maskOf(Reg(SavedFrame::preservedRegister));
            site->lateClobbers |= lost;
            site->isSuspension = true;
            patch->clobberLate(lost);
            patch->setIsSuspension(true);
            if (description.salt & 4) {
                for (unsigned i = 0; i < description.operands.size(); ++i) {
                    if (description.operands[i].constraint == ValueRep::LateColdAny && program.expressions[description.operands[i].expression].type == Int64 && !values[i]->hasNoPosition()) {
                        patch->setOperandEncodedAsFrameWordIndex(i);
                        break;
                    }
                }
            }
        }
        if (block < 0) {
            PatchGenerator*& generator = generators[otherPatch];
            if (!generator) {
                generator = procedure.createGenerator([site, all = allocatable, &f = compiled](CCallHelpers& jit, const PatchParameters& params) {
                    PatchSite* own = new PatchSite(*site);
                    f.patchSites.append(own);
                    describePatchSite(own, params, all);
                    emitCallToRuntime(jit, own, true);
                });
            }
            patch->setGenerator(generator);
            if (description.kind == PurePatch) {
                patch->pureFunction = pure;
                output.recordPurePatch(patch);
            }
            return patch;
        }
        patch->setGenerator([site, all = allocatable](CCallHelpers& jit, const PatchParameters& params) {
            describePatchSite(site, params, all);
            emitCallToRuntime(jit, site, true);
            AllowMacroScratchRegisterUsage allow(jit);
            jit.load64(CCallHelpers::Address(scratchGPR, OBJECT_OFFSETOF(RegisterArea, successor)), scratchGPR);
            CCallHelpers::Jump other = jit.branchTest64(CCallHelpers::NonZero, scratchGPR);
            CCallHelpers::Jump toFirst;
            if (!params.fallsThroughToSuccessor(0))
                toFirst = jit.jump();
            params.addLatePath([other, toFirst, first = params.successorLabels()[0], second = params.successorLabels()[1]](CCallHelpers& jit) {
                if (toFirst.isSet())
                    toFirst.linkTo(*first, &jit);
                other.link(&jit);
                jit.jump().linkTo(*second, &jit);
            });
        });
        if (description.kind == TailPatch) {
            PatchSite* second = new PatchSite(*site);
            compiled.patchSites.append(second);
            site->tailPart = 1;
            second->tailPart = 2;
            patch->setRunsWithoutFrame(description.runsWithoutFrame);
            patch->setLeavesByReturning(description.writesLocalState);
            bool follows = (description.salt & 48) == 48;
            patch->setFollowsFrameSetup(follows && !description.writesLocalState);
            patch->setTestsStackAheadOfFramedPart(follows && !description.writesLocalState && isX86_64() && description.runsWithoutFrame);
            patch->effects.writesLocalState = false;
            patch->setGenerator([site, second](CCallHelpers& jit, const PatchParameters& params) {
                describePatchSite(site, params, 0);
                describePatchSite(second, params, 0);
                emitCallToRuntime(jit, site, true);
                if (params.frameIsSetUp())
                    emitFrameTeardown(jit, params.frame());
                emitCallToRuntime(jit, second, true);
                jit.ret();
            });
            patch->effects.terminal = true;
            return patch;
        }
        const ProgramBlock& otherBlock = program.blocks[block];
        for (unsigned i = 0; i < otherBlock.successors.size(); ++i)
            output.appendSuccessor(Edge(machineBlocks[otherBlock.successors[i]], static_cast<EdgeClass>(otherBlock.edgeClasses[i])));
        if (description.kind == ThrowingPatch) {
            site->registersClobberedOnThrow = callerSaved[0] | callerSaved[1];
            patch->clobberLateOnSuccessor(1, registerSetOf(site->registersClobberedOnThrow));
        }
        patch->effects.terminal = true;
        return patch;
    }

    std::map<uint8_t, HeapSet> heapSets;
    std::map<int, PatchOperation*> pendingPatch;
    void takeResults(int otherPatch, PatchOperation* patch)
    {
        const PatchDescription& description = program.patches[otherPatch];
        if (!patch)
            return;
        if (description.results.size() == 1)
            currentValues[description.results[0].variable] = patch;
        for (unsigned i = 0; description.results.size() > 1 && i < description.results.size(); ++i)
            currentValues[description.results[i].variable] = output.extract(patch, i);
    }

    void emitStatement(const Statement& statement, int block)
    {
        switch (statement.kind) {
        case Statement::Assignment:
            currentValues[statement.variable] = emitExpression(statement.expression);
            return;
        case Statement::MemoryStore: {
            Operation* value = emitExpression(statement.expression);
            Address address = addressOf(statement.place, accessSize(statement.access, program.expressions[statement.expression].type));
            output.Builder::store(statement.access, value, address.base, address.offset, address.heap);
            return;
        }
        case Statement::MemoryUpdate: {
            TypeKind otherType = program.expressions[statement.expression].type;
            unsigned bytes = accessSize(statement.access, otherType);
            Address address = addressOf(statement.place, bytes);
            Opcode loadOpcode = statement.access == Store8 ? Load8Z : statement.access == Store16 ? Load16Z
                                                                                                  : Load;
            Operation* old = output.Builder::load(loadOpcode, typeOf(otherType), address.base, address.offset, address.heap);
            Operation* other = emitExpression(statement.expression);
            Operation* sum = statement.hasConstantOnLeft ? output.binary(statement.opcode, other, old) : output.binary(statement.opcode, old, other);
            output.Builder::store(statement.access, sum, address.base, address.offset, address.heap);
            if (statement.variable >= 0)
                currentValues[statement.variable] = (statement.form & 1) ? old : sum;
            return;
        }
        case Statement::PatchCall: {
            PatchOperation* patch = emitPatch(statement.patch, -1);
            if (program.patches[statement.patch].kind != KeepAlivePatch)
                takeResults(statement.patch, patch);
            return;
        }
        case Statement::ExitCheck: {
            const PatchDescription& description = program.patches[statement.patch];
            Operation* predicate = emitExpression(statement.expression);
            std::vector<Operation*> values;
            for (const PatchOperand& operand : description.operands) {
                values.push_back(emitExpression(operand.expression));
                if (values.back()->opcode() == NumberOf)
                    values.back() = output.opaque(values.back());
            }
            PatchSite* site = new PatchSite;
            compiled.patchSites.append(site);
            site->description = &description;
            site->id = statement.patch;
            site->isCheck = true;
            PatchOperation* check = output.checkIsZero(predicate);
            for (unsigned i = 0; i < values.size(); ++i) {
                check->append(values[i], ValueRep(ValueRep::ColdAny));
                site->operandTypes.append(program.expressions[description.operands[i].expression].type);
                site->isLateOperand.append(false);
            }
            check->setRunsWithoutFrame(description.salt & 2);
            PatchGenerator*& generator = generators[statement.patch];
            if (!generator) {
                generator = procedure.createGenerator([site, all = allocatable, &f = compiled](CCallHelpers& jit, const PatchParameters& params) {
                    PatchSite* own = new PatchSite(*site);
                    f.patchSites.append(own);
                    describePatchSite(own, params, all);
                    emitCallToRuntime(jit, own, false);
                });
            }
            check->setGenerator(generator);
            check->setSharesLatePath(description.salt & 8);
            return;
        }
        case Statement::MemoryFence:
            output.fence();
            return;
        case Statement::PointerSelection: {
            bool narrow = program.pointerKinds[statement.variable] == 2;
            Operation* condition = emitExpression(statement.expression);
            Operation* operation = narrow ? output.select(condition, currentValues[narrowBufferVariable], output.const32(0)) : output.select(condition, currentValues[bufferVariable], output.const64(0));
            if (operation->isConstant())
                operation = output.opaque(operation);
            if (narrow && program.marksNarrowReferences && (operation->opcode() == Select || operation->opcode() == Opaque))
                operation->markAsNarrowReference();
            currentValues[statement.variable] = operation;
            return;
        }
        case Statement::TrappingLoad: {
            Address address = addressOf(statement.place, accessSize(statement.access, Int32));
            output.setOrigin(Origin(static_cast<uint32_t>(statement.patch) + 1));
            Operation* loaded = output.Builder::load(statement.access, Int32, address.base, address.offset, address.heap, Builder::Trapping::Yes);
            output.setOrigin(Origin());
            if (statement.variable >= 0)
                currentValues[statement.variable] = loaded;
            return;
        }
        case Statement::NumberedRead:
        case Statement::LazyConstant: {
            TypeKind kind = program.variableTypes[statement.variable];
            LType type = typeOf(kind);
            bool reads = statement.kind == Statement::NumberedRead;
            Operation* owner = preventFolding(emitExpression(statement.expression));
            PureFunction function { (1ull << 42) + statement.key * 4 + static_cast<unsigned>(kind) + (reads ? 1ull << 20 : 0), reads, false, reads };
            if (Operation* known = reads ? output.findPurePatch(function, type, std::span<Operation* const>(&owner, 1)) : output.findPurePatch(function, type, {})) {
                currentValues[statement.variable] = known;
                return;
            }
            Operation* asked = reads || owner->type() == Int32 ? owner : output.unary(Trunc, owner);
            Operation* test = reads ? output.unary(Trunc, output.binary(BitAnd, owner, constantOf(Int64, 1))) : asked;
            ScopeFork fork = output.forkScope();
            Machine::Block* first = output.newBlock();
            Machine::Block* second = statement.form & 1 ? nullptr : output.newBlock();
            Machine::Block* join = output.newBlock();
            Operation* phi = output.phiIn(join, type);
            Operation* other = constantOf(kind, Interpreter::valueOfKey(kind, statement.key));
            if (!second)
                output.upsilon(other, phi);
            EdgeClass classOfSecond = statement.form & 2 ? EdgeClass::Rare : statement.form & 4 ? EdgeClass::Unlikely
                                                                                                : EdgeClass::Normal;
            output.branch(preventFolding(test), Edge(first, EdgeClass::Normal), Edge(second ? second : join, second ? classOfSecond : EdgeClass::Normal));
            if (statement.form & 8)
                output.appendToArm(first, fork);
            else
                output.appendTo(first);
            Operation* found = nullptr;
            AbstractHeap heap;
            if (reads) {
                Address address = addressOf(statement.place, accessSize(Load, kind));
                heap = address.heap;
                found = output.Builder::load(Load, type, address.base, address.offset, address.heap);
            } else
                found = output.opaque(constantOf(kind, Interpreter::valueOfKey(kind, statement.key)));
            output.upsilon(found, phi);
            output.jump(Edge(join, EdgeClass::Normal));
            if (second) {
                if (statement.form & 8)
                    output.appendToArm(second, fork);
                else
                    output.appendTo(second);
                output.upsilon(other, phi);
                output.jump(Edge(join, EdgeClass::Normal));
            }
            output.appendToJoin(join, fork);
            currentBlock = join;
            if (reads)
                output.recordInPlaceOfReadOfMemory(function, phi, owner, heap);
            else
                output.recordInPlaceOfPurePatch(function, phi);
            currentValues[statement.variable] = phi;
            return;
        }
        case Statement::ResultsOfPatch:
            takeResults(statement.patch, pendingPatch[block]);
            return;
        }
    }

    bool isEveryPathEmitted(int right) const
    {
        std::vector<char> seen(numberOfBlocks, 0);
        std::vector<int> worklist { right };
        while (worklist.size()) {
            int value = worklist.back();
            worklist.pop_back();
            for (int predecessor : predecessors[value]) {
                if (predecessor == immediateDominators[right] || seen[predecessor])
                    continue;
                seen[predecessor] = 1;
                if (!isEmitted[predecessor])
                    return false;
                worklist.push_back(predecessor);
            }
        }
        return true;
    }

    void enterBlock(int right, int previous)
    {
        if (!right && previous < 0)
            return;
        if (!right || right == program.secondEntryBlock) {
            output.appendTo(machineBlocks[right]);
            return;
        }
        if (program.usesScopes) {
            if (predecessors[right].size() == 1 && machineBlocks[right]->numberOfIncomingEdges() == 1) {
                int predecessor = predecessors[right][0];
                bool direct = currentBlock == machineBlocks[right]->singlePredecessor();
                if (predecessor == previous && direct && program.blocks[predecessor].terminator == ProgramBlock::JumpTerminator && program.blocks[predecessor].edgeClasses[0] != 2 && random.chance(60)) {
                    output.appendToContinuation(machineBlocks[right]);
                    return;
                }
                if (random.chance(75) && scopeForks[predecessor].block) {
                    output.appendToSuccessor(machineBlocks[right], scopeForks[predecessor]);
                    return;
                }
            } else if (predecessors[right].size() > 1 && immediateDominators[right] >= 0 && immediateDominators[right] < static_cast<int>(numberOfBlocks) && scopeForks[immediateDominators[right]].block && random.chance(60)) {
                const ScopeFork& fork = scopeForks[immediateDominators[right]];
                if (output.canRestoreScope(fork) && isEveryPathEmitted(right)) {
                    output.appendToJoin(machineBlocks[right], fork);
                    return;
                }
            }
        }
        output.appendTo(machineBlocks[right]);
    }

    void run()
    {
        usesStrongLiveness = procedure.traits().checksAllocation || (program.emissionSeed & 1);
        analyze();
        machineBlocks.assign(numberOfBlocks, nullptr);
        phis.assign(numberOfBlocks, std::vector<Operation*>(program.variableTypes.size(), nullptr));
        valuesAtEnd.assign(numberOfBlocks, {});
        scopeForks.assign(numberOfBlocks, ScopeFork());
        isEmitted.assign(numberOfBlocks, 0);
        Machine::Block* entry = procedure.addEntrypoint(primaryEntry);
        machineBlocks[0] = entry;
        if (program.secondEntryBlock >= 0)
            machineBlocks[program.secondEntryBlock] = procedure.addEntrypoint(1);
        output.appendTo(machineBlocks[reversePostOrder[0]]);
        for (int right : reversePostOrder) {
            if (!machineBlocks[right])
                machineBlocks[right] = output.newBlock();
        }
        for (unsigned i = 0; program.usesSlot && i < program.numberOfSlots; ++i)
            slots.push_back(procedure.addStackSlot(slotSize));
        if (program.extraSlotSize) {
            StackSlot* extra = procedure.addStackSlot(program.extraSlotSize);
            slots.push_back(extra);
        }
        {
            Memory memory;
            memset(memory.regions[ConstantDataRegion], 0, regionSize);
            fillConstantData(memory, program.seed);
            constantDataIndex = procedure.addConstantData(std::span<const uint8_t>(memory.regions[ConstantDataRegion], regionSize), 16, ConstantKind::Unlabeled);
        }
        if (program.stackCheckMode) {
            procedure.setStackCheckGenerator([](void*, CCallHelpers& jit, const Frame&, const StackCheckSite&) {
                AllowMacroScratchRegisterUsage allow(jit);
                jit.move(CCallHelpers::TrustedImm64(0x1badbadbadbadbad), scratchGPR);
            },
                nullptr);
            if (program.stackCheckMode == 2)
                procedure.setLargestFrameSetUpWithoutStackCheck(64);
        }
        int previous = -1;
        for (int right : reversePostOrder) {
            enterBlock(right, previous);
            currentBlock = machineBlocks[right];
            if (!right || right == program.secondEntryBlock) {
                currentValues.assign(program.variableTypes.size(), nullptr);
                bufferValue = output.argumentRegister(Reg(GPRInfo::argumentGPR0));
                for (unsigned i = 0; i < 4; ++i)
                    currentValues[1 + i] = output.argumentRegister(Reg(GPRInfo::toArgumentRegister(1 + i)));
                for (unsigned i = 0; i < 3; ++i)
                    currentValues[5 + i] = output.argumentRegister(Reg(FPRInfo::toArgumentRegister(i)));
                narrowBufferValue = output.unary(Trunc, output.lShr(bufferValue, 3));
                if (program.marksNarrowReferences)
                    narrowBufferValue->markAsNarrowReference();
                currentValues[bufferVariable] = bufferValue;
                currentValues[narrowBufferVariable] = narrowBufferValue;
                for (unsigned variable = 0; variable < program.variableTypes.size(); ++variable) {
                    if (!currentValues[variable])
                        currentValues[variable] = constantOf(program.variableTypes[variable], 0);
                }
            } else {
                currentValues = immediateDominators[right] < static_cast<int>(numberOfBlocks) ? valuesAtEnd[immediateDominators[right]] : std::vector<Operation*>(program.variableTypes.size(), nullptr);
                for (unsigned variable = 0; variable < program.variableTypes.size(); ++variable) {
                    if (needsPhi[right][variable])
                        currentValues[variable] = phiOf(right, static_cast<int>(variable));
                }
            }
            const ProgramBlock& block = program.blocks[right];
            for (unsigned j = 0; j < block.statements.size(); ++j) {
                if (!deadStatements[right][j])
                    emitStatement(block.statements[j], right);
            }
            switch (block.terminator) {
            case ProgramBlock::JumpTerminator:
                emitUpsilons(right);
                if (program.usesScopes)
                    scopeForks[right] = output.forkScope();
                output.jump(Edge(machineBlocks[block.successors[0]], block.edgeClasses[0] == 2 ? EdgeClass::Normal : static_cast<EdgeClass>(block.edgeClasses[0])));
                break;
            case ProgramBlock::BranchTerminator: {
                Operation* condition = preventFolding(emitExpression(block.expression));
                emitUpsilons(right);
                if (program.usesScopes)
                    scopeForks[right] = output.forkScope();
                output.branch(condition, Edge(machineBlocks[block.successors[0]], static_cast<EdgeClass>(block.edgeClasses[0])), Edge(machineBlocks[block.successors[1]], static_cast<EdgeClass>(block.edgeClasses[1])));
                break;
            }
            case ProgramBlock::SwitchTerminator: {
                Operation* value = preventFolding(emitExpression(block.expression));
                emitUpsilons(right);
                if (program.usesScopes)
                    scopeForks[right] = output.forkScope();
                Vector<SwitchCase> cases;
                for (unsigned i = 0; i < block.caseValues.size(); ++i)
                    cases.append(SwitchCase { block.caseValues[i], Edge(machineBlocks[block.successors[i]], static_cast<EdgeClass>(block.edgeClasses[i])) });
                output.switchOn(value, cases.span(), Edge(machineBlocks[block.successors.back()], static_cast<EdgeClass>(block.edgeClasses.back())));
                break;
            }
            case ProgramBlock::ReturnTerminator:
                output.ret(emitExpression(block.expression));
                break;
            case ProgramBlock::PatchTerminator: {
                PatchOperation* patch = emitPatch(block.patch, right);
                if (block.successors.size() && program.blocks[block.successors[0]].isPatchContinuation)
                    pendingPatch[block.successors[0]] = patch;
                break;
            }
            case ProgramBlock::UnreachableTerminator:
                output.oops();
                break;
            }
            valuesAtEnd[right] = currentValues;
            isEmitted[right] = 1;
            previous = right;
        }
    }
};

static ProcedureTraits traitsOfProgram(const Program& program, int check)
{
    ProcedureTraits traits;
    uint64_t bits = program.traitBits;
    bool throws = false, tails = false;
    for (const PatchDescription& description : program.patches) {
        throws |= description.kind == ThrowingPatch;
        tails |= description.kind == TailPatch;
    }
    traits.hasHandlers = throws || !(bits & 7);
    traits.savesFrameOnSuspend = program.suspends || !((bits >> 3) & 7);
    traits.savesRegistersOfCallers = !tails && !traits.hasHandlers && !traits.savesFrameOnSuspend && !((bits >> 6) & 15);
    traits.chargesForCalleeSaves = (bits >> 10) & 1;
    traits.mayOmitFrameRecord = (bits >> 11) & 3;
    traits.shouldUseFallbackBlockOrder = !((bits >> 13) & 7);
    traits.checksAllocation = check < 0 ? (bits >> 16) & 1 : check;
    if (program.usesHostFeatures)
        traits.features = CPUFeatureSet::host();
    return traits;
}

struct Outcome {
    std::string kind;
    std::string detail;
    unsigned runs { 0 }, exits { 0 };
    bool wasRefused { false };
};

static void fillPinnedConstants()
{
    for (uint64_t bits : bitsOfPinnedConstants()) {
        std::optional<PinnedConstant> pinned = findPinnedConstant(bits);
        if (pinned && pinned->offset)
            memcpy(testInstance + *pinned->offset, &bits, 8);
    }
}

static ProgramArguments argumentsOfInput(uint64_t seed, unsigned input)
{
    RandomNumbers random { seed * 77 + input * 1234567 + 5 };
    ProgramArguments arguments;
    static const uint64_t integers[] = { 0, 1, ~0ull, 2, 5, 13, 31, 100, 65537, 0x7fffffff, 0xffffffff80000000ull, 0x80000000ull, 0xffffffffull, 0x100000000ull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0xfffe000000000005ull, 0xfffe0000ffffffffull };
    for (uint64_t& bits : arguments.integers) {
        bits = random.chance(50) ? integers[random.pick(std::size(integers))] : random.next();
        if (random.chance(25))
            bits = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(bits)));
    }
    static const double doubles[] = { 0.0, -0.0, 1.0, -1.5, 2147483647.0, -2147483648.0, 2147483648.0, 1e300, 0.1, 4294967296.0, 9007199254740993.0 };
    for (uint64_t& bits : arguments.numbers) {
        bits = bitsFromDouble(random.chance(50) ? doubles[random.pick(std::size(doubles))] : static_cast<double>(static_cast<int64_t>(random.next())) / 4096);
        if (random.chance(8))
            bits = 0x7ff8000000000000ull;
        if (random.chance(6))
            bits = random.chance(50) ? 0x7ff0000000000000ull : 0xfff0000000000000ull;
    }
    return arguments;
}

static std::string describeFailure(const Procedure& procedure)
{
    StringPrintStream stream;
    const Failure& failure = *procedure.failure();
    stream.print(failure.kind, " at @", failure.operation, " ", failure.check.function, ":", failure.check.line);
    return toStdString(stream);
}

static Outcome runProgram(const Program& program, unsigned inputs, int check, bool show)
{
    Outcome outcome;
    std::vector<Trace> traces(inputs);
    std::vector<ProgramArguments> arguments(inputs);
    std::vector<Memory> starts(inputs);
    for (unsigned i = 0; i < inputs; ++i) {
        arguments[i] = argumentsOfInput(program.seed, i);
        fillMemory(starts[i], program.seed * 3 + i);
        fillConstantData(starts[i], program.seed);
        Interpreter interpreter(program);
        interpreter.run(arguments[i], starts[i], program.secondEntryBlock >= 0 && (i & 1));
        if (interpreter.trace.isTooLong) {
            outcome.wasRefused = true;
            return outcome;
        }
        traces[i] = interpreter.trace;
    }
    ProcedureTraits traits = traitsOfProgram(program, check);
    CompiledProgram compiled(traits);
    compiled.seed = program.seed;
    ProgramEmitter emitter(program, compiled);
    emitter.run();
    auto dump = [&](const char* what, void (*print)(PrintStream&, const Procedure&)) {
        StringPrintStream stream;
        print(stream, compiled.procedure);
        printf("---- %s\n%s\n", what, toStdString(stream).c_str());
    };
    if (show)
        dump("procedure", printProcedure);
    if (!compiled.procedure.failure())
        prepareForGeneration(compiled.procedure);
    if (compiled.procedure.failure()) {
        if (compiled.procedure.failure()->kind == FailureKind::OutOfRegisters) {
            outcome.wasRefused = true;
            return outcome;
        }
        outcome.kind = "REFUSED";
        outcome.detail = describeFailure(compiled.procedure);
        FailureKind kind = compiled.procedure.failure()->kind;
        if (show && (kind == FailureKind::WrongValueInRegister || kind == FailureKind::WrongValueInSpillSlot || kind == FailureKind::UpperHalfIsNotClear || kind == FailureKind::NarrowReferenceWithoutPointer)) {
            dump("order", printOrder);
            dump("liveness", printLiveness);
            dump("allocation", printAllocation);
        }
        return outcome;
    }
    if (show) {
        dump("order", printOrder);
        dump("liveness", printLiveness);
        dump("allocation", printAllocation);
        dump("frame", printFrame);
    }
    if (!compiled.compile(program.branchReach)) {
        outcome.kind = "REFUSED";
        outcome.detail = compiled.procedure.failure() ? describeFailure(compiled.procedure) : "link";
        return outcome;
    }
    if (show) {
        printf("---- code at %p, entry %p, %zu bytes\n", compiled.codeStart, compiled.entry, compiled.codeSize);
        fflush(stdout);
    }
    RegisterFile file = registerFileFor(traits);
    RegisterMask kept = (file.calleeSaved | pinnedRegisters());
    for (unsigned i = 0; i < numberOfGPRs; ++i) {
        if (!isSavable(static_cast<GPRReg>(i)))
            kept &= ~maskOf(Reg(static_cast<GPRReg>(i)));
    }
    kept &= ~maskOf(Reg(MacroAssembler::stackPointerRegister));
    for (unsigned i = 0; i < inputs; ++i) {
        memset(programStackTop - 4096, 0xa7, 4096 + 512);
        for (unsigned choice = 0; choice < SlotRegion; ++choice)
            memcpy(regionPointer(choice), starts[i].regions[choice], regionSize);
        RegisterArea& area = *registerArea;
        RandomNumbers random { program.seed + i * 99 };
        for (unsigned k = 0; k < 32; ++k) {
            area.entryGPRs[k] = random.next();
            area.entryFPRs[k] = random.next();
        }
        area.entryGPRs[GPRInfo::argumentGPR0] = std::bit_cast<uint64_t>(memoryBuffer);
        for (unsigned k = 0; k < 4; ++k)
            area.entryGPRs[GPRInfo::toArgumentRegister(1 + k)] = arguments[i].integers[k];
        for (unsigned k = 0; k < 3; ++k)
            area.entryFPRs[FPRInfo::toArgumentRegister(k)] = arguments[i].numbers[k];
        area.entryGPRs[instanceGPR] = std::bit_cast<uint64_t>(testInstance);
        area.entryGPRs[cellCageBaseGPR] = 0;
#if CPU(ARM64)
        area.entryGPRs[GPRInfo::numberTagRegister] = JSValue::NumberTag;
        area.entryGPRs[GPRInfo::notCellMaskRegister] = JSValue::NotCellMask;
#endif
        area.target = std::bit_cast<uint64_t>(program.secondEntryBlock >= 0 && (i & 1) ? compiled.secondEntry : compiled.entry);
        currentRun.stackDistance = 0;
        currentRun.codeStart = compiled.codeStart;
        currentRun.codeSize = compiled.codeSize;
        currentRun.trappingAccesses = &compiled.trappingAccesses;
        currentRun.expectedTrace = &traces[i];
        currentRun.eventIndex = 0;
        currentRun.error.clear();
        currentRun.didExit = false;
        currentRun.garbageState = program.seed + i;
        currentRun.isRunning = true;
        bool left = false;
        alarm(20);
        if (!sigsetjmp(currentRun.exitPoint, 1))
            reinterpret_cast<void (*)()>(untagCFunctionPtr<JSEntryPtrTag>(entryTrampoline->code().taggedPtr()))();
        else
            left = true;
        alarm(0);
        currentRun.isRunning = false;
        ++outcome.runs;
        const Trace& trace = traces[i];
        auto wrong = [&](const std::string& what) {
            outcome.kind = "DIFFERS";
            outcome.detail = "input " + std::to_string(i) + ": " + what;
        };
        if (left && !currentRun.error.empty()) {
            wrong(currentRun.error);
            return outcome;
        }
        if (left != trace.didExit) {
            wrong(left ? "leaves by an exit, expected to return" : "returns, expected to leave by an exit");
            return outcome;
        }
        if (currentRun.eventIndex != trace.events.size()) {
            wrong("ends after " + std::to_string(currentRun.eventIndex) + " events of " + std::to_string(trace.events.size()));
            return outcome;
        }
        if (left) {
            ++outcome.exits;
            continue;
        }
        if (area.exitGPRs[GPRInfo::returnValueGPR] != trace.result) {
            wrong("returns " + toHex(area.exitGPRs[GPRInfo::returnValueGPR]) + ", expected " + toHex(trace.result));
            return outcome;
        }
        std::string difference = findMemoryDifference(trace.finalMemory);
        if (difference != "none") {
            wrong("memory at the end differs at " + difference);
            return outcome;
        }
        if (area.exitStackPointer + currentRun.stackDistance != std::bit_cast<uint64_t>(programStackTop)) {
            wrong("the stack pointer differs by " + std::to_string(static_cast<int64_t>(area.exitStackPointer - std::bit_cast<uint64_t>(programStackTop))));
            return outcome;
        }
        for (unsigned k = regionSize; k < 512; ++k) {
            if (programStackTop[k] != 0xa7) {
                wrong("the word of the caller at " + std::to_string(k) + " is written");
                return outcome;
            }
        }
        bool isLost = false;
        forEachRegister(kept, [&](Reg reg) {
            uint64_t before = reg.isGPR() ? area.entryGPRs[reg.gpr()] : area.entryFPRs[reg.fpr()];
            uint64_t behind = reg.isGPR() ? area.exitGPRs[reg.gpr()] : area.exitFPRs[reg.fpr()];
            if (before != behind && !isLost) {
                isLost = true;
                StringPrintStream stream;
                stream.print(reg);
                wrong(std::string("the register of the caller ") + toStdString(stream) + " is lost");
            }
        });
        if (area.exitGPRs[GPRInfo::callFrameRegister] != area.entryGPRs[GPRInfo::callFrameRegister] && !isLost) {
            isLost = true;
            wrong("the frame pointer is lost");
        }
        if (isLost)
            return outcome;
    }
    return outcome;
}

static std::string describeExpression(const Program& program, int expression)
{
    if (expression < 0)
        return "-";
    const Expression& node = program.expressions[expression];
    static const char* const typeNames[] = { "v", "i", "l", "f", "d", "t" };
    std::string string = typeNames[node.type];
    auto name = [](Opcode opcode) { return std::string(descriptionOf(opcode).name.characters()); };
    auto describePlace = [&](const MemoryPlace& place) {
        static const char* const modes[] = { "", "zext", "sext", "i64", "narrow", "mul", "stepped", "sum" };
        std::string otherString = std::string(regionNames[place.region]) + (place.usesRootHeap ? "!top" : "") + (place.baseVariable >= 0 ? " of v" + std::to_string(place.baseVariable) : "") + "[";
        if (place.index >= 0)
            otherString += std::string(modes[place.mode]) + "(" + describeExpression(program, place.index) + " & " + std::to_string(place.mask) + " step " + std::to_string(place.step) + ") << " + std::to_string(place.scale) + " ";
        else if (place.mode == NarrowReference)
            otherString += "narrow ";
        return otherString + "+ " + std::to_string(place.offset) + "]";
    };
    switch (node.kind) {
    case Expression::Variable:
        return "v" + std::to_string(node.variable);
    case Expression::LinkedNumber:
        return "number" + std::to_string(node.bits);
    case Expression::Constant:
        return string + ":" + (node.type == Double ? std::to_string(doubleFromBits(node.bits)) : toHex(node.bits));
    case Expression::MemoryLoad:
        return name(node.opcode) + "." + string + describePlace(node.place);
    case Expression::SelectOperation:
        return "(" + describeExpression(program, node.children[0]) + " ? " + describeExpression(program, node.children[1]) + " : " + describeExpression(program, node.children[2]) + ")";
    case Expression::CanonicalNumber:
        return "Canon(" + describeExpression(program, node.children[0]) + ")";
    case Expression::BitCast:
        return "Cast." + string + "(" + describeExpression(program, node.children[0]) + ")";
    case Expression::OpaqueValue:
        return "Opaque(" + describeExpression(program, node.children[0]) + ")";
    case Expression::LowHalf:
        return "LowWord(" + describeExpression(program, node.children[0]) + ")";
    case Expression::NonNegativeExtension:
        return "ExtNN(" + describeExpression(program, node.children[0]) + ")";
    case Expression::ChillOperation:
        return "Chill" + name(node.opcode) + "(" + describeExpression(program, node.children[0]) + ", " + describeExpression(program, node.children[1]) + ")";
    case Expression::UnaryOperation:
        return name(node.opcode) + "(" + describeExpression(program, node.children[0]) + ")";
    case Expression::BinaryOperation:
        return name(node.opcode) + "(" + describeExpression(program, node.children[0]) + ", " + describeExpression(program, node.children[1]) + ")";
    }
    return "?";
}
static std::string describePatch(const Program& program, int patch)
{
    const PatchDescription& description = program.patches[patch];
    static const char* const kinds[] = { "WarmAny", "ColdAny", "LateColdAny", "SomeReg", "SomeEarly", "SomeLate", "Reg", "StackArg", "Stack", "Const" };
    std::string string = std::string(patchKindNames[description.kind]) + " #" + std::to_string(patch) + " reads " + std::to_string(description.reads) + " writes " + std::to_string(description.writes) + (description.mayCollect ? " collects" : "") + (description.exitsSideways ? " sideways" : "") + (description.writesLocalState ? " local" : "") + (description.isControlDependent ? " control" : "") + (description.runsWithoutFrame ? " frameless" : "") + (description.bringsOperands ? " brings" : "") + (description.fillsRegisterFromResult ? " fills" : "") + (description.needsFrameRecord ? " record" : "") + " scratch " + std::to_string(description.numberOfGPScratches) + "/" + std::to_string(description.numberOfFPScratches) + " early " + toHex(description.earlyClobbers) + " late " + toHex(description.lateClobbers) + " ->";
    for (const PatchResult& result : description.results)
        string += " v" + std::to_string(result.variable) + ":" + kinds[result.constraint] + (result.constraint == ValueRep::Register ? std::to_string(result.location) : "");
    string += " (";
    for (const PatchOperand& operand : description.operands)
        string += (operand.isSlotAddress ? std::string("&slot") : describeExpression(program, operand.expression)) + ":" + kinds[operand.constraint] + (operand.constraint == ValueRep::Register || operand.constraint == ValueRep::StackArgument ? std::to_string(operand.location) : "") + ", ";
    return string + ")";
}
static void printProgram(const Program& program)
{
    printf("seed %llu traits %llx features %d scopes %d reach %d vars:", static_cast<unsigned long long>(program.seed), static_cast<unsigned long long>(program.traitBits), program.usesHostFeatures, program.usesScopes, program.branchReach);
    static const char* const typeNames[] = { "v", "i", "l", "f", "d", "t" };
    for (unsigned variable = 0; variable < program.variableTypes.size(); ++variable)
        printf(" v%u:%s%s", variable, typeNames[program.variableTypes[variable]], program.mayHoldAnyBits[variable] ? "" : "~");
    printf("\n");
    std::vector<char> seen(program.blocks.size(), 0);
    std::vector<int> worklist { 0 };
    seen[0] = 1;
    if (program.secondEntryBlock >= 0) {
        worklist.push_back(program.secondEntryBlock);
        seen[program.secondEntryBlock] = 1;
        printf("second entry b%d\n", program.secondEntryBlock);
    }
    while (worklist.size()) {
        int right = worklist.back();
        worklist.pop_back();
        for (int successor : program.blocks[right].successors) {
            if (!seen[successor]) {
                seen[successor] = 1;
                worklist.push_back(successor);
            }
        }
    }
    for (unsigned block = 0; block < program.blocks.size(); ++block) {
        if (!seen[block])
            continue;
        const ProgramBlock& otherBlock = program.blocks[block];
        printf("b%u:\n", block);
        auto describePlace = [&](const MemoryPlace& place) {
            static const char* const modes[] = { "", "zext", "sext", "i64", "narrow", "mul", "stepped", "sum" };
            std::string string = std::string(regionNames[place.region]) + (place.usesRootHeap ? "!top" : "") + (place.baseVariable >= 0 ? " of v" + std::to_string(place.baseVariable) : "") + "[";
            if (place.index >= 0)
                string += std::string(modes[place.mode]) + "(" + describeExpression(program, place.index) + " & " + std::to_string(place.mask) + " step " + std::to_string(place.step) + ") << " + std::to_string(place.scale) + " ";
            else if (place.mode == NarrowReference)
                string += "narrow ";
            return string + "+ " + std::to_string(place.offset) + "]";
        };
        for (const Statement& statement : otherBlock.statements) {
            switch (statement.kind) {
            case Statement::Assignment:
                printf("  v%d = %s\n", statement.variable, describeExpression(program, statement.expression).c_str());
                break;
            case Statement::MemoryStore:
                printf("  %s %s <- %s\n", descriptionOf(statement.access).name.characters(), describePlace(statement.place).c_str(), describeExpression(program, statement.expression).c_str());
                break;
            case Statement::MemoryUpdate:
                printf("  %s %s %s= %s%s -> v%d%s\n", descriptionOf(statement.access).name.characters(), describePlace(statement.place).c_str(), descriptionOf(statement.opcode).name.characters(), describeExpression(program, statement.expression).c_str(), statement.hasConstantOnLeft ? " (left)" : "", statement.variable, statement.form & 1 ? " (old)" : "");
                break;
            case Statement::PatchCall:
                printf("  %s\n", describePatch(program, statement.patch).c_str());
                break;
            case Statement::ExitCheck:
                printf("  check %s: %s\n", describeExpression(program, statement.expression).c_str(), describePatch(program, statement.patch).c_str());
                break;
            case Statement::MemoryFence:
                printf("  fence\n");
                break;
            case Statement::PointerSelection:
                printf("  v%d = pointer %s\n", statement.variable, describeExpression(program, statement.expression).c_str());
                break;
            case Statement::NumberedRead:
                printf("  v%d = read %llu of %s off %s arms %u\n", statement.variable, static_cast<unsigned long long>(statement.key), describePlace(statement.place).c_str(), describeExpression(program, statement.expression).c_str(), statement.form);
                break;
            case Statement::LazyConstant:
                printf("  v%d = same %llu beside %s arms %u\n", statement.variable, static_cast<unsigned long long>(statement.key), describeExpression(program, statement.expression).c_str(), statement.form);
                break;
            case Statement::TrappingLoad:
                printf("  v%d = trapping #%d %s %s of v%d\n", statement.variable, statement.patch, descriptionOf(statement.access).name.characters(), describePlace(statement.place).c_str(), statement.place.baseVariable);
                break;
            case Statement::ResultsOfPatch:
                printf("  results of #%d\n", statement.patch);
                break;
            }
        }
        static const char* const names[] = { "jump", "branch", "switch", "ret", "patch", "oops" };
        printf("  %s %s", names[otherBlock.terminator], otherBlock.terminator == ProgramBlock::PatchTerminator ? describePatch(program, otherBlock.patch).c_str() : describeExpression(program, otherBlock.expression).c_str());
        for (unsigned i = 0; i < otherBlock.successors.size(); ++i)
            printf(" %sb%d%s", otherBlock.terminator == ProgramBlock::SwitchTerminator && i < otherBlock.caseValues.size() ? (std::to_string(otherBlock.caseValues[i]) + ":").c_str() : "", otherBlock.successors[i], otherBlock.edgeClasses[i] == 2 ? "(rare)" : otherBlock.edgeClasses[i] == 1 ? "(unlikely)"
                                                                                                                                                                                                                                                                                                 : "");
        printf("\n");
    }
}

static uint64_t registerInContext(void* context, GPRReg reg)
{
#if CPU(X86_64)
    static const int places[] = { REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI, REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15 };
    return static_cast<uint64_t>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[places[reg]]);
#else
    return static_cast<ucontext_t*>(context)->uc_mcontext.regs[reg];
#endif
}

static constexpr int handledSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGALRM, SIGABRT };

static void handleSignal(int signal, siginfo_t* info, void* context);

class ScopedSignalHandlers {
    WTF_MAKE_NONCOPYABLE(ScopedSignalHandlers);

public:
    ScopedSignalHandlers()
        : m_stack(FillWith {}, 1 << 18, 0)
    {
        stack_t stack {};
        stack.ss_size = m_stack.size();
        stack.ss_sp = m_stack.mutableSpan().data();
        sigaltstack(&stack, &m_previousStack);
        struct sigaction action {};
        action.sa_sigaction = handleSignal;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        for (size_t i = 0; i < std::size(handledSignals); ++i)
            sigaction(handledSignals[i], &action, &m_previousActions[i]);
    }

    ~ScopedSignalHandlers()
    {
        for (size_t i = 0; i < std::size(handledSignals); ++i)
            sigaction(handledSignals[i], &m_previousActions[i], nullptr);
        sigaltstack(&m_previousStack, nullptr);
    }

    const struct sigaction& previousActionOf(int signal) const
    {
        size_t i = 0;
        while (handledSignals[i] != signal)
            ++i;
        return m_previousActions[i];
    }

private:
    Vector<uint8_t> m_stack;
    stack_t m_previousStack {};
    std::array<struct sigaction, std::size(handledSignals)> m_previousActions {};
};

static const ScopedSignalHandlers* currentSignalHandlers;

static void handleSignal(int signal, siginfo_t* info, void* context)
{
    if (currentRun.isRunning) {
#if CPU(X86_64)
        uint64_t programCounter = static_cast<uint64_t>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
#else
        uint64_t programCounter = static_cast<ucontext_t*>(context)->uc_mcontext.pc;
#endif
        if (signal == SIGSEGV && programCounter >= std::bit_cast<uint64_t>(currentRun.codeStart) && programCounter < std::bit_cast<uint64_t>(currentRun.codeStart) + currentRun.codeSize) {
            uint32_t offset = static_cast<uint32_t>(programCounter - std::bit_cast<uint64_t>(currentRun.codeStart));
            for (const LinkedTrappingAccess& access : *currentRun.trappingAccesses) {
                if (access.codeOffset != offset)
                    continue;
                int patch = static_cast<int>(access.origin.bits()) - 1;
                currentRun.isRunning = false;
                currentRun.error.clear();
                if (currentRun.eventIndex >= currentRun.expectedTrace->events.size() || currentRun.expectedTrace->events[currentRun.eventIndex].patch != patch)
                    currentRun.error = "trap " + std::to_string(patch) + " at event " + std::to_string(currentRun.eventIndex) + ", expected " + (currentRun.eventIndex < currentRun.expectedTrace->events.size() ? std::to_string(currentRun.expectedTrace->events[currentRun.eventIndex].patch) : "the end");
                else {
                    const TraceEvent& event = currentRun.expectedTrace->events[currentRun.eventIndex++];
                    uint64_t reference = registerInContext(context, access.reference);
                    if (access.referenceIsNarrow)
                        reference = static_cast<uint32_t>(reference);
                    if (event.memoryHash != currentMemoryHash())
                        currentRun.error = "memory differs at trap " + std::to_string(patch);
                    else if (reference)
                        currentRun.error = "the reference of trap " + std::to_string(patch) + " is " + toHex(reference);
                }
                siglongjmp(currentRun.exitPoint, 1);
            }
        }
        currentRun.isRunning = false;
        currentRun.error = std::string(signal == SIGALRM ? "does not end" : "signal ") + std::to_string(signal) + " at " + toHex(programCounter) + " address " + toHex(std::bit_cast<uint64_t>(info->si_addr)) + " after " + std::to_string(currentRun.eventIndex) + " events";
        siglongjmp(currentRun.exitPoint, 1);
    }
    sigaction(signal, &currentSignalHandlers->previousActionOf(signal), nullptr);
}

static void prepareProcess()
{
    if (registerArea)
        return;
    registerArea = static_cast<RegisterArea*>(mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    uint8_t* runtimeStack = static_cast<uint8_t*>(mmap(nullptr, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    runtimeStackTop = runtimeStack + (1 << 20) - 256;
    programStackBase = static_cast<uint8_t*>(mmap(nullptr, programStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    programStackTop = programStackBase + programStackSize - 4096;
    for (uintptr_t hint = 0x20000000; !memoryBuffer && hint < 0x700000000; hint += 0x1000000) {
        void* mapping = mmap(reinterpret_cast<void*>(hint), 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reinterpret_cast<uintptr_t>(mapping) < 0x700000000)
            memoryBuffer = static_cast<uint8_t*>(mapping) + 4096;
        else
            munmap(mapping, 65536);
    }
    RELEASE_ASSERT(memoryBuffer);
    uint8_t* instance = static_cast<uint8_t*>(mmap(nullptr, 1 << 18, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    testInstance = instance + (1 << 17);
    fillPinnedConstants();
    programHeaps = new ProgramHeaps;
    compileEntryTrampoline();
}

static Outcome runSeed(uint64_t seed, const ExecutionFuzzRequest& request, bool show)
{
    prepareProcess();
    Program program = generateProgram(seed);
    if (show)
        printProgram(program);
    ScopedSignalHandlers handlers;
    SetForScope current(currentSignalHandlers, &handlers);
    return runProgram(program, request.inputs, request.checksAllocation ? *request.checksAllocation : -1, show);
}

} // namespace ExecutionFuzzer

String fuzzOneExecution(uint64_t seed, const ExecutionFuzzRequest& request)
{
    ExecutionFuzzer::Outcome outcome = ExecutionFuzzer::runSeed(seed, request, false);
    if (outcome.kind.empty())
        return {};
    return String::fromLatin1((outcome.kind + ": " + outcome.detail).c_str());
}

unsigned fuzzExecution(const ExecutionFuzzRequest& request, PrintStream& out)
{
    if (request.seedToShow) {
        ExecutionFuzzer::Outcome outcome = ExecutionFuzzer::runSeed(*request.seedToShow, request, true);
        out.println("seed ", *request.seedToShow, ": ", outcome.kind.empty() ? "passed" : outcome.kind.c_str(), " ", outcome.detail.c_str());
        return !outcome.kind.empty();
    }
    unsigned numberOfFailures = 0;
    for (unsigned index = 0; index < request.count; ++index) {
        uint64_t seed = request.seed + index;
        String failure = fuzzOneExecution(seed, request);
        if (failure.isNull())
            continue;
        out.println("FAIL seed ", seed, " ", failure);
        out.flush();
        ++numberOfFailures;
    }
    return numberOfFailures;
}

}
}
} // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT) && OS(LINUX) && !ASAN_ENABLED
