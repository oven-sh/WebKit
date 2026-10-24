/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestReport.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTDiagnostics.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <tuple>
#include <wtf/SaturatingArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

static bool isTypeTest(Opcode opcode)
{
    if (opcode == Opcode::Check || std::ranges::contains(replacedOperationsOf(opcode), Opcode::Check))
        return true;
    return opcode == Opcode::TypeTest || familyOf(opcode) == OperationFamily::Guard;
}

static OpcodeSet computeTypeTestOpcodes()
{
    OpcodeSet typeTestOpcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (isTypeTest(opcode))
            typeTestOpcodes.add(opcode);
    }
    return typeTestOpcodes;
}

static void appendRecord(const Program& program, FunctionID id, const Instruction& test, Vector<Wire::TypeTestRecord>& records)
{
    const Function& function = program.at(id);
    Origin origin = test.origin();
    Wire::SourceLocation location = function.locationOf(origin);
    if (!hasModule(location))
        return;

    FunctionID source = origin.inlineFrame ? function.inlineFrames[origin.inlineFrame].callee : id;
    records.append(Wire::TypeTestRecord {
        .location = location,
        .function = findFunctionAsRead(&program, source).index(),
        .failure = Wire::TypeTestFailure::TakesGenericPath,
        .copies = 1,
    });
}

static void appendRecordsOf(const Program& program, FunctionID id, const OpcodeSet& typeTestOpcodes, Vector<Wire::TypeTestRecord>& records)
{
    forEachListedValue(program, id, typeTestOpcodes, [&](BlockID, uint32_t, ValueID, const Value& value) {
        appendRecord(program, id, value, records);
    });
    if (!mayEndBlockWith(program, id, { TerminatorKind::SwitchOnShape }))
        return;
    for (const Block& block : program.at(id).blocks) {
        if (block.terminator.kind == TerminatorKind::SwitchOnShape)
            appendRecord(program, id, block.terminator, records);
    }
}

static auto orderKey(const Wire::TypeTestRecord& record)
{
    return std::tuple { record.location.module, record.location.position, record.function };
}

static void foldCopies(Vector<Wire::TypeTestRecord>& sortedRecords)
{
    size_t recordCount = 0;
    for (Wire::TypeTestRecord record : sortedRecords) {
        if (recordCount && sortedRecords[recordCount - 1].location == record.location) {
            uint32_t& copies = sortedRecords[recordCount - 1].copies;
            copies = saturatingSum<uint32_t>(copies, record.copies);
            continue;
        }
        sortedRecords[recordCount++] = record;
    }
    sortedRecords.shrink(recordCount);
}

Vector<Wire::TypeTestRecord> collectTypeTestRecords(const Program& program)
{
    ParallelLoops serialLoops(1);
    return collectTypeTestRecords(program, serialLoops);
}

Vector<Wire::TypeTestRecord> collectTypeTestRecords(const Program& program, const ParallelLoops& loops)
{
    OpcodeSet typeTestOpcodes = computeTypeTestOpcodes();
    Vector<Wire::TypeTestRecord> records = collectInParallel<Wire::TypeTestRecord>(loops, program.functions.size(), [&](unsigned index, Vector<Wire::TypeTestRecord>& found) {
        appendRecordsOf(program, FunctionID(index), typeTestOpcodes, found);
    });

    parallelSort(loops, records.mutableSpan(), [](const Wire::TypeTestRecord& a, const Wire::TypeTestRecord& b) {
        return orderKey(a) < orderKey(b);
    });
    foldCopies(records);
    return records;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
