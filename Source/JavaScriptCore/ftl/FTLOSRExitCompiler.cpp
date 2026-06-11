/*
 * Copyright (C) 2013-2022 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "FTLOSRExitCompiler.h"

#if ENABLE(FTL_JIT)

#include "AssemblyHelpersSpoolers.h"
#include "BytecodeStructs.h"
#include "CheckpointOSRExitSideState.h"
#include "DFGOSRExitCompilerCommon.h"
#include "FTLJITCode.h"
#include "FTLLocation.h"
#include "FTLOSRExit.h"
#include "FTLOperations.h"
#include "FTLSaveRestore.h"
#include "FTLState.h"
#include "JSCJSValueInlines.h"
#include "LinkBuffer.h"
#include "MaxFrameExtentForSlowPathCall.h"
#include "OperandsInlines.h"
#include "ProbeContext.h"
#include "VMLite.h"
#include <limits>
#include <wtf/Lock.h>
#include <wtf/ScopedLambda.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

// UNGIL U-T3 emitter (defined in jit/AssemblyHelpers.cpp). Self-declaration,
// mirroring that TU's own pattern — no header owns this form yet.
void loadVMLite(AssemblyHelpers&, GPRReg);

namespace FTL {

using namespace DFG;

// UNGIL U-T4b: gilOff, N threads can fire the SAME not-yet-compiled exit
// concurrently; exit.m_code must be published exactly once.
// Coarse process-wide lock — exit-stub compilation is a once-per-exit slow
// path. Lock RANK (NOT a leaf — it is held across the whole of compileStub):
// OUTERMOST of everything compileStub acquires, i.e. OUTER to
// codeBlock->m_lock (ConcurrentJSLocker, getArrayProfile),
// ScratchBufferRegistry::m_lock -> VMLiteRegistry lock -> per-lite
// scratchBufferLock (via VM::allocateBakedScratchBufferIndex, §LK.6 chain),
// and LinkBuffer/executable-allocator locks. It is acquired ONLY from the
// exit-generation thunk's operation call with no other JSC lock held, and
// must never be taken while holding any of the above. GIL-on never takes it
// (flag-off identity). Sibling with the same rank:
// ftlLazySlowPathGenerationLock (FTLOperations.cpp); the two are never
// nested. Rank recorded in INTEGRATE-ungil.md (U-T4b entry).
static Lock ftlOSRExitGenerationLock;

// UNGIL §A.1.6 (ANNEX A16, U-T4b): addressing of the exit's scratch memory,
// in both modes. GIL-on (baked == false): today's baked absolute pointers,
// byte-for-byte. gilOff (baked == true): the stub bakes only a process-wide
// ScratchBufferRegistry INDEX; the CURRENT lite's dataBuffer() is
// materialized into a GPR right before each use (loadVMLite -> segment ->
// [index] -> +offsetof(m_buffer); rematerialization per §A.1.2) and all
// scratch addressing is (base GPR + static offset). By convention
// GPRInfo::regT3 holds the data base across compileRecovery calls — the
// caller materializes it and compileRecovery never clobbers it.
struct ExitScratchAddressing {
    bool baked { false };
    unsigned bakedIndex { std::numeric_limits<unsigned>::max() };
    EncodedJSValue* scratch { nullptr }; // GIL-on dataBuffer(); null when baked.
    ptrdiff_t materializationPointersOffset { 0 };
    ptrdiff_t materializationArgumentsOffset { 0 };
    ptrdiff_t registerScratchOffset { 0 };
    ptrdiff_t unwindScratchOffset { 0 };

    char* registerScratch() const
    {
        ASSERT(!baked);
        return std::bit_cast<char*>(scratch) + registerScratchOffset;
    }
    EncodedJSValue* materializationPointer(unsigned slot) const
    {
        ASSERT(!baked);
        return std::bit_cast<EncodedJSValue*>(std::bit_cast<char*>(scratch) + materializationPointersOffset) + slot;
    }
    void materializeDataBase(AssemblyHelpers& jit, GPRReg dest) const
    {
        ASSERT(baked);
        materializeBakedScratchBufferDataPointer(jit, bakedIndex, dest);
    }
};

static void reboxAccordingToFormat(
    DataFormat format, AssemblyHelpers& jit, GPRReg value, GPRReg scratch1, GPRReg scratch2)
{
    switch (format) {
    case DataFormatInt32: {
        jit.zeroExtend32ToWord(value, value);
        jit.or64(GPRInfo::numberTagRegister, value);
        break;
    }

    case DataFormatInt52: {
        jit.rshift64(AssemblyHelpers::TrustedImm32(JSValue::int52ShiftAmount), value);
        jit.moveDoubleTo64(FPRInfo::fpRegT0, scratch2);
        jit.boxInt52(value, value, scratch1, FPRInfo::fpRegT0);
        jit.move64ToDouble(scratch2, FPRInfo::fpRegT0);
        break;
    }

    case DataFormatStrictInt52: {
        jit.moveDoubleTo64(FPRInfo::fpRegT0, scratch2);
        jit.boxInt52(value, value, scratch1, FPRInfo::fpRegT0);
        jit.move64ToDouble(scratch2, FPRInfo::fpRegT0);
        break;
    }

    case DataFormatBoolean: {
        jit.zeroExtend32ToWord(value, value);
        jit.or32(MacroAssembler::TrustedImm32(JSValue::ValueFalse), value);
        break;
    }

    case DataFormatJS: {
        // Done already!
        break;
    }

    case DataFormatDouble: {
        jit.moveDoubleTo64(FPRInfo::fpRegT0, scratch1);
        jit.move64ToDouble(value, FPRInfo::fpRegT0);
        jit.purifyNaN(FPRInfo::fpRegT0, FPRInfo::fpRegT0);
        jit.boxDouble(FPRInfo::fpRegT0, value);
        jit.move64ToDouble(scratch1, FPRInfo::fpRegT0);
        break;
    }

    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
}

// Baked mode (addressing.baked): the caller guarantees GPRInfo::regT3 holds
// the current lite's exit-buffer dataBuffer() at entry; this function only
// reads regT3 (writes regT0; rebox uses regT0-regT2 and fpRegT0).
static void compileRecovery(
    CCallHelpers& jit, const ExitValue& value,
    const FixedVector<B3::ValueRep>& valueReps,
    const ExitScratchAddressing& addressing,
    const UncheckedKeyHashMap<ExitTimeObjectMaterialization*, unsigned>& materializationToSlot)
{
    switch (value.kind()) {
    case ExitValueDead: {
        EncodedJSValue deadValue = Options::poisonDeadOSRExitVariables() ? poisonedDeadOSRExitValue : encodedJSUndefined();
        jit.move(MacroAssembler::TrustedImm64(deadValue), GPRInfo::regT0);
        break;
    }

    case ExitValueConstant:
        jit.move(MacroAssembler::TrustedImm64(JSValue::encode(value.constant())), GPRInfo::regT0);
        break;

    case ExitValueArgument:
        if (addressing.baked) [[unlikely]] {
            Location::forValueRep(valueReps[value.exitArgument().argument()]).restoreInto(
                jit, GPRInfo::regT3, addressing.registerScratchOffset, GPRInfo::regT0);
        } else {
            Location::forValueRep(valueReps[value.exitArgument().argument()]).restoreInto(
                jit, addressing.registerScratch(), GPRInfo::regT0);
        }
        break;

    case ExitValueInJSStack:
    case ExitValueInJSStackAsInt32:
    case ExitValueInJSStackAsInt52:
    case ExitValueInJSStackAsDouble:
        jit.load64(AssemblyHelpers::addressFor(value.virtualRegister()), GPRInfo::regT0);
        break;

    case ExitValueMaterializeNewObject:
        if (addressing.baked) [[unlikely]] {
            jit.loadPtr(
                CCallHelpers::Address(
                    GPRInfo::regT3,
                    static_cast<int32_t>(addressing.materializationPointersOffset + materializationToSlot.get(value.objectMaterialization()) * sizeof(EncodedJSValue))),
                GPRInfo::regT0);
        } else
            jit.loadPtr(addressing.materializationPointer(materializationToSlot.get(value.objectMaterialization())), GPRInfo::regT0);
        break;

    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }

    reboxAccordingToFormat(
        value.dataFormat(), jit, GPRInfo::regT0, GPRInfo::regT1, GPRInfo::regT2);
}

static void compileStub(VM& vm, unsigned exitID, JITCode* jitCode, OSRExit& exit, CodeBlock* codeBlock)
{
    // This code requires framePointerRegister is the same as callFrameRegister
    static_assert(MacroAssembler::framePointerRegister == GPRInfo::callFrameRegister, "MacroAssembler::framePointerRegister and GPRInfo::callFrameRegister must be the same");

    CCallHelpers jit(codeBlock);

    if (Options::printEachOSRExit()) [[unlikely]] {
        SpeculationFailureDebugInfo* debugInfo = new SpeculationFailureDebugInfo;
        debugInfo->codeBlock = jit.codeBlock();
        debugInfo->kind = exit.m_kind;
        debugInfo->exitIndex = exitID;
        debugInfo->bytecodeIndex = exit.m_codeOrigin.bytecodeIndex();
        jit.probe(tagCFunction<JITProbePtrTag>(operationDebugPrintSpeculationFailure), debugInfo);
    }

    // The first thing we need to do is restablish our frame in the case of an exception.
    if (exit.isGenericUnwindHandler()) {
        if (vm.gilOff()) [[unlikely]] {
            // UNGIL §A.1.3 (U-T4b): callFrameForCatch and topEntryFrame are
            // per-lite Group-3 state gilOff. Host-side, this stub compiles on
            // the exiting thread (its lite is current) — read through the
            // mode-split accessor; emitted-side, the stub is shared by every
            // thread that exits here — resolve the CURRENT lite.
            RELEASE_ASSERT(vm.group3Primitives().callFrameForCatch); // The first time we hit this exit, like at all other times, this field should be non-null.
            restoreCalleeSavesFromCurrentVMLiteEntryFrameCalleeSavesBuffer(jit);
            loadVMLite(jit, MacroAssembler::framePointerRegister);
            jit.loadPtr(
                CCallHelpers::Address(
                    MacroAssembler::framePointerRegister,
                    static_cast<int32_t>(VMLite::offsetOfPrimitives() + VMLitePrimitives::offsetOf_callFrameForCatch())),
                MacroAssembler::framePointerRegister);
        } else {
            RELEASE_ASSERT(vm.callFrameForCatch); // The first time we hit this exit, like at all other times, this field should be non-null.
            jit.restoreCalleeSavesFromEntryFrameCalleeSavesBuffer(vm.topEntryFrame);
            jit.loadPtr(vm.addressOfCallFrameForCatch(), MacroAssembler::framePointerRegister);
        }
        jit.addPtr(CCallHelpers::TrustedImm32(codeBlock->stackPointerOffset() * sizeof(Register)),
            MacroAssembler::framePointerRegister, CCallHelpers::stackPointerRegister);

        // Do a pushToSave because that's what the exit compiler below expects the stack
        // to look like because that's the last thing the ExitThunkGenerator does. The code
        // below doesn't actually use the value that was pushed, but it does rely on the
        // general shape of the stack being as it is in the non-exception OSR case.
        jit.pushToSaveImmediateWithoutTouchingRegisters(CCallHelpers::TrustedImm32(0xbadbeef));
    }

    // We need scratch space to save all registers, to build up the JS stack, to deal with unwind
    // fixup, pointers to all of the objects we materialize, and the elements inside those objects
    // that we materialize.
    
    // Figure out how much space we need for those object allocations.
    unsigned numMaterializations = 0;
    size_t maxMaterializationNumArguments = 0;
    for (ExitTimeObjectMaterialization* materialization : exit.m_descriptor->m_materializations) {
        numMaterializations++;
        
        maxMaterializationNumArguments = std::max(
            maxMaterializationNumArguments,
            materialization->properties().size());
    }
    
    const size_t scratchBufferSize =
        sizeof(EncodedJSValue) * (exit.m_descriptor->m_values.size() + numMaterializations + maxMaterializationNumArguments) +
        requiredScratchMemorySizeInBytes() +
        codeBlock->jitCode()->calleeSaveRegisters()->sizeOfAreaInBytes();

    // UNGIL §A.1.6 (ANNEX A16, U-T4b): see ExitScratchAddressing. gilOff,
    // this stub is shared by every thread that takes the exit, so the scratch
    // memory is reached through a baked registry INDEX (per-lite buffers)
    // instead of baked absolute pointers. GIL-on is byte-for-byte today's
    // emission.
    ExitScratchAddressing addressing;
    addressing.baked = vm.gilOff();
    addressing.materializationPointersOffset = sizeof(EncodedJSValue) * exit.m_descriptor->m_values.size();
    addressing.materializationArgumentsOffset = addressing.materializationPointersOffset + sizeof(EncodedJSValue) * numMaterializations;
    addressing.registerScratchOffset = addressing.materializationArgumentsOffset + sizeof(EncodedJSValue) * maxMaterializationNumArguments;
    addressing.unwindScratchOffset = addressing.registerScratchOffset + requiredScratchMemorySizeInBytes();

    ScratchBuffer* scratchBuffer = nullptr;
    if (addressing.baked) [[unlikely]]
        addressing.bakedIndex = vm.allocateBakedScratchBufferIndex(scratchBufferSize);
    else {
        scratchBuffer = vm.scratchBufferForSize(scratchBufferSize);
        addressing.scratch = scratchBuffer ? static_cast<EncodedJSValue*>(scratchBuffer->dataBuffer()) : nullptr;
    }

    // GIL-on absolute views; null (and unused) in baked mode.
    EncodedJSValue* scratch = addressing.scratch;
    EncodedJSValue* materializationPointers = nullptr;
    EncodedJSValue* materializationArguments = nullptr;
    char* registerScratch = nullptr;
    uint64_t* unwindScratch = nullptr;
    if (!addressing.baked) {
        materializationPointers = scratch + exit.m_descriptor->m_values.size();
        materializationArguments = materializationPointers + numMaterializations;
        registerScratch = std::bit_cast<char*>(materializationArguments + maxMaterializationNumArguments);
        unwindScratch = std::bit_cast<uint64_t*>(registerScratch + requiredScratchMemorySizeInBytes());
    }

    UncheckedKeyHashMap<ExitTimeObjectMaterialization*, unsigned> materializationToSlot;
    unsigned materializationCount = 0;
    for (ExitTimeObjectMaterialization* materialization : exit.m_descriptor->m_materializations)
        materializationToSlot.add(materialization, materializationCount++);

    auto recoverValue = [&] (const ExitValue& value) {
        compileRecovery(
            jit, value,
            exit.m_valueReps,
            addressing, materializationToSlot);
    };

    // Note that we come in here, the stack used to be as B3 left it except that someone called pushToSave().
    // We don't care about the value they saved. But, we do appreciate the fact that they did it, because we use
    // that slot for saveAllRegisters().

    if (addressing.baked) [[unlikely]] {
        saveAllRegisters(jit, scopedLambda<void(AssemblyHelpers&, GPRReg)>(
            [&] (AssemblyHelpers& jit, GPRReg baseGPR) {
                addressing.materializeDataBase(jit, baseGPR);
                jit.addPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(addressing.registerScratchOffset)), baseGPR);
            }));
    } else
        saveAllRegisters(jit, registerScratch);

    if constexpr (validateDFGDoesGC) {
        if (Options::validateDoesGC()) {
            // We're about to exit optimized code. So, there's no longer any optimized
            // code running that expects no GC. We need to set this before object
            // materialization below.

            // Even though we set Heap::m_doesGC in compileFTLOSRExit(), we also need
            // to set it here because compileFTLOSRExit() is only called on the first time
            // we exit from this site, but all subsequent exits will take this compiled
            // ramp without calling compileFTLOSRExit() first.
            if (vm.gilOff()) [[unlikely]] {
                // UNGIL AB18-C: repeated exits never re-enter
                // compileFTLOSRExit, so a baked &m_doesGC store would leave
                // the exiting thread's lite slot holding the last per-node
                // expectation — write the CURRENT thread's lite slot (same
                // rationale as the DFGOSRExit.cpp ramp split). regT0 is dead
                // here: saveAllRegisters captured the live state above and
                // the popToRestore below overwrites it. Two imm32 stores for
                // uniformity with the per-node split (this Special encoding
                // fits imm32 only by encoding accident; imm64-through-scratch
                // is a wild store on x86_64).
                DoesGCCheck check;
                check.u.encoded = DoesGCCheck::encode(true, DoesGCCheck::Special::FTLOSRExit);
                loadVMLite(jit, GPRInfo::regT0);
                jit.store32(CCallHelpers::TrustedImm32(check.u.other), CCallHelpers::Address(GPRInfo::regT0, static_cast<int32_t>(VMLite::offsetOfDoesGC() + OBJECT_OFFSETOF(DoesGCCheck, u.other))));
                jit.store32(CCallHelpers::TrustedImm32(check.u.nodeIndex), CCallHelpers::Address(GPRInfo::regT0, static_cast<int32_t>(VMLite::offsetOfDoesGC() + OBJECT_OFFSETOF(DoesGCCheck, u.nodeIndex))));
            } else
                jit.store64(CCallHelpers::TrustedImm64(DoesGCCheck::encode(true, DoesGCCheck::Special::FTLOSRExit)), vm.addressOfDoesGC());
        }
    }

    // Bring the stack back into a sane form and assert that it's sane.
    jit.popToRestore(GPRInfo::regT0);
    jit.checkStackPointerAlignment();
    
    if (vm.m_perBytecodeProfiler && jitCode->dfgCommon()->compilation) [[unlikely]] {
        Profiler::Database& database = *vm.m_perBytecodeProfiler;
        Profiler::Compilation* compilation = jitCode->dfgCommon()->compilation.get();
        
        Profiler::OSRExit* profilerExit = compilation->addOSRExit(
            exitID, Profiler::OriginStack(database, codeBlock, exit.m_codeOrigin),
            exit.m_kind, exit.m_kind == UncountableInvalidation);
        jit.add64(CCallHelpers::TrustedImm32(1), CCallHelpers::AbsoluteAddress(profilerExit->counterAddress()));
    }

    // The remaining code assumes that SP/FP are in the same state that they were in the FTL's
    // call frame.
    
    // Get the call frame and tag thingies.
    // Restore the exiting function's callFrame value into a regT4
    jit.emitMaterializeTagCheckRegisters();
    
    // Do some value profiling.
    if (exit.m_descriptor->m_profileDataFormat != DataFormatNone) {
        if (addressing.baked) [[unlikely]] {
            addressing.materializeDataBase(jit, GPRInfo::regT3);
            Location::forValueRep(exit.m_valueReps[0]).restoreInto(jit, GPRInfo::regT3, addressing.registerScratchOffset, GPRInfo::regT0);
        } else
            Location::forValueRep(exit.m_valueReps[0]).restoreInto(jit, registerScratch, GPRInfo::regT0);
        reboxAccordingToFormat(exit.m_descriptor->m_profileDataFormat, jit, GPRInfo::regT0, GPRInfo::regT1, GPRInfo::regT2);
        
        if (exit.m_kind == BadCache || exit.m_kind == BadIndexingType) {
            CodeOrigin codeOrigin = exit.m_codeOriginForExitProfile;
            CodeBlock* codeBlock = jit.baselineCodeBlockFor(codeOrigin);
            if (ArrayProfile* arrayProfile = codeBlock->getArrayProfile(ConcurrentJSLocker(codeBlock->m_lock), codeOrigin.bytecodeIndex())) {
                jit.move(CCallHelpers::TrustedImmPtr(arrayProfile), GPRInfo::regT3);
                jit.load32(MacroAssembler::Address(GPRInfo::regT0, JSCell::structureIDOffset()), GPRInfo::regT1);
                jit.store32(GPRInfo::regT1, CCallHelpers::Address(GPRInfo::regT3, ArrayProfile::offsetOfSpeculationFailureStructureID()));

                jit.load8(MacroAssembler::Address(GPRInfo::regT0, JSCell::typeInfoTypeOffset()), GPRInfo::regT2);
                jit.sub32(MacroAssembler::TrustedImm32(FirstTypedArrayType), GPRInfo::regT2);
                auto notTypedArray = jit.branch32(MacroAssembler::AboveOrEqual, GPRInfo::regT2, MacroAssembler::TrustedImm32(NumberOfTypedArrayTypesExcludingDataView));
                jit.move(MacroAssembler::TrustedImmPtr(typedArrayModes), GPRInfo::regT1);
                jit.load32(MacroAssembler::BaseIndex(GPRInfo::regT1, GPRInfo::regT2, MacroAssembler::TimesFour), GPRInfo::regT2);
                auto storeArrayModes = jit.jump();

                notTypedArray.link(&jit);
                jit.load8(MacroAssembler::Address(GPRInfo::regT0, JSCell::indexingTypeAndMiscOffset()), GPRInfo::regT1);
                jit.and32(MacroAssembler::TrustedImm32(IndexingModeMask), GPRInfo::regT1);
                jit.lshift32(MacroAssembler::TrustedImm32(1), GPRInfo::regT1, GPRInfo::regT2);
                storeArrayModes.link(&jit);
                jit.or32(GPRInfo::regT2, CCallHelpers::Address(GPRInfo::regT3, ArrayProfile::offsetOfArrayModes()));
            }
        }

        if (exit.m_descriptor->m_valueProfile)
            exit.m_descriptor->m_valueProfile.emitReportValue(jit, jit.codeBlock(), JSValueRegs(GPRInfo::regT0), GPRInfo::regT1);
    }

    // Materialize all objects. Don't materialize an object until all
    // of the objects it needs have been materialized. We break cycles
    // by populating objects late - we only consider an object as
    // needing another object if the later is needed for the
    // allocation of the former.

    UncheckedKeyHashSet<ExitTimeObjectMaterialization*> toMaterialize;
    for (ExitTimeObjectMaterialization* materialization : exit.m_descriptor->m_materializations)
        toMaterialize.add(materialization);

    while (!toMaterialize.isEmpty()) {
        unsigned previousToMaterializeSize = toMaterialize.size();

        Vector<ExitTimeObjectMaterialization*> worklist;
        worklist.appendRange(toMaterialize.begin(), toMaterialize.end());
        for (ExitTimeObjectMaterialization* materialization : worklist) {
            // Check if we can do anything about this right now.
            bool allGood = true;
            for (ExitPropertyValue value : materialization->properties()) {
                if (!value.value().isObjectMaterialization())
                    continue;
                if (!value.location().neededForMaterialization())
                    continue;
                if (toMaterialize.contains(value.value().objectMaterialization())) {
                    // Gotta skip this one, since it needs a
                    // materialization that hasn't been materialized.
                    allGood = false;
                    break;
                }
            }
            if (!allGood)
                continue;

            // All systems go for materializing the object. First we
            // recover the values of all of its fields and then we
            // call a function to actually allocate the beast.
            // We only recover the fields that are needed for the allocation.
            if (addressing.baked) [[unlikely]]
                addressing.materializeDataBase(jit, GPRInfo::regT3); // compileRecovery's base; rematerialized per materialization (calls below clobber it).
            for (unsigned propertyIndex = materialization->properties().size(); propertyIndex--;) {
                const ExitPropertyValue& property = materialization->properties()[propertyIndex];
                if (!property.location().neededForMaterialization())
                    continue;

                ASSERT(property.value().kind() != ExitValueDead);
                recoverValue(property.value());
                if (addressing.baked) [[unlikely]]
                    jit.storePtr(GPRInfo::regT0, CCallHelpers::Address(GPRInfo::regT3, static_cast<int32_t>(addressing.materializationArgumentsOffset + propertyIndex * sizeof(EncodedJSValue))));
                else
                    jit.storePtr(GPRInfo::regT0, materializationArguments + propertyIndex);
            }

            static_assert(FunctionTraits<decltype(operationMaterializeObjectInOSR)>::arity < GPRInfo::numberOfArgumentRegisters, "This call assumes that we don't pass arguments on the stack.");
            if (addressing.baked) [[unlikely]] {
                jit.addPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(addressing.materializationArgumentsOffset)), GPRInfo::regT3);
                jit.setupArguments<decltype(operationMaterializeObjectInOSR)>(
                    CCallHelpers::TrustedImmPtr(codeBlock->globalObjectFor(materialization->origin())),
                    CCallHelpers::TrustedImmPtr(materialization),
                    GPRInfo::regT3);
            } else {
                jit.setupArguments<decltype(operationMaterializeObjectInOSR)>(
                    CCallHelpers::TrustedImmPtr(codeBlock->globalObjectFor(materialization->origin())),
                    CCallHelpers::TrustedImmPtr(materialization),
                    CCallHelpers::TrustedImmPtr(materializationArguments));
            }
            jit.prepareCallOperation(vm);
            jit.move(CCallHelpers::TrustedImmPtr(tagCFunction<OperationPtrTag>(operationMaterializeObjectInOSR)), GPRInfo::nonArgGPR0);
            jit.call(GPRInfo::nonArgGPR0, OperationPtrTag);
            if (addressing.baked) [[unlikely]] {
                addressing.materializeDataBase(jit, GPRInfo::regT1);
                jit.storePtr(
                    GPRInfo::returnValueGPR,
                    CCallHelpers::Address(GPRInfo::regT1, static_cast<int32_t>(addressing.materializationPointersOffset + materializationToSlot.get(materialization) * sizeof(EncodedJSValue))));
            } else
                jit.storePtr(GPRInfo::returnValueGPR, addressing.materializationPointer(materializationToSlot.get(materialization)));

            // Let everyone know that we're done.
            toMaterialize.remove(materialization);
        }
        
        // We expect progress! This ensures that we crash rather than looping infinitely if there
        // is something broken about this fixpoint. Or, this could happen if we ever violate the
        // "materializations form a DAG" rule.
        RELEASE_ASSERT(toMaterialize.size() < previousToMaterializeSize);
    }

    // Now that all the objects have been allocated, we populate them
    // with the correct values. This time we can recover all the
    // fields, including those that are only needed for the allocation.
    for (ExitTimeObjectMaterialization* materialization : exit.m_descriptor->m_materializations) {
        if (addressing.baked) [[unlikely]]
            addressing.materializeDataBase(jit, GPRInfo::regT3); // compileRecovery's base; rematerialized per materialization (the call below clobbers it).
        for (unsigned propertyIndex = materialization->properties().size(); propertyIndex--;) {
            recoverValue(materialization->properties()[propertyIndex].value());
            if (addressing.baked) [[unlikely]]
                jit.storePtr(GPRInfo::regT0, CCallHelpers::Address(GPRInfo::regT3, static_cast<int32_t>(addressing.materializationArgumentsOffset + propertyIndex * sizeof(EncodedJSValue))));
            else
                jit.storePtr(GPRInfo::regT0, materializationArguments + propertyIndex);
        }

        static_assert(FunctionTraits<decltype(operationPopulateObjectInOSR)>::arity < GPRInfo::numberOfArgumentRegisters, "This call assumes that we don't pass arguments on the stack.");
        if (addressing.baked) [[unlikely]] {
            addressing.materializeDataBase(jit, GPRInfo::regT2);
            jit.addPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(addressing.materializationPointersOffset + materializationToSlot.get(materialization) * sizeof(EncodedJSValue))), GPRInfo::regT2);
            jit.addPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(addressing.materializationArgumentsOffset)), GPRInfo::regT3);
            jit.setupArguments<decltype(operationPopulateObjectInOSR)>(
                CCallHelpers::TrustedImmPtr(codeBlock->globalObjectFor(materialization->origin())),
                CCallHelpers::TrustedImmPtr(materialization),
                GPRInfo::regT2,
                GPRInfo::regT3);
        } else {
            jit.setupArguments<decltype(operationPopulateObjectInOSR)>(
                CCallHelpers::TrustedImmPtr(codeBlock->globalObjectFor(materialization->origin())),
                CCallHelpers::TrustedImmPtr(materialization),
                CCallHelpers::TrustedImmPtr(addressing.materializationPointer(materializationToSlot.get(materialization))),
                CCallHelpers::TrustedImmPtr(materializationArguments));
        }
        jit.prepareCallOperation(vm);
        jit.move(CCallHelpers::TrustedImmPtr(tagCFunction<OperationPtrTag>(operationPopulateObjectInOSR)), GPRInfo::nonArgGPR0);
        jit.call(GPRInfo::nonArgGPR0, OperationPtrTag);
    }

    // Save all state from wherever the exit data tells us it was, into the appropriate place in
    // the scratch buffer. This also does the reboxing.
    
    {
        std::optional<GPRReg> undefinedGPR;
        if (addressing.baked) [[unlikely]]
            addressing.materializeDataBase(jit, GPRInfo::regT3); // Lives across the loop; compileRecovery/restoreInto never clobber it.
        else
            jit.move(CCallHelpers::TrustedImmPtr(scratch), GPRInfo::regT3);
        CCallHelpers::CopySpooler spooler(jit, CCallHelpers::framePointerRegister, GPRInfo::regT3, GPRInfo::regT0, GPRInfo::regT1);
        for (unsigned index = exit.m_descriptor->m_values.size(); index--;) {
            auto& value = exit.m_descriptor->m_values[index];
            if (value.dataFormat() == DataFormatJS) {
                switch (value.kind()) {
                case ExitValueDead:
                    if (Options::poisonDeadOSRExitVariables()) [[unlikely]] {
                        spooler.moveConstant(poisonedDeadOSRExitValue);
                        spooler.storeGPR(index * sizeof(EncodedJSValue));
                        break;
                    }

                    if (!undefinedGPR) [[unlikely]] {
                        jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsUndefined())), GPRInfo::regT4);
                        undefinedGPR = GPRInfo::regT4;
                    }
                    spooler.copyGPR(undefinedGPR.value());
                    spooler.storeGPR(index * sizeof(EncodedJSValue));
                    break;

                case ExitValueConstant: {
                    EncodedJSValue currentConstant = JSValue::encode(value.constant());
                    if (currentConstant == encodedJSUndefined()) {
                        if (!undefinedGPR) [[unlikely]] {
                            jit.move(CCallHelpers::TrustedImm64(JSValue::encode(jsUndefined())), GPRInfo::regT4);
                            undefinedGPR = GPRInfo::regT4;
                        }
                        spooler.copyGPR(undefinedGPR.value());
                    } else
                        spooler.moveConstant(currentConstant);
                    spooler.storeGPR(index * sizeof(EncodedJSValue));
                    break;
                }

                case ExitValueArgument:
                    if (addressing.baked) [[unlikely]]
                        Location::forValueRep(exit.m_valueReps[value.exitArgument().argument()]).restoreInto(jit, GPRInfo::regT3, addressing.registerScratchOffset, GPRInfo::regT0);
                    else
                        Location::forValueRep(exit.m_valueReps[value.exitArgument().argument()]).restoreInto(jit, registerScratch, GPRInfo::regT0);
                    jit.store64(GPRInfo::regT0, CCallHelpers::Address(GPRInfo::regT3, index * sizeof(EncodedJSValue)));
                    break;

                case ExitValueInJSStack:
                case ExitValueInJSStackAsInt32:
                case ExitValueInJSStackAsInt52:
                case ExitValueInJSStackAsDouble:
                    spooler.loadGPR(value.virtualRegister().offset() * sizeof(EncodedJSValue));
                    spooler.storeGPR(index * sizeof(EncodedJSValue));
                    break;

                case ExitValueMaterializeNewObject:
                    if (addressing.baked) [[unlikely]]
                        jit.loadPtr(CCallHelpers::Address(GPRInfo::regT3, static_cast<int32_t>(addressing.materializationPointersOffset + materializationToSlot.get(value.objectMaterialization()) * sizeof(EncodedJSValue))), GPRInfo::regT0);
                    else
                        jit.loadPtr(addressing.materializationPointer(materializationToSlot.get(value.objectMaterialization())), GPRInfo::regT0);
                    jit.store64(GPRInfo::regT0, CCallHelpers::Address(GPRInfo::regT3, index * sizeof(EncodedJSValue)));
                    break;

                default:
                    RELEASE_ASSERT_NOT_REACHED();
                    break;
                }
            } else {
                recoverValue(value);
                jit.store64(GPRInfo::regT0, CCallHelpers::Address(GPRInfo::regT3, index * sizeof(EncodedJSValue)));
            }
        }
        spooler.finalizeGPR();
    }

    // The scratch buffer can become the sole retainer of saved on-stack values, so set the
    // active length for the GC. (Baked mode: per-lite buffers are GC-scanned
    // through the registry walk via each lite's ownership list, jit R2 — the
    // active length lives at offset 0 of the resolved ScratchBuffer.)
    if (addressing.baked) [[unlikely]] {
        materializeBakedScratchBufferPointer(jit, addressing.bakedIndex, GPRInfo::regT0);
        jit.storePtr(CCallHelpers::TrustedImm32(scratchBufferSize), CCallHelpers::Address(GPRInfo::regT0));
    } else if (scratchBuffer) {
        jit.move(CCallHelpers::TrustedImmPtr(scratchBuffer->addressOfActiveLength()), GPRInfo::regT0);
        jit.storePtr(CCallHelpers::TrustedImm32(scratchBufferSize), CCallHelpers::Address(GPRInfo::regT0));
    }

    // Henceforth we make it look like the exiting function was called through a register
    // preservation wrapper. This implies that FP must be nudged down by a certain amount. Then
    // we restore the various things according to either exit.m_descriptor->m_values or by copying from the
    // old frame, and finally we save the various callee-save registers into where the
    // restoration thunk would restore them from.
    
    // Before we start messing with the frame, we need to set aside any registers that the
    // FTL code was preserving.
    {
        constexpr GPRReg srcBufferGPR = GPRInfo::regT2;
        constexpr GPRReg destBufferGPR = GPRInfo::regT3;
        jit.move(CCallHelpers::framePointerRegister, srcBufferGPR);
        if (addressing.baked) [[unlikely]] {
            addressing.materializeDataBase(jit, destBufferGPR);
            jit.addPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(addressing.unwindScratchOffset)), destBufferGPR);
        } else
            jit.move(CCallHelpers::TrustedImmPtr(unwindScratch), destBufferGPR);
        CCallHelpers::CopySpooler spooler(CCallHelpers::CopySpooler::BufferRegs::AllowModification, jit, srcBufferGPR, destBufferGPR, GPRInfo::regT0, GPRInfo::regT1);
        for (unsigned i = codeBlock->jitCode()->calleeSaveRegisters()->registerCount(); i--;) {
            RegisterAtOffset entry = codeBlock->jitCode()->calleeSaveRegisters()->at(i);
            spooler.loadGPR(entry.offset());
            spooler.storeGPR(i * sizeof(uint64_t));
        }
        spooler.finalizeGPR();
    }
    
    CodeBlock* baselineCodeBlock = jit.baselineCodeBlockFor(exit.m_codeOrigin);

    // First set up SP so that our data doesn't get clobbered by signals.
    unsigned conservativeStackDelta =
        (exit.m_descriptor->m_values.numberOfLocals() + CodeBlock::calleeSaveSpaceAsVirtualRegisters(*baselineCodeBlock->jitCode()->calleeSaveRegisters())) * sizeof(Register) +
        maxFrameExtentForSlowPathCall;
    conservativeStackDelta = WTF::roundUpToMultipleOf(
        stackAlignmentBytes(), conservativeStackDelta);
    jit.addPtr(
        MacroAssembler::TrustedImm32(-conservativeStackDelta),
        MacroAssembler::framePointerRegister, MacroAssembler::stackPointerRegister);
    jit.checkStackPointerAlignment();

    {
        auto allFTLCalleeSaves = RegisterSet::ftlCalleeSaveRegisters();
        const RegisterAtOffsetList* baselineCalleeSaves = baselineCodeBlock->jitCode()->calleeSaveRegisters();
        auto iterateCalleeSavesImpl = [&](auto check, auto func) {
            for (Reg reg = Reg::first(); reg <= Reg::last(); reg = reg.next()) {
                if (!allFTLCalleeSaves.contains(reg, IgnoreVectors))
                    continue;
                if (!check(reg))
                    continue;
                unsigned unwindIndex = codeBlock->jitCode()->calleeSaveRegisters()->indexOf(reg);
                const RegisterAtOffset* baselineRegisterOffset = baselineCalleeSaves->find(reg);
                func(reg, unwindIndex, baselineRegisterOffset);
            }
        };

        auto iterateGPRCalleeSaves = [&](auto func) {
            iterateCalleeSavesImpl([](Reg reg) { return reg.isGPR(); }, func);
        };

        auto iterateFPRCalleeSaves = [&](auto func) {
            iterateCalleeSavesImpl([](Reg reg) { return reg.isFPR(); }, func);
        };

        {
            // unwindIndex == UINT_MAX indicates that the FTL compilation didn't preserve these registers.
            // This means that it also didn't use them. Their values at the beginning of OSR exit should
            // be the ones to retain. We saved all registers into the register scratch buffer at the beginning
            // of the thunk. So we can restore them from there.
            ASSERT(!allFTLCalleeSaves.contains(GPRInfo::regT3, IgnoreVectors));
            ASSERT(!allFTLCalleeSaves.contains(GPRInfo::regT0, IgnoreVectors));
            ASSERT(!allFTLCalleeSaves.contains(GPRInfo::regT1, IgnoreVectors));
            ASSERT(!allFTLCalleeSaves.contains(FPRInfo::fpRegT0, IgnoreVectors));
            ASSERT(!allFTLCalleeSaves.contains(FPRInfo::fpRegT1, IgnoreVectors));
            if (addressing.baked) [[unlikely]] {
                addressing.materializeDataBase(jit, GPRInfo::regT3);
                jit.addPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(addressing.registerScratchOffset)), GPRInfo::regT3);
            } else
                jit.move(CCallHelpers::TrustedImmPtr(registerScratch), GPRInfo::regT3);
            {
                // Load from registerScratch buffer to callee-save registers.
                CCallHelpers::LoadRegSpooler spooler(jit, GPRInfo::regT3);
                iterateGPRCalleeSaves([&](Reg reg, unsigned unwindIndex, const RegisterAtOffset* baselineRegisterOffset) {
                    if (unwindIndex == UINT_MAX && !baselineRegisterOffset)
                        spooler.loadGPR({ reg, static_cast<ptrdiff_t>(offsetOfReg(reg)), conservativeWidthWithoutVectors(reg) });
                });
                spooler.finalizeGPR();
                iterateFPRCalleeSaves([&](Reg reg, unsigned unwindIndex, const RegisterAtOffset* baselineRegisterOffset) {
                    if (unwindIndex == UINT_MAX && !baselineRegisterOffset)
                        spooler.loadFPR({ reg, static_cast<ptrdiff_t>(offsetOfReg(reg)), conservativeWidthWithoutVectors(reg) });
                });
                spooler.finalizeFPR();
            }
            {
                // Copy from registerScratch buffer to call frame.
                CCallHelpers::CopySpooler spooler(jit, GPRInfo::regT3, CCallHelpers::framePointerRegister, GPRInfo::regT0, GPRInfo::regT1, FPRInfo::fpRegT0, FPRInfo::fpRegT1);
                iterateGPRCalleeSaves([&](Reg reg, unsigned unwindIndex, const RegisterAtOffset* baselineRegisterOffset) {
                    if (unwindIndex == UINT_MAX && baselineRegisterOffset) {
                        spooler.loadGPR(offsetOfReg(reg));
                        spooler.storeGPR(baselineRegisterOffset->offset());
                    }
                });
                spooler.finalizeGPR();
                iterateFPRCalleeSaves([&](Reg reg, unsigned unwindIndex, const RegisterAtOffset* baselineRegisterOffset) {
                    if (unwindIndex == UINT_MAX && baselineRegisterOffset) {
                        spooler.loadFPR(offsetOfReg(reg));
                        spooler.storeFPR(baselineRegisterOffset->offset());
                    }
                });
                spooler.finalizeFPR();
            }
        }
        {
            // The FTL compilation preserved these registers. Their new values are therefore irrelevant,
            // but we can get their values that were preserved by using the unwind data. We've already
            // copied all unwind-able preserved registers into the unwind scratch buffer, so we can get
            // the values to restore from there.
            ASSERT(static_cast<size_t>(addressing.unwindScratchOffset - addressing.registerScratchOffset) == requiredScratchMemorySizeInBytes());
            jit.addPtr(CCallHelpers::TrustedImm32(requiredScratchMemorySizeInBytes()), GPRInfo::regT3); // Change registerScratch to unwindScratch.
            {
                // Load from unwindScratch buffer to callee-save registers.
                CCallHelpers::LoadRegSpooler spooler(jit, GPRInfo::regT3);
                iterateGPRCalleeSaves([&](Reg reg, unsigned unwindIndex, const RegisterAtOffset* baselineRegisterOffset) {
                    if (unwindIndex != UINT_MAX && !baselineRegisterOffset)
                        spooler.loadGPR({ reg, static_cast<ptrdiff_t>(unwindIndex * sizeof(uint64_t)), conservativeWidthWithoutVectors(reg) });
                });
                spooler.finalizeGPR();
                iterateFPRCalleeSaves([&](Reg reg, unsigned unwindIndex, const RegisterAtOffset* baselineRegisterOffset) {
                    if (unwindIndex != UINT_MAX && !baselineRegisterOffset)
                        spooler.loadFPR({ reg, static_cast<ptrdiff_t>(unwindIndex * sizeof(uint64_t)), conservativeWidthWithoutVectors(reg) });
                });
                spooler.finalizeFPR();
            }
            {
                // Copy from unwindScratch buffer to call frame.
                CCallHelpers::CopySpooler spooler(jit, GPRInfo::regT3, CCallHelpers::framePointerRegister, GPRInfo::regT0, GPRInfo::regT1, FPRInfo::fpRegT0, FPRInfo::fpRegT1);
                iterateGPRCalleeSaves([&](Reg, unsigned unwindIndex, const RegisterAtOffset* baselineRegisterOffset) {
                    if (unwindIndex != UINT_MAX && baselineRegisterOffset) {
                        spooler.loadGPR(static_cast<ptrdiff_t>(unwindIndex * sizeof(uint64_t)));
                        spooler.storeGPR(baselineRegisterOffset->offset());
                    }
                });
                spooler.finalizeGPR();
                iterateFPRCalleeSaves([&](Reg, unsigned unwindIndex, const RegisterAtOffset* baselineRegisterOffset) {
                    if (unwindIndex != UINT_MAX && baselineRegisterOffset) {
                        spooler.loadFPR(static_cast<ptrdiff_t>(unwindIndex * sizeof(uint64_t)));
                        spooler.storeFPR(baselineRegisterOffset->offset());
                    }
                });
                spooler.finalizeFPR();
            }
        }
    }

    size_t baselineVirtualRegistersForCalleeSaves = CodeBlock::calleeSaveSpaceAsVirtualRegisters(*baselineCodeBlock->jitCode()->calleeSaveRegisters());

    if (exit.m_kind == WillThrowOutOfMemoryError) {
        jit.store32(CCallHelpers::TrustedImm32(exit.m_exitCallSiteIndex.bits()), CCallHelpers::tagFor(CallFrameSlot::argumentCountIncludingThis));
        jit.setupArguments<decltype(operationThrowOutOfMemoryError)>(CCallHelpers::TrustedImmPtr(&vm));
        jit.prepareCallOperation(vm);
        jit.move(AssemblyHelpers::TrustedImmPtr(tagCFunction<OperationPtrTag>(operationThrowOutOfMemoryError)), GPRInfo::nonArgGPR0);
        jit.call(GPRInfo::nonArgGPR0, OperationPtrTag);
    }

    if (exit.m_codeOrigin.inlineStackContainsActiveCheckpoint()) {
        if (addressing.baked) [[unlikely]] {
            addressing.materializeDataBase(jit, GPRInfo::regT2);
            jit.addPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(sizeof(EncodedJSValue) * exit.m_descriptor->m_values.tmpIndex(0))), GPRInfo::regT2);
            jit.setupArguments<decltype(operationMaterializeOSRExitSideState)>(CCallHelpers::TrustedImmPtr(&vm), CCallHelpers::TrustedImmPtr(&exit), GPRInfo::regT2);
        } else {
            EncodedJSValue* tmpScratch = scratch + exit.m_descriptor->m_values.tmpIndex(0);
            jit.setupArguments<decltype(operationMaterializeOSRExitSideState)>(CCallHelpers::TrustedImmPtr(&vm), CCallHelpers::TrustedImmPtr(&exit), CCallHelpers::TrustedImmPtr(tmpScratch));
        }
        jit.prepareCallOperation(vm);
        jit.move(AssemblyHelpers::TrustedImmPtr(tagCFunction<OperationPtrTag>(operationMaterializeOSRExitSideState)), GPRInfo::nonArgGPR0);
        jit.call(GPRInfo::nonArgGPR0, OperationPtrTag);
    }

    // Now get state out of the scratch buffer and place it back into the stack. The values are
    // already reboxed so we just move them.
    {
        constexpr GPRReg srcBufferGPR = GPRInfo::regT2;
        constexpr GPRReg destBufferGPR = GPRInfo::regT3;
        if (addressing.baked) [[unlikely]]
            addressing.materializeDataBase(jit, srcBufferGPR);
        else
            jit.move(CCallHelpers::TrustedImmPtr(scratch), srcBufferGPR);
        jit.move(GPRInfo::callFrameRegister, destBufferGPR);
        CCallHelpers::CopySpooler spooler(CCallHelpers::CopySpooler::BufferRegs::AllowModification, jit, srcBufferGPR, destBufferGPR, GPRInfo::regT0, GPRInfo::regT1);
        for (unsigned index = exit.m_descriptor->m_values.size(); index--;) {
            Operand operand = exit.m_descriptor->m_values.operandForIndex(index);

            if (operand.isTmp())
                continue;

            if (operand.isLocal() && operand.toLocal() < static_cast<int>(baselineVirtualRegistersForCalleeSaves))
                continue;

            spooler.loadGPR(index * sizeof(EncodedJSValue));
            spooler.storeGPR(operand.virtualRegister().offset() * sizeof(EncodedJSValue));
        }
        spooler.finalizeGPR();
    }

    if (addressing.baked) [[unlikely]] {
        materializeBakedScratchBufferPointer(jit, addressing.bakedIndex, GPRInfo::regT0);
        jit.storePtr(CCallHelpers::TrustedImm32(0), CCallHelpers::Address(GPRInfo::regT0));
    } else if (scratchBuffer) {
        jit.move(CCallHelpers::TrustedImmPtr(scratchBuffer->addressOfActiveLength()), GPRInfo::regT0);
        jit.storePtr(CCallHelpers::TrustedImm32(0), CCallHelpers::Address(GPRInfo::regT0));
    }

    handleExitCounts(vm, jit, exit);
    reifyInlinedCallFrames(jit, exit);
    adjustAndJumpToTarget(vm, jit, exit);
    
    LinkBuffer patchBuffer(jit, codeBlock, LinkBuffer::Profile::FTLOSRExit);
    exit.m_code = FINALIZE_CODE_IF(
        shouldDumpDisassembly() || Options::verboseOSR() || Options::verboseFTLOSRExit(),
        patchBuffer, OSRExitPtrTag, nullptr,
        "FTL OSR exit #%u (D@%u, %s, %s) from %s, with operands = %s",
            exitID, exit.m_dfgNodeIndex, toCString(exit.m_codeOrigin).data(),
            toCString(exit.m_kind).data(), toCString(*codeBlock).data(),
            toCString(ignoringContext<DumpContext>(exit.m_descriptor->m_values)).data()
        );
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationCompileFTLOSRExit, void*, (CallFrame* callFrame, unsigned exitID))
{
    dataLogLnIf(shouldDumpDisassembly() || Options::verboseOSR() || Options::verboseFTLOSRExit(), "Compiling OSR exit with exitID = ", exitID);

    VM& vm = callFrame->deprecatedVM();
    // Don't need an ActiveScratchBufferScope here because we DeferGCForAWhile below.

    if constexpr (validateDFGDoesGC) {
        // We're about to exit optimized code. So, there's no longer any optimized
        // code running that expects no GC.
        vm.setDoesGCExpectation(true, DoesGCCheck::Special::FTLOSRExit);
    }

    // UNGIL §A.1.3: read through the mode-split accessor — gilOff the live
    // callFrameForCatch is the current lite's, not the inert VM block's.
    if (vm.group3Primitives().callFrameForCatch)
        RELEASE_ASSERT(vm.group3Primitives().callFrameForCatch == callFrame);
    
    CodeBlock* codeBlock = callFrame->codeBlock();
    
    ASSERT(codeBlock);
    ASSERT(codeBlock->jitType() == JITType::FTLJIT);
    
    // It's sort of preferable that we don't GC while in here. Anyways, doing so wouldn't
    // really be profitable.
    DeferGCForAWhile deferGC(vm);

    JITCode* jitCode = codeBlock->jitCode()->ftl();
    OSRExit& exit = jitCode->m_osrExit[exitID];
    
    if (shouldDumpDisassembly() || Options::verboseOSR() || Options::verboseFTLOSRExit()) {
        dataLogLn(
            "    Owning block: ", pointerDump(codeBlock), "\n",
            "    Origin: ", exit.m_codeOrigin);
        if (exit.m_codeOriginForExitProfile != exit.m_codeOrigin)
            dataLogLn("    Origin for exit profile: ", exit.m_codeOriginForExitProfile);
        dataLogLn(
            "    Current call site index: ", callFrame->callSiteIndex().bits(), "\n",
            "    Exit is exception handler: ", exit.isExceptionHandler(), "\n",
            "    Is unwind handler: ", exit.isGenericUnwindHandler(), "\n",
            "    Exit values: ", exit.m_descriptor->m_values, "\n",
            "    Value reps: ", listDump(exit.m_valueReps));
        if (!exit.m_descriptor->m_materializations.isEmpty()) {
            dataLogLn("    Materializations:");
            for (ExitTimeObjectMaterialization* materialization : exit.m_descriptor->m_materializations)
                dataLogLn("        ", pointerDump(materialization));
        }
    }

    if (vm.gilOff()) [[unlikely]] {
        // UNGIL U-T4b: N threads can race to compile the SAME exit. Compile
        // exactly once under the generation lock; losers reuse the winner's
        // stub. We deliberately do NOT repatch the exit jump in this mode:
        // other mutators may be concurrently EXECUTING that jump, and
        // MacroAssembler::repatchJump rewrites an unaligned rel32 on x86_64
        // with no atomicity guarantee (torn fetch -> wild jump); ISB1 only
        // licenses patching performed inside a stop. The jump keeps pointing
        // at the generation thunk, which calls back here and tail-calls our
        // return value — the protocol stays data-only. Exits are rare and
        // trigger reoptimization, so the extra thunk round-trips are noise.
        Locker locker { ftlOSRExitGenerationLock };
        if (!exit.m_code)
            compileStub(vm, exitID, jitCode, exit, codeBlock);
        return exit.m_code.code().taggedPtr();
    }

    compileStub(vm, exitID, jitCode, exit, codeBlock);

    MacroAssembler::repatchJump(
        exit.codeLocationForRepatch(codeBlock), CodeLocationLabel<OSRExitPtrTag>(exit.m_code.code()));

    return exit.m_code.code().taggedPtr();
}

} } // namespace JSC::FTL

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(FTL_JIT)
