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

namespace AOT { namespace Typed {

class FrameMapEmitter final : public OperationEmitter {
public:
    explicit FrameMapEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    struct Roots {
        unsigned firstChild { 0 };
        Vector<FrameSlotKind, 8> kinds;
    };

    // Code::liveAt(suspension), and the operands that the suspension holds, as LateColdAny operands: each value whose form holds cells, and each such result of a live value that has several.
    // Marks the Patch as a suspension and gives it the clobbers of one.
    Roots appendRoots(Machine::PatchOperation*, const ValueContext& suspension);
    static std::optional<uint64_t> siteWordFor(FrameMapRecords&, const Machine::PatchParameters&, const Roots&, unsigned promiseChild);
    static bool savesDeadPreservedRegister(const Machine::PatchParameters&);
    // In front of a call that clobbers calleeSavesClobberedAtSuspensions(): a callee stores the saved registers that it uses, and that word would go on naming a dead object.
    static void releaseSavedRegisters(CCallHelpers&, const Machine::PatchParameters&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
