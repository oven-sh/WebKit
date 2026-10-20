/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFrameMaps.h"
#include "AOTOperationEmitter.h"
#include <optional>

namespace JSC {

namespace B3 {
class StackmapGenerationParams;
class ValueRep;
namespace Air {
class Code;
}
}

namespace AOT { namespace Typed {

class FrameMapEmitter final : public OperationEmitter {
public:
    explicit FrameMapEmitter(CodeGenerator&);

    struct Roots {
        unsigned firstChild { 0 };
        unsigned count { 0 };
        Vector<FrameSlotKind, 8> kinds;
    };

    Roots appendRoots(B3::PatchpointValue*, const ValueContext& suspension);
    static std::optional<uint64_t> siteWordFor(FrameMapRecords&, const B3::StackmapGenerationParams&, const Roots&, unsigned promiseChild);

private:
    static std::optional<uint32_t> locationOf(const B3::ValueRep&, FrameSlotKind);
    static std::optional<uint32_t> savedRegistersWord(const B3::Air::Code&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
