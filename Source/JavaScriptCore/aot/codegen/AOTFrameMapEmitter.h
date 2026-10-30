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

class CCallHelpers;

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
    static bool savesDeadPreservedRegister(const B3::StackmapGenerationParams&);
    // In front of a call that clobbers calleeSavesClobberedAtSuspensions(): a callee stores the saved registers that it uses, and that word would go on naming a dead object.
    static void releaseSavedRegisters(CCallHelpers&, const B3::StackmapGenerationParams&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
