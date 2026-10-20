/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageStamp.h"

#if ENABLE(AOT)

#include "AOTImageFormat.h"
#include "AOTImageText.h"
#include "AOTInstance.h"
#include "AOTPositionTable.h"
#include "AOTProgramTables.h"
#include "AOTRegExpRecord.h"
#include "AOTRuntimeTable.h"
#include "AOTStubBlob.h"
#include "AOTStubFrames.h"
#include "AOTStubRegisters.h"
#include "AOTWire.h"
#include "FFIStubs.h"
#include <bit>

namespace JSC { namespace AOT {

uint64_t imageStamp(CPUFeatureSet features, const std::array<uint8_t, 16>& identity)
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
    mix(sizeof(ImageString));
    mix(sizeof(ImageLongString));
    mix(sizeof(ImageIdentifier));
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
