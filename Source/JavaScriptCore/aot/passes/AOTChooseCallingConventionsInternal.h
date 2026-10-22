/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTChooseCallingConventions.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

struct ConventionInputs {
    ProgramEditor& editor;
    const OptimizationInputs& inputs;
    const PassRunner& runner;
    BuildContext& build;

    const Program& program() const { return editor.program(); }
    bool isEnabledFor(FunctionID) const;
};

enum class TailCalleeKind : uint8_t { Unknown, Direct };

struct TailCallSite {
    FunctionID caller;
    BlockID block;
    TailCalleeKind calleeKind { TailCalleeKind::Unknown };
    FunctionID callee;
    TailCallRequirement requirement { TailCallRequirement::Required };
    bool hasCheckedCast { false };
};

struct DirectCallUse {
    BitVector isCalledAndReturnedTo;
    BitVector isTailCalled;
};

Vector<TailCallSite> collectTailCallSites(const ConventionInputs&);
DirectCallUse collectDirectCallUse(const ConventionInputs&);

void chooseStubBodies(const ConventionInputs&, std::span<const TailCallSite>, const DirectCallUse&, ConventionRegisters);
void chooseResultRepresentations(const ConventionInputs&, std::span<const TailCallSite>, const DirectCallUse&, CallingConventionTables&);
void demoteMismatchedTailCalls(const ConventionInputs&, Vector<TailCallSite>&, const CallingConventionTables&, BitVector& changedFunctions);
void reserveStackWords(const ConventionInputs&, std::span<const TailCallSite>, CallingConventionTables&);
void chooseSecondaryEntries(const ConventionInputs&, CallingConventionTables&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
