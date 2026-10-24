/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTKnownValue.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;

// WHAT A LIST HOLDS IS KNOWN ONLY WHERE ITS LITERAL IS WHOLE: every element is written in the literal, and nothing ever changes the list.

// By allocation site. Run once, behind flow and in front of every pass that takes a literal apart: there each completion still stands beside its literal as an operation on it.
BitVector collectSitesOfWholeLiterals(const Program&, const FlowResults&, BuildContext&);

// The value is a whole literal of this function, seen through casts, and holds primitive constants alone: those.
std::optional<Vector<KnownValue, 8>> findConstantsOfWholeLiteral(const Program&, const FlowResults&, const Function&, ValueID);

// The same, also for what was read from a variable of a module that has one such list for good.
std::optional<Vector<KnownValue, 8>> findConstantsOfWholeList(const Program&, const FlowResults&, const Function&, ValueID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
