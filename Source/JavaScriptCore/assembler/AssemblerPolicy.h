/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "CPUFeatureSet.h"

namespace JSC {

enum class OopsForm : uint8_t {
    AbortWithReason,
    Breakpoint,
};

struct AssemblerPolicy {
    static AssemblerPolicy forJIT()
    {
        AssemblerPolicy policy;
        policy.features = CPUFeatureSet::host();
        return policy;
    }

    static constexpr AssemblerPolicy forImage(CPUFeatureSet features)
    {
        AssemblerPolicy policy;
        policy.features = features;
        policy.blindsConstants = false;
        policy.recordsProcessAddresses = true;
        policy.recordsLinkedSites = true;
        policy.shortensJumps = true;
        policy.shortensEncodings = true;
        policy.oops = OopsForm::Breakpoint;
        return policy;
    }

    friend constexpr bool operator==(const AssemblerPolicy&, const AssemblerPolicy&) = default;

    constexpr bool needsLinkRecords() const { return recordsProcessAddresses || recordsLinkedSites || shortensJumps; }

    CPUFeatureSet features;
    bool blindsConstants { true };
    bool recordsProcessAddresses { false };
    bool recordsLinkedSites { false };
    bool shortensJumps { false };
    bool shortensEncodings { false };
    OopsForm oops { OopsForm::AbortWithReason };
};

} // namespace JSC
