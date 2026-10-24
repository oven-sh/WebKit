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

BitVector collectSitesOfWholeLiterals(const Program&, const FlowResults&, BuildContext&);

std::optional<Vector<KnownValue, 8>> findConstantsOfWholeLiteral(const Program&, const FlowResults&, const Function&, ValueID);

std::optional<Vector<KnownValue, 8>> findConstantsOfWholeList(const Program&, const FlowResults&, const Function&, ValueID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
