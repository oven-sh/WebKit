/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeDescription.h"

#if ENABLE(AOT)

#include "AOTCodeGenerator.h"
#include "AOTCodeRecords.h"
#include "AOTEntryEmitter.h"
#include "AOTStackCheckEmitter.h"
#include "AirCode.h"
#include "B3Procedure.h"
#include "LinkBuffer.h"
#include <algorithm>
#include <bit>
#include <wtf/CheckedArithmetic.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static Vector<CodeEntry, 1> describeEntries(CodeRecords& records, LinkBuffer& linkBuffer)
{
    Vector<RecordedEntry, 1>& recorded = records.entries;
    std::ranges::sort(recorded, { }, &RecordedEntry::id);
    RELEASE_ASSERT(!recorded.isEmpty() && recorded[0].id == primaryEntry);
    RELEASE_ASSERT(std::ranges::adjacent_find(recorded, { }, &RecordedEntry::id) == recorded.end());

    Vector<CodeEntry, 1> entries;
    entries.reserveInitialCapacity(recorded.size());
    for (const RecordedEntry& entry : recorded)
        entries.append({ linkBuffer.offsetOf(entry.label), entry.id });
    return entries;
}

Vector<ConstantData> copyConstants(const B3::Procedure& procedure)
{
    Vector<ConstantData> constants;
    constants.reserveInitialCapacity(procedure.dataSections().size());
    for (const B3::Procedure::DataSectionRecord& section : procedure.dataSections()) {
        RELEASE_ASSERT(section.size);
        constants.append({
            Vector<uint8_t>(unsafeMakeSpan(static_cast<const uint8_t*>(section.data), section.size)),
            alignmentOfConstant(section.size),
        });
    }
    return constants;
}

Vector<AOT::JumpTable> takeJumpTables(B3::Procedure& procedure)
{
    Vector<AOT::JumpTable> jumpTables;
    jumpTables.reserveInitialCapacity(procedure.numberOfJumpTables());
    for (unsigned index = 0; index < procedure.numberOfJumpTables(); ++index)
        jumpTables.append({ WTF::move(procedure.jumpTable(index).targetOffsets) });
    return jumpTables;
}

static CodeBody describeBody(CodeRecords& records, EmittedCode& emitted)
{
    LinkBuffer& linkBuffer = emitted.buffer.linkBuffer();
    CodeBody body;
    body.relocations = WTF::move(emitted.relocations);
    records.stubCalls.describeCalls(body.relocations.span(), body, &linkBuffer);
    body.units = records.units.link(emitted.buffer, body.relocations.span());
    body.sizeOfHotBlocks = records.units.sizeOfHotBlocks();
    body.localSites = linkBuffer.linkedSites();
    body.bytes = Vector<uint8_t>(emitted.buffer.bytes());

    uint32_t size = safeCast<uint32_t>(body.bytes.size());
    body.paddingStart = std::min(linkBuffer.offsetOf(emitted.paddingStart), size);
    body.entries = describeEntries(records, linkBuffer);
    if (emitted.procedure) {
        body.constants = copyConstants(*emitted.procedure);
        body.jumpTables = takeJumpTables(*emitted.procedure);
    }
    return body;
}

static Vector<CodeRange, 1> describeFrameRanges(const B3::Air::Code& code, LinkBuffer& linkBuffer)
{
    Vector<CodeRange, 1> ranges;
    for (const B3::Air::FrameRange& range : code.frameRanges()) {
        CodeRange linked { linkBuffer.offsetOf(range.begin), linkBuffer.offsetOf(range.end) };
        RELEASE_ASSERT(linked.begin <= linked.end && (ranges.isEmpty() || ranges.last().end <= linked.begin));
        if (linked.begin == linked.end)
            continue;
        if (!ranges.isEmpty() && ranges.last().end == linked.begin) {
            ranges.last().end = linked.end;
            continue;
        }
        ranges.append(linked);
    }
    return ranges;
}

// A landing pad takes every register that a call may clobber as clobbered, so unwinding restores only the callee-saved registers of the VM.
static RegisterAtOffsetList registersRestoredByUnwinding(const B3::Procedure& procedure, const Function& function)
{
    if (function.role != FunctionRole::StubBody)
        return procedure.calleeSaveRegisterAtOffsetList();
    RegisterSet restored = RegisterSet::vmCalleeSaveRegisters();
    for (const RegisterAtOffset& entry : procedure.calleeSaveRegisterAtOffsetList())
        RELEASE_ASSERT(!restored.contains(entry.reg(), IgnoreVectors));
    return { };
}

static FrameDescription describeFrame(const CodeRecords& records, EmittedCode& emitted, FunctionID id, const Function& function)
{
    FrameDescription frame;
    frame.spillsArgumentRegisters = records.frame.spillsArgumentRegisters;
    frame.stackUsedAtCalls = records.frame.stackUsedAtCalls;
    frame.isEnteredWithoutStackCheck = emitted.stackChecks.isEnteredWithoutStackCheck;
    frame.isProvenToFitOnStack = emitted.stackChecks.isProvenToFitOnStack;
    if (!emitted.procedure)
        return frame;

    LinkBuffer& linkBuffer = emitted.buffer.linkBuffer();
    const B3::Procedure& procedure = *emitted.procedure;
    const B3::Air::Code& code = procedure.code();
    frame.frameSizeInBytes = procedure.frameSize();
    frame.hasFrameRecord = !code.frameLayout().hasFrame || code.frameLayout().hasFrameRecord;
    frame.calleeSaveRegisters = registersRestoredByUnwinding(procedure, function);
    frame.frameRanges = describeFrameRanges(code, linkBuffer);
    // A frame that is set up for its stack check alone is torn down at once: no byte of code runs in it.
    RELEASE_ASSERT(code.frameLayout().hasFrame || !frame.hasFrame());
    frame.prologueChecks = linkStackChecks(linkBuffer, id, records);
    // A frame is allocated whole when such a check calls, and without a record only its size leads from there to the caller.
    if (!frame.hasFrameRecord) {
        for (const PrologueCheck& check : frame.prologueChecks) {
            RELEASE_ASSERT(frame.frameRanges.isEmpty() || frame.frameRanges.last().end <= check.callOffset);
            frame.frameRanges.append({ check.callOffset, check.callOffset + static_cast<uint32_t>(sizeOfNearCall) });
        }
    }
    return frame;
}

CompiledCode describeCode(BuildContext& build, const CodeGenerationInputs& inputs, FunctionID id, const Function& function, CodeRecords& records, EmittedCode&& emitted)
{
    CompiledCode result;
    result.function = id;
    result.entry = describeEntry(inputs, id, function);
    result.body = describeBody(records, emitted);
    result.sites = describeSites({ build, inputs.program, id, function }, records, emitted.buffer.linkBuffer(), result.body);
    result.frame = describeFrame(records, emitted, id, function);
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
