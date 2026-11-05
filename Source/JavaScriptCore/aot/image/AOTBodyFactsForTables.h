/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTImageForward.h"
#include "AOTVector.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;
class LinkedFunctions;

namespace Typed {
class Plan;
class Program;
}

struct BodyFactsForTables {
    BitVector functionsThatOnlyReturn;
    BitVector functionsWithFrames;
};

BodyFactsForTables collectBodyFactsForTables(const Typed::Program&, const Typed::Plan&, const LinkedFunctions&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
