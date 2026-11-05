/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

struct LoopOverIterator {
    ValueID step;
    BlockID blockOfStep;
    bool stepStandsInHeadOfLoop { false };
};
std::optional<LoopOverIterator> findLoopOverIterator(const BodyIndex&, ValueID open);
void buildFirstReadsThatOpenOfGeneratorLeavesOut(Walker&, Origin);
void buildFirstReadThatCloseOfGeneratorLeavesOut(Walker&, Origin);
bool buildLoopOverGenerator(Walker&, const BodyValue&);
bool engineAnswersForGenerators(Walker&);
GeneratorStepCheck checkLeftToStepOf(Walker&, Reference generator);
Reference buildStepOfGenerator(Walker&, Reference generator, ResumeMode, Reference sent, GeneratorStepCheck, Origin);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
