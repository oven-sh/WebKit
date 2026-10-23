/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageStamp.h"

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTIdentifierTable.h"
#include "AOTImageFormat.h"
#include "AOTImageText.h"
#include "AOTInstance.h"
#include "AOTPositionTable.h"
#include "AOTProgramTables.h"
#include "AOTRegExpRecord.h"
#include "AOTRuntimeFactValues.h"
#include "AOTRuntimeTable.h"
#include "AOTStubBlob.h"
#include "AOTStubFrames.h"
#include "AOTStubRegisters.h"
#include "AOTWire.h"
#include "FFIStubs.h"
#include <bit>

namespace JSC { namespace AOT {

static constexpr RuntimeDescription::OperatingSystem describedOperatingSystem()
{
#if OS(LINUX)
    return RuntimeDescription::OperatingSystem::Linux;
#elif OS(DARWIN)
    return RuntimeDescription::OperatingSystem::Darwin;
#elif OS(WINDOWS)
    return RuntimeDescription::OperatingSystem::Windows;
#elif OS(FREEBSD)
    return RuntimeDescription::OperatingSystem::FreeBSD;
#else
    return RuntimeDescription::OperatingSystem::Other;
#endif
}

extern "C" AOT_KEPT_BY_EVERY_LINKER constinit const DescribedRuntime describedRuntime = {
    {
        RuntimeDescription::expectedMagic,
        sizeOfDescribedRuntime,
        numberOfRuntimeFacts,
        CPUFeatureSet::buildTarget().bits(),
        static_cast<uint8_t>(hostArchitecture()),
        describedOperatingSystem(),
#if ENABLE(AOT_POSITION_INDEPENDENT_IMAGE)
        RuntimeDescription::ImageForm::PositionIndependent,
#else
        RuntimeDescription::ImageForm::Absolute,
#endif
        (ASSERT_ENABLED ? RuntimeDescription::Assertions : 0) | (ASAN_ENABLED ? RuntimeDescription::AddressSanitizer : 0),
        0,
    },
    {
#define AOT_RUNTIME_FACT(name, value) checkedRuntimeFact(value),
#include "AOTRuntimeFacts.def"
#undef AOT_RUNTIME_FACT
    },
};

uint64_t imageStamp(CPUFeatureSet features, const std::array<uint8_t, 16>& identity)
{
    return imageStamp(features, identity, imageFormOfThisRuntime());
}

uint64_t imageStamp(CPUFeatureSet features, const std::array<uint8_t, 16>& identity, Wire::ImageForm form)
{
    uint64_t stamp = Wire::schemaHash;
    auto mix = [&](uint64_t value) {
        stamp = (stamp ^ value) * 0x100000001b3ULL;
        stamp ^= stamp >> 29;
    };
    mix(runtimeLayoutHash());
    auto identityWords = std::bit_cast<std::array<uint64_t, 2>>(identity);
    mix(identityWords[0]);
    mix(identityWords[1]);
#if CPU(ARM64)
    mix(1 | static_cast<uint64_t>(features.bits()) << 8);
#elif CPU(X86_64)
    mix(2 | static_cast<uint64_t>(features.bits()) << 8);
#endif
    mix(numberOfEntries);
    mix(numberOfImageSections);
    mix(sizeof(ImageHeader));
    mix(sizeof(ImageFunction));
    mix(sizeof(ImageFrame));
    mix(sizeof(ImageInnerEntry));
    mix(sizeof(StubAddressInImage));
    mix(sizeof(ImageStringWord));
    mix(sizeof(ImageLongString));
    mix(ImageListedString::maxNumberOfWords);
    mix(sizeof(ImageIdentifierWord));
    mix(sizeof(IdentifierTable::Header) + IdentifierTable::firstHashBitInEntry);
    if (form == Wire::ImageForm::PositionIndependent)
        mix(sizeof(ImageStringRecord));
    mix(sizeof(ImageStringHeader));
    mix(sizeof(ImageRegExp));
    mix(sizeof(ImageRegExpHash));
    mix(sizeof(StubReturnSite));
    mix(sizeof(ProgramTables));
#if USE(BUN_JSC_ADDITIONS)
    mix(sizeof(FFI::StubDirectory));
    mix(sizeof(FFI::InvokeArguments));
    mix(sizeof(FFI::CallbackRegisters));
#endif
    mix(imagePageSize);
    mix(imageFunctionAlignment);
    mix(imageStubsAlignment);
    mix(imageSectionAlignment);
    mix(imageConstantsAlignment);
    mix(sitesBetweenPositionCheckpoints);
    mix(InstanceLayout::offsetOfStringCells);
    mix(InstanceLayout::sizeOfSiteCache);
    mix(static_cast<uint64_t>(instanceGPR));
    mix(static_cast<uint64_t>(cellCageBaseGPR));
    mix(static_cast<uint64_t>(-AdapterFrame::offsetOfInstance));
    mix(AdapterFrame::saveAreaSize);
    mix(numberOfStubFrameKinds);
    return stamp;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
