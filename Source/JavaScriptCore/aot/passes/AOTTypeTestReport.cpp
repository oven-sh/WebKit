/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestReport.h"

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <optional>
#include <tuple>
#include <wtf/SaturatingArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

using FailureByOpcode = std::array<std::optional<Wire::TypeTestFailure>, numberOfEnumerators<Opcode>>;

static std::optional<Wire::TypeTestFailure> failureOf(Opcode opcode)
{
    if (opcode == Opcode::Claim || std::ranges::contains(replacedOperationsOf(opcode), Opcode::Claim))
        return Wire::TypeTestFailure::Throws;
    if (opcode == Opcode::TypeTest || familyOf(opcode) == OperationFamily::Guard)
        return Wire::TypeTestFailure::TakesGenericPath;
    return std::nullopt;
}

static FailureByOpcode computeFailureByOpcode()
{
    FailureByOpcode failureByOpcode;
    for (Opcode opcode : allMembersOf<Opcode>)
        failureByOpcode[static_cast<size_t>(opcode)] = failureOf(opcode);
    return failureByOpcode;
}

static void appendRecord(const Program& program, FunctionID id, const Instruction& test, Wire::TypeTestFailure failure, Vector<Wire::TypeTestRecord>& records)
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
        .failure = failure,
        .copies = 1,
    });
}

static void appendRecordsOf(const Program& program, FunctionID id, const FailureByOpcode& failureByOpcode, Vector<Wire::TypeTestRecord>& records)
{
    const Function& function = program.at(id);
    for (const Block& block : function.blocks) {
        for (ValueID valueID : block.values) {
            const Value& value = function.at(valueID);
            if (std::optional<Wire::TypeTestFailure> failure = failureByOpcode[static_cast<size_t>(value.opcode)])
                appendRecord(program, id, value, *failure, records);
        }
        if (block.terminator.kind == TerminatorKind::SwitchOnShape)
            appendRecord(program, id, block.terminator, Wire::TypeTestFailure::TakesGenericPath, records);
    }
}

static auto orderKey(const Wire::TypeTestRecord& record)
{
    return std::tuple { record.location.module, record.location.position, record.failure, record.function };
}

static bool isSameSourceTest(const Wire::TypeTestRecord& a, const Wire::TypeTestRecord& b)
{
    return a.location == b.location && a.failure == b.failure;
}

static void foldCopies(Vector<Wire::TypeTestRecord>& sortedRecords)
{
    size_t recordCount = 0;
    for (Wire::TypeTestRecord record : sortedRecords) {
        if (recordCount && isSameSourceTest(sortedRecords[recordCount - 1], record)) {
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
    FailureByOpcode failureByOpcode = computeFailureByOpcode();
    Vector<Wire::TypeTestRecord> records = collectInParallel<Wire::TypeTestRecord>(loops, program.functions.size(), [&](unsigned index, Vector<Wire::TypeTestRecord>& found) {
        appendRecordsOf(program, FunctionID(index), failureByOpcode, found);
    });

    parallelSort(loops, records.mutableSpan(), [](const Wire::TypeTestRecord& a, const Wire::TypeTestRecord& b) {
        return orderKey(a) < orderKey(b);
    });
    foldCopies(records);
    return records;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
