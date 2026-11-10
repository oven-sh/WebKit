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
        const Machine::StackSlot* slotOfKeptWords { nullptr };
        uint32_t numberOfTracedWordsInSlot { 0 };
    };

    Roots appendRoots(Machine::PatchOperation*, const ValueContext& suspension);
    static std::optional<uint64_t> siteWordFor(FrameMapRecords&, const Machine::PatchParameters&, const Roots&, unsigned promiseChild);
    static std::optional<uint64_t> siteWordOfFrameOnHeap(const Machine::PatchParameters&);
    static void releaseSavedRegisters(CCallHelpers&, const Machine::PatchParameters&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
