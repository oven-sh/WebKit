/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTVector.h"
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class ProgramEditor;
struct OptimizationInputs;

// Answers with the callees of all direct calls in the function, or with null where they are not known.
using DirectCalleesOfFunction = ScopedLambda<const CompactList<FunctionID>*(FunctionID)>;

// The reader is handed each function that a head was copied into, behind all copies.
void inlineHeads(ProgramEditor&, const OptimizationInputs&, BuildContext&, const ScopedLambda<void(FunctionID)>* readEditedFunction = nullptr, const DirectCalleesOfFunction* = nullptr);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
