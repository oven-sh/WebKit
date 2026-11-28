/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReaderInternal.h"

#if ENABLE(AOT)

#include "AOTStatistics.h"
#include <algorithm>
#include <atomic>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(std::ranges::none_of(allMembersOf<TerminatorKind>, [](TerminatorKind kind) { return wireFlagsOf(kind); }), "A terminator has no flags word on the wire.");

void Reader::readFunctions()
{
    constexpr size_t functionsPerChunk = 32;
    const ParallelLoops& loops = m_build.parallelLoops();
    size_t numberOfFunctions = m_sections.functions.size();
    RELEASE_ASSERT(!m_externalBodies || m_externalBodies->bodyCount == numberOfFunctions);

    growFromEmpty(loops, m_program.functions, numberOfFunctions);
    readInChunks(loops, numberOfFunctions, functionsPerChunk, [&](uint32_t index) {
        readFunction(index, m_program.functions[index]);
    });

    std::atomic<uint64_t> numberOfValues { 0 };
    forEachRangeOfIndicesInParallel(loops, numberOfFunctions, [&](size_t first, size_t end) {
        uint64_t numberInRange = 0;
        for (const Wire::Function& wire : m_sections.functions.subspan(first, end - first))
            numberInRange += wire.body.valueCount;
        numberOfValues.fetch_add(numberInRange, std::memory_order_relaxed);
    });
    m_build.statistics().count(StatisticID::FunctionsRead, numberOfFunctions);
    m_build.statistics().count(StatisticID::ValuesRead, numberOfValues.load(std::memory_order_relaxed));
}

static void readFunctionRecordOfUnit(RecordChecker& checker, const Wire::Function& wire, const UnitIDRemap& unit, FunctionRecord& record)
{
    Wire::Function withoutFunctionTypes = wire;
    withoutFunctionTypes.functionTypes = { };
    checker.setUnit(&unit);
    read(checker, withoutFunctionTypes, record);
    checker.setUnit(nullptr);

    std::span<const uint32_t> functionTypes = checker.words(wire.functionTypes);
    record.functionTypes.reserveInitialCapacity(functionTypes.size());
    for (uint32_t functionType : functionTypes)
        record.functionTypes.append(checker.id<FunctionID>(functionType));
}

void Reader::readFunction(uint32_t index, Function& function) const
{
    const Wire::Function& wire = m_sections.functions[index];
    const UnitIDRemap& unit = m_program.unitsOfBodies.unitOf(m_externalBodies, FunctionID(index));
    RecordChecker checker(*this);

    bool recordHoldsNumbersOfUnit = m_externalBodies && m_externalBodies->unitCount;
    if (recordHoldsNumbersOfUnit)
        readFunctionRecordOfUnit(checker, wire, unit, function);
    else
        read(checker, wire, static_cast<FunctionRecord&>(function));

    std::span<const uint32_t> words;
    if (m_externalBodies) {
        ASSERT(!wire.body.code.count);
        const ExternalFunctionBody& body = unsafeMakeSpan(m_externalBodies->bodies, m_externalBodies->bodyCount)[index];
        words = unsafeMakeSpan(body.code, body.wordCount);
    } else
        words = checker.code(wire.body.code);

    function.body = Body(words, wire.body.blockCount, wire.body.valueCount, unit);
    function.capabilities = initialCapabilitiesOf(function);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
