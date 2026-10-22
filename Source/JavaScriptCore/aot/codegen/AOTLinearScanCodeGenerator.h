/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDirectCodeGenerator.h"
#include <memory>

namespace JSC { namespace AOT { namespace Typed {

class LinearScanCodeGenerator;

struct LinearScanCodeGeneratorDeleter {
    void operator()(LinearScanCodeGenerator*) const;
};

using LinearScanCodeGeneratorPointer = std::unique_ptr<LinearScanCodeGenerator, LinearScanCodeGeneratorDeleter>;

enum class LoopsInDirectCode : uint8_t {
    AreGenerated,
    AreLeftToB3,
};

LinearScanCodeGeneratorPointer planLinearScanCode(B3::Procedure&, DirectCodeAttempt, LoopsInDirectCode);
DirectCodeAttempt attemptAfterFailure(const LinearScanCodeGenerator&);
bool generateLinearScanCode(LinearScanCodeGenerator&, CCallHelpers&, CodeRecords&, const BlockColdnessRule&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
