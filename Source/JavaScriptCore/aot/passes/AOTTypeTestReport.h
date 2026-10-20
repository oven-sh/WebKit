/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTWire.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

Vector<Wire::TypeTestRecord> collectTypeTestRecords(const Program&);
Vector<Wire::TypeTestRecord> collectTypeTestRecords(const Program&, const ParallelLoops&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
