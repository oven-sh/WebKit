/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineDescription.h"

#if ENABLE(AOT)

#include "AOTMachineFrame.h"
#include "StackAlignment.h"
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Machine {

static void describeFrameRanges(const Procedure& procedure, const LinkBuffer& linkBuffer, decltype(FrameDescription::frameRanges)& ranges)
{
    uint32_t endOfPreviousRange = 0;
    for (const FrameRange& range : procedure.frameRanges()) {
        uint32_t begin = describedOffsetOf(linkBuffer, range.begin);
        uint32_t end = describedOffsetOf(linkBuffer, range.end);
        RELEASE_ASSERT(endOfPreviousRange <= begin && begin <= end, endOfPreviousRange, begin, end);
        endOfPreviousRange = end;

        if (begin == end)
            continue;
        if (!ranges.isEmpty() && ranges.last().end == begin) {
            ranges.last().end = end;
            continue;
        }
        ranges.append(CodeRange { begin, end });
    }
}

void describeListedConstants(const Procedure& procedure, decltype(RareListsOfBody::constants)& constants)
{
    std::span<const ConstantDataRecord> records = procedure.constantData();
    constants.reserveCapacity(records.size());
    for (const ConstantDataRecord& record : records) {
        RELEASE_ASSERT(!record.bytes.empty());
        RELEASE_ASSERT(hasOneBitSet(record.alignment) && record.alignment <= imageConstantsAlignment, record.alignment);
        constants.append(ConstantData { decltype(ConstantData::bytes)(record.bytes), record.alignment, record.kind });
    }
}

void describeListedJumpTables(const Procedure& procedure, const LinkBuffer& linkBuffer, decltype(RareListsOfBody::jumpTables)& jumpTables)
{
    std::span<const JumpTableRecord> records = procedure.jumpTables();
    jumpTables.reserveCapacity(records.size());
    for (const JumpTableRecord& record : records) {
        AOT::JumpTable table;
        table.targets.reserveInitialCapacity(record.targets.size());
        for (MacroAssembler::Label target : record.targets)
            table.targets.append(describedOffsetOf(linkBuffer, target));
        jumpTables.append(WTF::move(table));
    }
}

void describeFrame(const Procedure& procedure, const LinkBuffer& linkBuffer, FrameDescription& description)
{
    RELEASE_ASSERT(!procedure.failure());
    const Frame& frame = procedure.frame();
    decltype(FrameDescription::frameRanges)& ranges = description.frameRanges;
    ranges.clear();
    describeFrameRanges(procedure, linkBuffer, ranges);

    RELEASE_ASSERT(frame.hasFrame() || (ranges.isEmpty() && !frame.frameSize() && !frame.bytesDeadAtSuspensions()));
    RELEASE_ASSERT(!ranges.isEmpty() || !frame.numberOfCalleeSaves());
    RELEASE_ASSERT(!(frame.frameSize() % stackAlignmentBytes()), frame.frameSize());
    RELEASE_ASSERT(frame.bytesDeadAtSuspensions() <= frame.frameSize(), frame.bytesDeadAtSuspensions(), frame.frameSize());

    description.frameSizeInBytes = frame.frameSize();
    description.bytesAboveFrameRecord = 0;
    description.bytesDeadAtSuspensions = frame.bytesDeadAtSuspensions();
    description.hasFrameRecord = frame.hasFrameRecord();
    if (frame.numberOfCalleeSaves())
        description.calleeSaveRegisters = frame.calleeSaveRegisterAtOffsetList();
    else
        description.calleeSaveRegisters = { };
}

void describeListedTrappingAccesses(const Procedure& procedure, const LinkBuffer& linkBuffer, Vector<LinkedTrappingAccess>& accesses)
{
    std::span<const TrappingAccess> records = procedure.trappingAccesses();
    accesses.reserveCapacity(accesses.size() + records.size());
    for (const TrappingAccess& record : records) {
        uint32_t codeOffset = describedOffsetOf(linkBuffer, record.instruction);
        RELEASE_ASSERT(accesses.isEmpty() || accesses.last().codeOffset < codeOffset, codeOffset);
        RELEASE_ASSERT(record.reference != InvalidGPRReg);
        accesses.append(LinkedTrappingAccess { codeOffset, record.origin, record.reference, record.referenceIsNarrow });
    }
}

enum class DescribedRangesPartAt : bool { Origins, Operations };

static void describeRangesOfCode(const Procedure& procedure, const LinkBuffer& linkBuffer, DescribedRangesPartAt partAt, Vector<LinkedRangeOfOrigin>& ranges)
{
    RELEASE_ASSERT(ranges.isEmpty());

    std::span<const RangeOfOrigin> records = procedure.rangesOfOrigins();
    RELEASE_ASSERT(procedure.traits().notesRangesOfOrigins || records.empty());
    uint32_t endOfPreviousRange = 0;
    for (const RangeOfOrigin& record : records) {
        uint32_t begin = describedOffsetOf(linkBuffer, record.begin);
        uint32_t end = describedOffsetOf(linkBuffer, record.end);
        RELEASE_ASSERT(endOfPreviousRange <= begin && begin <= end, endOfPreviousRange, begin, end);
        endOfPreviousRange = end;

        if (begin == end || !record.origin)
            continue;
        bool goesOn = !ranges.isEmpty() && ranges.last().end == begin && ranges.last().origin == record.origin && !isOriginOfBackEnd(record.origin);
        if (goesOn && (partAt == DescribedRangesPartAt::Origins || ranges.last().operation == record.operation)) {
            ranges.last().end = end;
            continue;
        }
        ranges.append(LinkedRangeOfOrigin { begin, end, record.origin, record.operation });
    }
}

void describeRangesOfOrigins(const Procedure& procedure, const LinkBuffer& linkBuffer, Vector<LinkedRangeOfOrigin>& ranges)
{
    describeRangesOfCode(procedure, linkBuffer, DescribedRangesPartAt::Origins, ranges);
}

void describeRangesOfOperations(const Procedure& procedure, const LinkBuffer& linkBuffer, Vector<LinkedRangeOfOrigin>& ranges)
{
    describeRangesOfCode(procedure, linkBuffer, DescribedRangesPartAt::Operations, ranges);
}

void describeRareCode(const Procedure& procedure, const LinkBuffer& linkBuffer, Vector<LinkedRangeOfRareCode>& ranges)
{
    auto add = [&](uint32_t begin, uint32_t end) {
        RELEASE_ASSERT(begin <= end && (ranges.isEmpty() || ranges.last().end <= begin));
        if (begin == end)
            return;
        if (!ranges.isEmpty() && ranges.last().end == begin)
            ranges.last().end = end;
        else
            ranges.append({ begin, end });
    };
    for (const RangeOfRareCode& range : procedure.rangesOfRareCodeAmongOtherCode())
        add(describedOffsetOf(linkBuffer, range.begin), describedOffsetOf(linkBuffer, range.end));
    add(describedOffsetOf(linkBuffer, procedure.rareCodeStart()), safeCast<uint32_t>(linkBuffer.size()));
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
