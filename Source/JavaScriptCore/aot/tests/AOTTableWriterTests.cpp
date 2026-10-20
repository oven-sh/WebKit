/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageTestSupport.h"
#include "AOTTableWriter.h"
#include <array>

namespace JSC::AOT {

using namespace ImageTests;
using Typed::TableWriter;

static constexpr size_t vectorBytes = defaultLimitValue(LimitID::VectorBytes, hostArchitecture());
static_assert(vectorBytes < defaultLimitValue(LimitID::TableBytes, hostArchitecture()), "A writer tests VectorBytes first, so no writer reports TableBytes.");

static uint64_t hitCount(BuildContext& build, LimitID limit)
{
    return build.statistics().values()[static_cast<unsigned>(hitsOf(limit))];
}

static void testSharesAppendedBytes()
{
    BuildContext build(wireOptions());
    TableWriter writer(build);
    uint32_t first = writer.append(Vector<uint32_t> { 1, 2, 3 });
    uint32_t second = writer.append(Vector<uint32_t> { 1, 2, 4 });
    size_t size = writer.size();
    AOT_TEST_CHECK(second != first);
    AOT_TEST_CHECK(writer.append(Vector<uint32_t> { 1, 2, 3 }) == first);
    AOT_TEST_CHECK(writer.size() == size);
}

static void testSharesOnlyAppendedBytes()
{
    BuildContext build(wireOptions());
    TableWriter writer(build);
    constexpr std::array<uint8_t, 8> zeros { };
    uint32_t allocated = writer.allocate(zeros.size());
    uint32_t unshared = writer.appendUnshared(zeros);
    uint32_t appended = writer.append(std::span<const uint8_t> { zeros });
    AOT_TEST_CHECK(unshared == allocated + zeros.size());
    AOT_TEST_CHECK(appended == unshared + zeros.size());
}

static void testFillsToLimit()
{
    BuildContext build(wireOptions());
    TableWriter writer(build);
    AOT_TEST_CHECK(!writer.allocate(8));
    AOT_TEST_CHECK(writer.allocate(vectorBytes - 8, 1) == 8);
    AOT_TEST_CHECK(writer.take().size() == vectorBytes);
    AOT_TEST_CHECK(!hitCount(build, LimitID::VectorBytes));
    AOT_TEST_CHECK(!count(build, UserDiagnosticKind::LimitExceeded, LimitID::VectorBytes));
}

static void testVectorBytesOverflow()
{
    BuildContext build(wireOptions());
    TableWriter writer(build);
    constexpr std::array<uint8_t, 4> bytes { 1, 2, 3, 4 };
    constexpr size_t sizeOfFirstRecord = 8;
    constexpr size_t sizeOfLargeRecord = vectorBytes + 1 - sizeOfFirstRecord;
    static_assert(!(sizeOfLargeRecord % alignof(uint64_t)));
    AOT_TEST_CHECK(!writer.allocate(sizeOfFirstRecord));

    AOT_TEST_CHECK(!writer.allocate(sizeOfLargeRecord));
    AOT_TEST_CHECK(writer.size() == sizeOfLargeRecord);
    writer.at<uint64_t>(sizeOfLargeRecord - sizeof(uint64_t)) = 1;

    AOT_TEST_CHECK(!writer.allocate(8));
    AOT_TEST_CHECK(!writer.append(std::span<const uint8_t> { bytes }));
    AOT_TEST_CHECK(!writer.appendUnshared(bytes));
    writer.appendByte(5);
    AOT_TEST_CHECK(writer.size() == sizeOfLargeRecord);
    AOT_TEST_CHECK(writer.take().isEmpty());
    AOT_TEST_CHECK(hitCount(build, LimitID::VectorBytes) == 1);
    AOT_TEST_CHECK(count(build, UserDiagnosticKind::LimitExceeded, LimitID::VectorBytes) == 1);
    AOT_TEST_CHECK(build.diagnostics().userDiagnostics().size() == 1);
    AOT_TEST_CHECK(build.diagnostics().internalErrors().isEmpty());
}

std::span<const NativeTest> tableWriterTests()
{
    static constexpr NativeTest tests[] = {
        { "place/table-writer/shares-appended-bytes"_s, testSharesAppendedBytes },
        { "place/table-writer/shares-only-appended-bytes"_s, testSharesOnlyAppendedBytes },
        { "place/table-writer/fills-to-limit"_s, testFillsToLimit },
        { "place/table-writer/vector-bytes-overflow"_s, testVectorBytesOverflow },
    };
    return tests;
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
