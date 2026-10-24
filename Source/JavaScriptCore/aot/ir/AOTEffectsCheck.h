/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTVector.h"

namespace JSC { namespace AOT {

class Diagnostics;

namespace Typed {

class FunctionAnalyses;

class ReplacedEffects {
public:
    void add(const EffectsOnEdges&, Opcode);
    void add(const EffectContext&, const Function&, const Value&);
    void add(const EffectContext&, const Function&, const Terminator&);

    const EffectsOnEdges& effects() const LIFETIME_BOUND { return m_effects; }
    const Vector<Opcode, 4>& operations() const LIFETIME_BOUND { return m_operations; }
    const Vector<TerminatorKind, 1>& terminators() const LIFETIME_BOUND { return m_terminators; }

private:
    EffectsOnEdges m_effects { EffectsOnEdges::none() };
    Vector<Opcode, 4> m_operations;
    Vector<TerminatorKind, 1> m_terminators;
};

struct ReplacementSite {
    FunctionID function;
    BlockID block;
    ValueID value;
};

void checkReplacement(const EffectContext&, const Function&, const ReplacementSite&, const Value& replacement, const ReplacedEffects&, Diagnostics&);
void checkReplacement(const EffectContext&, const Function&, const ReplacementSite&, const Terminator& replacement, const ReplacedEffects&, Diagnostics&);

void checkDeclaredEffects(Diagnostics&);
// The analyses are those that the summaries were solved over: of the body against createSummariesFromFlow().
void checkSummaryCoversBody(const EffectContext&, FunctionID, FunctionAnalyses&, Diagnostics&);
// A call by value claims the summary of its callee and what the entrypoint does around the call of the callee: it reads the captures and the arguments and collects the rest arguments.
void checkCallByValueCoversEntrypoint(const Program&, FunctionID target, const Function& entrypoint, Diagnostics&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
