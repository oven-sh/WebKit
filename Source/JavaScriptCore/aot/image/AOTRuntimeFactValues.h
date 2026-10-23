/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTableArrays.h"
#include "AOTIdentifierTable.h"
#include "AOTImageFormat.h"
#include "AOTImageText.h"
#include "AOTInstance.h"
#include "AOTPositionTable.h"
#include "AOTProgramTables.h"
#include "AOTRegExpRecord.h"
#include "AOTRuntimeDescription.h"
#include "AOTRuntimeFactsOfLayoutsIncludes.h"
#include "AOTRuntimeTable.h"
#include "AOTStubBlob.h"
#include "AOTStubFrames.h"
#include "AOTStubRegisters.h"
#include "AOTWire.h"
#include "CPUFeatureSet.h"
#include "FFIStubs.h"
#include <iterator>

namespace JSC { namespace AOT {

static constexpr unsigned numberOfRuntimeFacts = 0
#define AOT_RUNTIME_FACT(name, value) + 1
#include "AOTRuntimeFacts.def"
#undef AOT_RUNTIME_FACT
    ;

struct DescribedRuntime {
    RuntimeDescription header;
    uint32_t facts[numberOfRuntimeFacts];
};
static constexpr uint32_t sizeOfDescribedRuntime = sizeof(RuntimeDescription) + numberOfRuntimeFacts * sizeof(uint32_t);
static_assert(offsetof(DescribedRuntime, facts) == sizeof(RuntimeDescription) && sizeOfDescribedRuntime <= sizeof(DescribedRuntime));

constexpr uint32_t checkedRuntimeFact(uint64_t value)
{
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(value <= UINT32_MAX);
    return static_cast<uint32_t>(value);
}

#if OS(DARWIN)
#define AOT_RUNTIME_DESCRIPTION_SECTION __attribute__((used, section("__TEXT,__bun_aot")))
#elif OS(WINDOWS)
#define AOT_RUNTIME_DESCRIPTION_SECTION __attribute__((used, section(".bunaot")))
#else
#define AOT_RUNTIME_DESCRIPTION_SECTION __attribute__((used, retain, section(".bun_aot")))
#endif

// Defined beside imageStamp(), which every run time has, so that no linker leaves it out.
extern "C" AOT_RUNTIME_DESCRIPTION_SECTION const DescribedRuntime describedRuntime;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
