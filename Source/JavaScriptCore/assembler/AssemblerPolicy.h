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
        policy.signsReturnAddresses = false;
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
    // With ENABLE(RETURN_ADDRESS_SIGNING). An image is the same whether or not what compiles it has that, and runs with a run time
    // that has it and with one that has not. Whatever reads the return address of a frame takes a signature off that may be there.
    bool signsReturnAddresses { true };
    OopsForm oops { OopsForm::AbortWithReason };
};

} // namespace JSC
