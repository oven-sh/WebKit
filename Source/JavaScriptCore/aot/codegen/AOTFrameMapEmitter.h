/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFrameMaps.h"
#include "AOTOperationEmitter.h"
#include "AOTVector.h"
#include <optional>

namespace JSC {

namespace B3 {
class StackmapGenerationParams;
}

namespace AOT { namespace Typed {

class FrameMapEmitter final : public OperationEmitter {
public:
    explicit FrameMapEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    struct Roots {
        unsigned firstChild { 0 };
        Vector<FrameSlotKind, 8> kinds;
    };

    Roots appendRoots(B3::PatchpointValue*, const ValueContext& suspension);
    static std::optional<uint64_t> siteWordFor(FrameMapRecords&, const B3::StackmapGenerationParams&, const Roots&, unsigned promiseChild);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
