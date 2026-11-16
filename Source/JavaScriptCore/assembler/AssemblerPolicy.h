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

// What code does with the link register behind a prologue, and behind an epilogue (AssemblerPolicy::keepsLinkRegisterUnsigned).
enum class SavedLinkRegister : bool { MayBeRead, IsDead };
enum class LoadedLinkRegister : bool { MayBeSavedAgain, IsOnlyReturnedTo };

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
        policy.trapsWithIllegalInstruction = true;
        policy.keepsLinkRegisterUnsigned = true;
        return policy;
    }

    // For the generators that the JITs share, which sign the link register at the entry of what they generate.
    constexpr AssemblerPolicy withLinkRegisterSignedUntilReturn() const
    {
        AssemblerPolicy policy = *this;
        policy.keepsLinkRegisterUnsigned = false;
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
    bool trapsWithIllegalInstruction { false };
    // ENABLE(RETURN_ADDRESS_SIGNING) only. A return address is signed to be saved and authenticated as it is loaded, so code that never
    // saves the link register does neither, and a jump may leave for any code that a call may enter.
    bool keepsLinkRegisterUnsigned { false };
    // ARM64 only. A branch of image code that spans more than half of this goes through branch islands.
    uint32_t branchReach { 128u << 20 };
};

} // namespace JSC
