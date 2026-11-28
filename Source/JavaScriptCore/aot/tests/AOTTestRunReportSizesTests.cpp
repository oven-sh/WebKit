/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTImageTestBodies.h"
#include "AOTProgramTables.h"
#include "AOTTableParts.h"
#include "AOTTestRunReportSizes.h"
#include <utility>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT::ImageTests {

std::span<const NativeTest> sizesOfImageTests();

static uint64_t sumOfCellsForSizesTests(const SizesOfImage::Cells& cells)
{
    uint64_t sum = 0;
    for (auto [name, number] : cells)
        sum += number;
    return sum;
}

static uint64_t cellForSizesTests(const SizesOfImage::Cells& cells, ASCIILiteral wanted)
{
    unsigned found = 0;
    uint64_t result = 0;
    for (auto [name, number] : cells) {
        if (name != wanted)
            continue;
        ++found;
        result = number;
    }
    AOT_TEST_CHECK(found == 1);
    return result;
}

static void testCellsOfEachLineAddUp()
{
    TestBuild build(4);
    TestImage image = builtImage(build, bodies(plainBody(1), plainBody(2), plainBody(3)));
    const ImageHeader& header = image.image->header();
    const auto& bytesOfParts = image.built.bytesOfTableParts;
    SizesOfImage sizes = measureImageForTestRunReport(std::as_const(image.bytes).span(), bytesOfParts, build.context());

    AOT_TEST_CHECK(cellForSizesTests(sizes.image, "functions emitted"_s) == build.statistic(StatisticID::ImageFunctionsEmitted));
    AOT_TEST_CHECK(cellForSizesTests(sizes.image, "code bytes"_s) == build.statistic(StatisticID::CodeBytesEmitted));
    AOT_TEST_CHECK(cellForSizesTests(sizes.image, "text bytes"_s) == build.statistic(StatisticID::StringBytesEmitted));
    AOT_TEST_CHECK(cellForSizesTests(sizes.image, "identifiers"_s) == image.image->text().numberOfIdentifiers());

    uint64_t tableBytes = cellForSizesTests(sizes.image, "table bytes"_s);
    AOT_TEST_CHECK(tableBytes == build.statistic(StatisticID::TableBytesEmitted) && tableBytes <= header.codeOffset);
    AOT_TEST_CHECK(sumOfCellsForSizesTests(sizes.tables) == tableBytes);
    AOT_TEST_CHECK(cellForSizesTests(sizes.tables, "other"_s) >= sizeof(ImageHeader));

    uint64_t programTableBytes = cellForSizesTests(sizes.image, "program table bytes"_s);
    AOT_TEST_CHECK(programTableBytes == header.sections[static_cast<unsigned>(ImageSection::ProgramTables)].size);
    AOT_TEST_CHECK(cellForSizesTests(sizes.tables, "program tables"_s) == programTableBytes);
    AOT_TEST_CHECK(sumOfCellsForSizesTests(sizes.programTables) == programTableBytes);
    uint64_t bytesOfFunctions = cellForSizesTests(sizes.programTables, "functions"_s);
    AOT_TEST_CHECK(bytesOfFunctions && bytesOfFunctions == bytesOfParts[static_cast<unsigned>(TablePartID::Functions)]);
    uint64_t otherBytes = cellForSizesTests(sizes.programTables, "other"_s);
    uint64_t mostOtherBytes = bytesOfParts[static_cast<unsigned>(TablePartID::Header)] + bytesOfParts[static_cast<unsigned>(TablePartID::ShapeSets)] + 8 * numberOfTableParts;
    AOT_TEST_CHECK(otherBytes >= sizeof(ProgramTables) && otherBytes <= mostOtherBytes);
}

std::span<const NativeTest> sizesOfImageTests()
{
    static constexpr NativeTest tests[] = {
        { "image/sizes/cells-of-each-line-add-up"_s, testCellsOfEachLineAddUp },
    };
    return tests;
}

} // namespace JSC::AOT::ImageTests

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
