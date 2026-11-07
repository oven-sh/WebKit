/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineFrameForms.h"

#if ENABLE(AOT)

#include "AOTImageFormat.h"
#include "AOTMachineSlots.h"
#include "StackAlignment.h"
#include <bit>
#include <limits>
#include <optional>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Machine {

static constexpr uint64_t bytesOfFrameFormWord = sizeof(CPURegister);
static constexpr uint64_t reachOfOneByteOfOffsetInFrameForm = 128;

static_assert(stackAlignmentBytes() == 2 * bytesOfFrameFormWord, "below a return address an odd number of words leaves the stack pointer aligned");
static_assert(reachOfOneByteOfOffsetInFrameForm <= maxBytesAllocatedBeforeStackCheck && reachOfOneByteOfOffsetInFrameForm / bytesOfFrameFormWord <= ImageFrame::maxCalleeSavesStart, "a frame with its saves at the bottom is allocated in front of its stack check, and ImageFrame can name the start of the run");

static uint64_t roundUpBytesOfFrameForm(uint64_t bytes)
{
    return WTF::roundUpToMultipleOf(stackAlignmentBytes(), bytes);
}

static FormOfFrame usualFormOfFrame(const Procedure& proc, uint64_t numberOfSaves)
{
    const uint64_t bytesOfSaves = roundUpBytesOfFrameForm(numberOfSaves * bytesOfFrameFormWord);
    const uint64_t bytesBelowFrameRecord = roundUpBytesOfFrameForm(bytesOfSaves + proc.slots().bytesInAll() + proc.callArgAreaSizeInBytes());
    RELEASE_ASSERT(bytesBelowFrameRecord <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max()));

    FormOfFrame form;
    form.topOfSlots = -static_cast<int32_t>(bytesOfSaves);
    form.offsetOfFirstSave = -static_cast<int32_t>(numberOfSaves * bytesOfFrameFormWord);
    form.bytesBelowFrameRecord = static_cast<uint32_t>(bytesBelowFrameRecord);
    return form;
}

static bool namesFewPlacesInFrame(const Procedure& proc)
{
    constexpr unsigned mostPlacesNamedOffStackPointer = 8;
    unsigned numberOfPlaces = 0;
    for (const Step& step : proc.allocation().steps()) {
        numberOfPlaces += step.destination.isSpill();
        for (Location source : step.sources)
            numberOfPlaces += source.isSpill();
        if (numberOfPlaces > mostPlacesNamedOffStackPointer)
            return false;
    }
    return true;
}

static std::optional<FormOfFrame> formOfFrameWithoutRecord(const Procedure& proc, uint64_t numberOfSaves)
{
    const ProcedureTraits& traits = proc.traits();
    if (!isX86_64() || !traits.mayOmitFrameRecord || traits.setsUpFrameAtEntry() || traits.savesRegistersOfCallers)
        return std::nullopt;
    if (proc.namesFramePointer() || !proc.stackSlots().empty() || proc.allocation().holdsPatchThatCountsOnFrameRecord())
        return std::nullopt;
    if (proc.allocation().holdsPatchWithOperandInFrame() && !namesFewPlacesInFrame(proc))
        return std::nullopt;

    const uint64_t bytesOfSlots = proc.slots().bytesInAll();
    const uint64_t bytesOfOutgoingWords = proc.callArgAreaSizeInBytes();

    const uint64_t bytesInAll = numberOfSaves * bytesOfFrameFormWord + bytesOfSlots + bytesOfOutgoingWords;
    const uint64_t bytesBelowFrameRecord = roundUpBytesOfFrameForm(bytesInAll > bytesOfFrameFormWord ? bytesInAll - bytesOfFrameFormWord : 0);
    if (bytesBelowFrameRecord > maxBytesAllocatedBeforeStackCheck)
        return std::nullopt;

    FormOfFrame form;
    form.hasFrameRecord = false;
    form.topOfSlots = static_cast<int32_t>(bytesOfFrameFormWord) - static_cast<int32_t>(numberOfSaves * bytesOfFrameFormWord);
    form.offsetOfFirstSave = form.topOfSlots;
    form.bytesBelowFrameRecord = static_cast<uint32_t>(bytesBelowFrameRecord);
    return form;
}

static std::optional<FormOfFrame> formOfFrameWithSavesAtBottom(const Procedure& proc, uint64_t numberOfSaves)
{
    const ProcedureTraits& traits = proc.traits();
    if (!isX86_64() || !numberOfSaves || traits.savesFrameOnSuspend || traits.savesRegistersOfCallers)
        return std::nullopt;
    if (!proc.slots().bytesInAll() || proc.callArgAreaSizeInBytes())
        return std::nullopt;
    if (roundUpBytesOfFrameForm(numberOfSaves * bytesOfFrameFormWord) + proc.slots().bytesInAll() > reachOfOneByteOfOffsetInFrameForm)
        return std::nullopt;

    const uint64_t bytesBelowFrameRecord = roundUpBytesOfFrameForm(proc.slots().bytesInAll() + numberOfSaves * bytesOfFrameFormWord);

    FormOfFrame form;
    form.savesLieAtBottom = true;
    form.offsetOfFirstSave = -static_cast<int32_t>(bytesBelowFrameRecord);
    form.bytesBelowFrameRecord = static_cast<uint32_t>(bytesBelowFrameRecord);
    return form;
}

FormOfFrame chooseFormOfFrame(const Procedure& proc, FrameCoverage coverage, RegisterMask saves)
{
    const uint64_t numberOfSaves = static_cast<uint64_t>(std::popcount(saves));
    if (coverage == FrameCoverage::None)
        return usualFormOfFrame(proc, numberOfSaves);
    if (std::optional<FormOfFrame> form = formOfFrameWithoutRecord(proc, numberOfSaves))
        return *form;
    if (std::optional<FormOfFrame> form = formOfFrameWithSavesAtBottom(proc, numberOfSaves))
        return *form;
    return usualFormOfFrame(proc, numberOfSaves);
}

static bool operationNamesFrameOfSomeForm(const Operation* value)
{
    if (!value->hasNoPosition())
        return false;
    if (value->opcode() == FramePointer || value->opcode() == SlotBase)
        return true;
    for (const Operation* child : value->children()) {
        if (operationNamesFrameOfSomeForm(child))
            return true;
    }
    return false;
}

bool stepCountsOnFrameRecord(const Procedure& proc, const Step& step)
{
    if (step.kind == StepKind::Materialize)
        return operationNamesFrameOfSomeForm(step.operation);
    if (step.kind != StepKind::Execute)
        return false;

    const PatchOperation* patch = step.operation->as<PatchOperation>();
    if (patch && patch->onlyKeepsChildrenAlive)
        return false;

    bool countsOnRecord = patch && patch->needsFrameRecord();
    forEachLeaf(proc.liveness(), step.operation, [&](const Operation*, unsigned, const Operation* leaf) {
        countsOnRecord |= operationNamesFrameOfSomeForm(leaf);
    });
    if (!patch || step.patchAssignment == UINT32_MAX)
        return countsOnRecord;
    for (Location location : proc.allocation().patchAssignment(step.patchAssignment).locations)
        countsOnRecord |= (location.isSpill() && !patch->findsOperandsInFrameWithoutRecord()) || location.kind() == Location::SlotBytes;
    return countsOnRecord;
}

#if !CPU(X86_64)
void emitFrameSetup(CCallHelpers& jit, const Procedure& proc, std::span<const GPRReg> registersPushedIntoFirstSlots)
{
    RELEASE_ASSERT(registersPushedIntoFirstSlots.empty());
    emitFrameSetup(jit, proc);
}

void emitTeardownOfFrameThatPopsWords(CCallHelpers&, const Frame&, std::span<const GPRReg>)
{
    RELEASE_ASSERT_NOT_REACHED();
}
#endif

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
