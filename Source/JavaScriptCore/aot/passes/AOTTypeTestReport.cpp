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
#include <algorithm>
#include <tuple>
#include <wtf/SaturatingArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

static void appendTypeTestRecord(const Program& program, FunctionID id, const Instruction& test, Vector<Wire::TypeTestRecord>& records)
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

Vector<Wire::TypeTestRecord> collectTypeTestRecords(const Program& program)
{
    return collectTypeTestRecords(program, ParallelLoops(1));
}

Vector<Wire::TypeTestRecord> collectTypeTestRecords(const Program& program, const ParallelLoops& loops)
{
    OpcodeSet typeTestOpcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (opcode == Opcode::Check || std::ranges::contains(replacedOperationsOf(opcode), Opcode::Check) || opcode == Opcode::TypeTest || familyOf(opcode) == OperationFamily::Guard)
            typeTestOpcodes.add(opcode);
    }

    Vector<Wire::TypeTestRecord> records = collectInParallel<Wire::TypeTestRecord>(loops, program.functions.size(), [&](unsigned index, Vector<Wire::TypeTestRecord>& found) {
        FunctionID id(index);
        forEachListedValue(program, id, typeTestOpcodes, [&](BlockID, uint32_t, ValueID, const Value& value) {
            appendTypeTestRecord(program, id, value, found);
        });
        if (!mayEndBlockWith(program, id, { TerminatorKind::SwitchOnShape }))
            return;
        for (const Block& block : program.at(id).blocks) {
            if (block.terminator.kind == TerminatorKind::SwitchOnShape)
                appendTypeTestRecord(program, id, block.terminator, found);
        }
    });

    auto orderKey = [](const Wire::TypeTestRecord& record) {
        return std::tuple { record.location.module, record.location.position, record.function };
    };
    parallelSort(loops, records.mutableSpan(), [&](const Wire::TypeTestRecord& a, const Wire::TypeTestRecord& b) {
        return orderKey(a) < orderKey(b);
    });

    size_t recordCount = 0;
    for (Wire::TypeTestRecord record : records) {
        if (recordCount && records[recordCount - 1].location == record.location) {
            uint32_t& copies = records[recordCount - 1].copies;
            copies = saturatingSum<uint32_t>(copies, record.copies);
            continue;
        }
        records[recordCount++] = record;
    }
    records.shrink(recordCount);
    return records;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
