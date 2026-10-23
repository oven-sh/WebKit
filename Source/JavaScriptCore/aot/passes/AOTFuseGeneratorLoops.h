/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEscapeAnalysis.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

struct GeneratorBodyDescription {
    FunctionID body;
    Vector<ValueID, 8> entryArguments;
    std::optional<unsigned> generatorParameter;
    std::optional<unsigned> sentParameter;
    std::optional<unsigned> modeParameter;
    ValueID startAtCreation;
};

std::optional<GeneratorBodyDescription> describeGeneratorBody(const Program&, const Function& caller, ValueID newGenerator);
ValueID appendAwait(IRBuilder&, ValueID awaited);

struct GeneratorStepSite {
    ValueID step;
    ResumeMode mode { ResumeMode::Next };
    ValueID sent;
};

struct FusionCandidate {
    ValueID generator;
    ObjectID object;
    GeneratorFusionDeclineReason obstacle { GeneratorFusionDeclineReason::Unknown };
    std::optional<Use> witness;
    GeneratorBodyDescription body;
    // The first site is body.startAtCreation: a CallDirect with one result, the value, and no sent value. Every other site is a StepGenerator.
    Vector<GeneratorStepSite, 4> sites;
};

Vector<FusionCandidate> findFusionCandidates(FunctionPassContext&);
GeneratorFusionDeclineReason findMergeObstacle(FunctionPassContext&, const FusionCandidate&);
void fuseGenerator(FunctionPassContext&, IRBuilder&, const FusionCandidate&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
