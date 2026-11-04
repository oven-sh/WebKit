/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

// An IteratorOpen, IteratorResume or IteratorClose of a `for (.. of ..)` over what is proven or expected to be a generator. False: nothing was built.
bool buildLoopOverGenerator(Walker&, const BodyValue&);
// From %GeneratorPrototype% upward `next` and `return` are the engine's for good, and @@iterator returns its receiver.
bool engineAnswersForGenerators(Walker&);
// What a step of `generator` has left to ask, by what is proven of it. Link's word that an object is as it was made is not taken for a generator.
GeneratorStepCheck checkLeftToStepOf(Walker&, Reference generator);
// `sent` may be null: undefined is sent. Null: no way leads on.
Reference buildStepOfGenerator(Walker&, Reference generator, ResumeMode, Reference sent, GeneratorStepCheck, Origin);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
