/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageTestSupport.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsExceptions.h"
#include "AOTVector.h"
#include <initializer_list>
#include <wtf/StdLibExtras.h>

namespace JSC::AOT::ImageTests {

inline constexpr unsigned numberOfMixedBodyKinds = 7;
inline constexpr unsigned numberOfMixedBodies = 86 * numberOfMixedBodyKinds;

inline CodeSketch framedSketch(uint32_t function)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    sketch.plain(0x1000 + function);
    return sketch;
}

inline CompiledCode plainBody(uint32_t function)
{
    CodeSketch sketch = framedSketch(function);
    sketch.callStub(writeBarrier(), sketch.addSite());
    sketch.end();
    return sketch.take();
}

inline CompiledCode entryOfBody(const TestBuild& build, uint32_t function)
{
    CodeSketch sketch(build.genericEntrypointOf(function));
    sketch.plain(0x2000 + function);
    sketch.jumpToFunction(function);
    return sketch.take();
}

inline CompiledCode takeWithHandler(CodeSketch& sketch, std::initializer_list<uint32_t> frameMapWords)
{
    uint32_t site = sketch.code().sites.positions.size() - 1;
    uint32_t handler = sketch.end();
    RareListsOfSites& rareLists = sketch.code().sites.rareLists.ensure();
    rareLists.handlers.append(SiteHandler { site, handler, HandlerType::Catch });
    rareLists.frameMapWords = frameMapWords;
    return sketch.take();
}

inline CompiledCode framedCallsBody(uint32_t function, uint32_t lastFunction)
{
    CodeSketch sketch(function);
    sketch.withFrame();
    sketch.callFunction(1, sketch.addSite());
    sketch.callFunction(lastFunction, sketch.addSite());
    return takeWithHandler(sketch, { 1, 2 });
}

inline CompiledCode mixedBody(uint32_t function, uint32_t lastFunction)
{
    CodeSketch sketch(function);
    switch (function % numberOfMixedBodyKinds) {
    case 0:
        return plainBody(function);
    case 1:
        sketch.plain(7);
        sketch.jumpToStub(catchException());
        break;
    case 2:
        sketch.jumpToFunction(function + 4 <= lastFunction ? function + 4 : 1);
        break;
    case 3:
        sketch.withFrame();
        sketch.plain(0x1000 + function);
        for (uint32_t word = 1; word <= 4; ++word)
            sketch.plain(word);
        sketch.callStub(writeBarrier(), sketch.addSite());
        sketch.end();
        break;
    case 4:
        return framedCallsBody(function, lastFunction);
    case 5:
        sketch.plain(function);
        sketch.jumpToFunction(function + 1 <= lastFunction ? function + 1 : 1);
        break;
    case 6:
        sketch.withFrame();
        sketch.plain(function);
        sketch.jumpToFunction(function + 1 <= lastFunction ? function + 1 : 1);
        break;
    }
    return sketch.take();
}

inline Vector<CompiledCode> mixedBodies(unsigned count)
{
    Vector<CompiledCode> result;
    for (uint32_t function = 1; function <= count; ++function)
        result.append(mixedBody(function, count));
    return result;
}

inline uint64_t sumOfStatistics(TestBuild& build, std::initializer_list<StatisticID> statistics)
{
    uint64_t sum = 0;
    for (StatisticID statistic : statistics)
        sum += build.statistic(statistic);
    return sum;
}

inline void checkPartsOfCodeAddUp(TestBuild& build, const TestImage& image)
{
    const ImageHeader& header = image.image->header();
    const ImageCodeRanges& ranges = header.code;
    AOT_TEST_CHECK(build.statistic(StatisticID::CodeBytesEmitted) == header.codeSize);

    uint64_t sizeOfBodies = 0;
    for (const CompiledCode& function : image.built.functions)
        sizeOfBodies += function.body.bytes.size();
    AOT_TEST_CHECK(build.statistic(StatisticID::FunctionBytesEmitted) == sizeOfBodies);
    AOT_TEST_CHECK(build.statistic(StatisticID::SharedCodeBytesEmitted) == static_cast<uint64_t>(image.built.sharedCode.veneersOffset) * image.built.layout.sharedCodeCopies.size());
    AOT_TEST_CHECK(build.statistic(StatisticID::VeneerBytesEmitted) == build.statistic(StatisticID::Veneers) * testTemplates().sizeOfVeneer());
    AOT_TEST_CHECK(build.statistic(StatisticID::StubBytes) + build.statistic(StatisticID::ArgumentThunkBytes) <= image.built.sharedCode.veneersOffset);
    AOT_TEST_CHECK(sumOfStatistics(build, {
        StatisticID::FunctionBytesEmitted,
        StatisticID::FarInstanceWordBytesEmitted,
        StatisticID::VeneerBytesEmitted,
        StatisticID::SharedCodeBytesEmitted,
        StatisticID::PaddingBytesBetweenFunctions,
    }) == ranges.functionsEnd);

    uint64_t roomOfRegExpCode = header.executableSize - ranges.regExpCodeOffset;
    uint64_t sizeOfRegExpCode = build.statistic(StatisticID::RegExpCodeBytesEmitted);
    AOT_TEST_CHECK(sizeOfRegExpCode <= roomOfRegExpCode && roomOfRegExpCode - sizeOfRegExpCode < RegExpSection::alignment);
    AOT_TEST_CHECK(build.statistic(StatisticID::RegExp16BitCodeBytes) <= sizeOfRegExpCode);
    if (ranges.constantsOffset < header.executableSize)
        AOT_TEST_CHECK(!roomOfRegExpCode && !ranges.ffiCodeSize && ranges.constantsOffset + WTF::roundUpToMultipleOf<imageConstantsAlignment>(ranges.constantsSize) == header.executableSize && header.codeSize == header.executableSize);
    else
        AOT_TEST_CHECK(WTF::roundUpToMultipleOf<imageConstantsAlignment>(header.executableSize + build.statistic(StatisticID::RegExpTableBytesEmitted)) == ranges.constantsOffset);
    AOT_TEST_CHECK(build.statistic(StatisticID::WasmCodeBytesEmitted) == ranges.webAssemblyCodeSize);
    AOT_TEST_CHECK(build.statistic(StatisticID::FFICodeBytesEmitted) == ranges.ffiCodeSize);
    AOT_TEST_CHECK(build.statistic(StatisticID::ConstantBytesEmitted) == ranges.constantsSize);
    AOT_TEST_CHECK(ranges.functionsEnd + sumOfStatistics(build, {
        StatisticID::WasmCodeBytesEmitted,
        StatisticID::FFICodeBytesEmitted,
        StatisticID::RegExpCodeBytesEmitted,
        StatisticID::RegExpTableBytesEmitted,
        StatisticID::ConstantBytesEmitted,
        StatisticID::FFIDirectoryBytesEmitted,
        StatisticID::PaddingBytesBetweenCodeParts,
    }) == header.codeSize);

    AOT_TEST_CHECK(sumOfStatistics(build, {
        StatisticID::ConstantBytesOfSharedConstants,
        StatisticID::ConstantBytesOfShapeSets,
        StatisticID::ConstantBytesOfShapeBitmaps,
        StatisticID::ConstantBytesOfOffsetTables,
        StatisticID::ConstantBytesOfCellTemplates,
        StatisticID::ConstantBytesOfNumbers,
        StatisticID::UnlabeledConstantBytes,
        StatisticID::ConstantBytesOfJumpTables,
        StatisticID::PaddingBytesBetweenConstants,
    }) == ranges.constantsSize);
}

inline TestImage builtImage(TestBuild& build, Vector<CompiledCode>&& code)
{
    uint64_t sizeOfBodiesAsCompiled = 0;
    for (const CompiledCode& function : code)
        sizeOfBodiesAsCompiled += function.body.bytes.size();
    TestImage image = build.build(WTF::move(code));
    AOT_TEST_CHECK(image.image);
    checkPartsOfCodeAddUp(build, image);
    AOT_TEST_CHECK(build.statistic(StatisticID::FunctionBytesEmitted) + build.statistic(StatisticID::FinalJumpBytesRemoved) == sizeOfBodiesAsCompiled);
    return image;
}

} // namespace JSC::AOT::ImageTests

#endif // ENABLE(AOT)
