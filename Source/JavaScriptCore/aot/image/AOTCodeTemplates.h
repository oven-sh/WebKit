/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRelocation.h"
#include <expected>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

struct CodeTemplates {
    JS_EXPORT_PRIVATE static std::expected<CodeTemplates, LinkFailure> create(const AssemblerPolicy&, BuildContext*);

    size_t sizeOfVeneer() const { return veneer.bytes.size(); }

    Vector<uint8_t> nearCall;
    Vector<uint8_t> nearJump;
    Vector<uint8_t> returnToCaller;
    Vector<uint8_t> breakpoint;
    RelocatableCode veneer;
};

JS_EXPORT_PRIVATE void fillWithNops(std::span<uint8_t>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
