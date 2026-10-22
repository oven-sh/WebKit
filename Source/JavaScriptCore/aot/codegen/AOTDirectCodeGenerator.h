/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "CCallHelpers.h"
#include <memory>

namespace JSC {

namespace B3 {
class Procedure;
}

namespace AOT { namespace Typed {

class BlockColdnessRule;
class DirectCodeGenerator;
struct CodeRecords;

struct DirectCodeGeneratorDeleter {
    void operator()(DirectCodeGenerator*) const;
};

using DirectCodeGeneratorPointer = std::unique_ptr<DirectCodeGenerator, DirectCodeGeneratorDeleter>;

enum class DirectCodeAttempt : uint8_t {
    First,
    WithSpareSlots,
    None,
};

DirectCodeGeneratorPointer planDirectCode(B3::Procedure&, DirectCodeAttempt);
DirectCodeAttempt attemptAfterFailure(const DirectCodeGenerator&);
bool generateDirectCode(DirectCodeGenerator&, CCallHelpers&, CodeRecords&, const BlockColdnessRule&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
