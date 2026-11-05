/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

bool buildLoopOverGenerator(Walker&, const BodyValue&);
bool engineAnswersForGenerators(Walker&);
std::optional<BuiltinID> findMemberAboveForIteration(const Walker&, BuiltinID start, BuiltinName);
GeneratorStepCheck checkLeftToStepOf(Walker&, Reference generator);
Reference buildStepOfGenerator(Walker&, Reference generator, ResumeMode, Reference sent, GeneratorStepCheck, Origin);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
