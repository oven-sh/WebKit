/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"

namespace JSC { namespace AOT {

class BuildContext;
class Image;

namespace Typed {
class Heat;
class Placement;
class Program;
class StringTable;
struct PipelineOutput;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
